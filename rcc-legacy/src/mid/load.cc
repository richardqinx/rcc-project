/* Source code for RCC-Legacy.
   Copyright (C) 2026 Richard Qin

This file is part of RCC.

RCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

RCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with RCC; see the file LICENSE.  If not see
<http://www.gnu.org/licenses/>.  */

/// @file load.cc
/// @brief Load/store meta information + load-forwarding optimization (loadopt).
///
/// See docs/load.md for architectural notes.

#include "rcc/all.h"

#include <cstdlib>

namespace rcc {

// loadsz / storesz - also used by alias.cc / mem.cc

int loadsz(Ins* l) {
    switch (l->op) {
        case Oloadsb:
        case Oloadub:
            return 1;
        case Oloadsh:
        case Oloaduh:
            return 2;
        case Oloadsw:
        case Oloaduw:
            return 4;
        case Oload:
            return KWIDE(l->cls) ? 8 : 4;
    }
    die("loadsz: not a load op");
}

int storesz(Ins* s) {
    switch (s->op) {
        case Ostoreb:
            return 1;
        case Ostoreh:
            return 2;
        case Ostorew:
        case Ostores:
            return 4;
        case Ostorel:
        case Ostored:
            return 8;
    }
    die("storesz: not a store op");
}

namespace {

struct Slice {
    Ref ref;
    int off;
    short sz;
    short cls;
};

struct Loc {
    enum Type { LRoot, LLoad, LNoLoad } type;
    uint off;
    Blk* blk;
};

}  // namespace

struct Insert {
    uint isphi : 1;
    uint num : 31;
    uint bid;
    uint off;
    Ins ins;        // valid when !isphi
    Slice slice;    // valid when isphi
    Phi* phi_node;  // valid when isphi
};

namespace {

// MASK(w): All-1 mask of width w bytes. Relies on unsigned overflow for w==8.
inline bits mskbits(int w) {
    return (BIT(8 * w - 1) << 1) - 1;
}

// Insert a deferred instruction, returning new tmp
Ref iins(Ctx* ctx, int cls, int op, Ref a0, Ref a1, Loc* l) {
    vgrow(ctx, &ctx->load_ilog, ++ctx->load_nlog);
    Insert* ist = &ctx->load_ilog[ctx->load_nlog - 1];
    ist->isphi = 0;
    ist->num = ctx->load_inum++;
    ist->bid = l->blk->id;
    ist->off = l->off;
    Ref to = newtmp(ctx, "ld", cls, ctx->load_curf);
    ist->ins.op = static_cast<uint>(op);
    ist->ins.cls = static_cast<uint>(cls);
    ist->ins.to = to;
    ist->ins.arg[0] = a0;
    ist->ins.arg[1] = a1;
    return to;
}

// Cast *r to cls, possibly inserting multiple Ocast / Oextuw
void cast_ref(Ctx* ctx, Ref* r, int cls, Loc* l) {
    if (rtype(*r) == RCon)
        return;
    assert(rtype(*r) == RTmp);
    int cls0 = ctx->load_curf->tmp[r->val].cls;
    if (cls0 == cls || (cls == Kw && cls0 == Kl))
        return;
    if (KWIDE(cls0) < KWIDE(cls)) {
        if (cls0 == Ks)
            *r = iins(ctx, Kw, Ocast, *r, Ref::none(), l);
        *r = iins(ctx, Kl, Oextuw, *r, Ref::none(), l);
        if (cls == Kd)
            *r = iins(ctx, Kd, Ocast, *r, Ref::none(), l);
    } else {
        if (cls0 == Kd && cls != Kl)
            *r = iins(ctx, Kl, Ocast, *r, Ref::none(), l);
        if (cls0 != Kd || cls != Kw)
            *r = iins(ctx, cls, Ocast, *r, Ref::none(), l);
    }
}

void mask_ref(Ctx* ctx, int cls, Ref* r, bits msk, Loc* l) {
    cast_ref(ctx, r, cls, l);
    *r = iins(ctx, cls, Oand, *r, getcon(ctx, static_cast<int64_t>(msk), ctx->load_curf), l);
}

// emit_load: Insert an actual load (fallback when forwarding fails)
Ref emit_load(Ctx* ctx, Slice sl, bits msk, Loc* l) {
    int ld;
    switch (sl.sz) {
        case 1:
            ld = Oloadub;
            break;
        case 2:
            ld = Oloaduh;
            break;
        case 4:
            ld = Oloaduw;
            break;
        case 8:
            ld = Oload;
            break;
        default:
            die("loadopt: emit_load on slice size %d", sl.sz);
    }
    bits all_msk = mskbits(sl.sz);
    bool all = (msk == all_msk);
    int cls = all ? sl.cls : (sl.sz > 4 ? Kl : Kw);

    Ref r = sl.ref;
    if (rtype(r) == RTmp) {
        Alias* a = &ctx->load_curf->tmp[r.val].alias;
        // offset_unknown aliases will not match via slice - invariant broken if reached
        assert(!a->offset_unknown);
        switch (a->type) {
            case Alias::ALoc:
            case Alias::AEsc:
            case Alias::AUnk:
                r = Ref::tmp(static_cast<uint>(a->base));
                if (a->offset) {
                    Ref r1 = getcon(ctx, a->offset, ctx->load_curf);
                    r = iins(ctx, Kl, Oadd, r, r1, l);
                }
                break;
            case Alias::ACon:
            case Alias::ASym: {
                Con c;
                memset(&c, 0, sizeof c);
                c.type = Con::CAddr;
                c.sym = a->u.sym;
                c.bits.i = a->offset;
                r = newcon(ctx, &c, ctx->load_curf);
                break;
            }
            default:
                die("loadopt: emit_load on alias type %d", a->type);
        }
    }
    r = iins(ctx, cls, ld, r, Ref::none(), l);
    if (!all)
        mask_ref(ctx, cls, &r, msk, l);
    return r;
}

// rebase: Replace sl.ref with alias base + off
void rebase(Ctx* ctx, Slice* sl) {
    if (rtype(sl->ref) != RTmp)
        return;
    Alias* a = &ctx->load_curf->tmp[sl->ref.val].alias;
    // offset_unknown aliases cannot be rebased
    if (a->offset_unknown)
        return;
    if (static_cast<int16_t>(a->offset) != a->offset)
        return;
    if (a->type != Alias::ALoc && a->type != Alias::AEsc && a->type != Alias::AUnk)
        return;
    sl->ref = Ref::tmp(static_cast<uint>(a->base));
    sl->off = static_cast<int>(a->offset);
}

// killsl: whether r is sl's base tmp (only valid for stack type)
bool killsl(Ctx* ctx, Ref r, Slice sl) {
    if (rtype(sl.ref) != RTmp)
        return false;
    Alias* a = &ctx->load_curf->tmp[sl.ref.val].alias;
    switch (a->type) {
        case Alias::ALoc:
        case Alias::AEsc:
        case Alias::AUnk:
            return req(Ref::tmp(static_cast<uint>(a->base)), r);
        case Alias::ACon:
        case Alias::ASym:
            return false;
        default:
            die("loadopt: killsl on alias type %d", a->type);
    }
}

// qsort comparator: bid asc -> phi before ins -> off asc -> num asc
int icmp(const void* pa, const void* pb) {
    const Insert* a = static_cast<const Insert*>(pa);
    const Insert* b = static_cast<const Insert*>(pb);
    int c;
    if ((c = static_cast<int>(a->bid) - static_cast<int>(b->bid)))
        return c;
    if (a->isphi && b->isphi)
        return 0;
    if (a->isphi)
        return -1;
    if (b->isphi)
        return 1;
    if ((c = static_cast<int>(a->off) - static_cast<int>(b->off)))
        return c;
    return static_cast<int>(a->num) - static_cast<int>(b->num);
}

Ref def(Ctx* ctx, Slice sl, bits msk, Blk* b, Ins* i, Loc* il);

Ref try_def(Ctx* ctx, Slice sl, bits msk, Blk* b, Ins* i, Loc* il) {
    // CFG traversal invariants
    assert(dom(b, il->blk));

    if (!i)
        i = &b->ins[b->nins];
    int cls = (sl.sz > 4) ? Kl : Kw;
    bits msks = mskbits(sl.sz);

    // Scan backward in block
    while (i > b->ins) {
        --i;
        if (killsl(ctx, i->to, sl))
            return Ref::none();
        if (i->op == Ocall && escapes(sl.ref, ctx->load_curf))
            return Ref::none();

        bool is_load = isload(i->op);
        int sz;
        Ref r1, r;
        if (is_load) {
            sz = loadsz(i);
            r1 = i->arg[0];
            r = i->to;
        } else if (isstore(i->op)) {
            sz = storesz(i);
            r1 = i->arg[1];
            r = i->arg[0];
        } else if (i->op == Oblit1) {
            assert(rtype(i->arg[0]) == RInt);
            sz = std::abs(rsval(i->arg[0]));
            assert(i > b->ins);
            --i;
            assert(i->op == Oblit0);
            r1 = i->arg[1];
            r = Ref::none();  // blit does not use r
        } else {
            continue;
        }

        int off;
        switch (alias(sl.ref, sl.off, sl.sz, r1, sz, &off, ctx->load_curf)) {
            case MustAlias: {
                Slice sl1 = sl;
                if (i->op == Oblit0) {
                    // adjust blit source slice
                    sl1.ref = i->arg[0];
                    if (off >= 0) {
                        assert(off < sz);
                        sl1.off = off;
                        sz -= off;
                        off = 0;
                    } else {
                        sl1.off = 0;
                        sl1.sz = static_cast<short>(sl1.sz + off);
                    }
                    if (sz > sl1.sz)
                        sz = sl1.sz;
                    assert(sz <= 8);
                    sl1.sz = static_cast<short>(sz);
                }

                bits msk1;
                int op;
                if (off < 0) {
                    off = -off;
                    msk1 = (mskbits(sz) << (8 * off)) & msks;
                    op = Oshl;
                } else {
                    msk1 = (mskbits(sz) >> (8 * off)) & msks;
                    op = Oshr;
                }
                if ((msk1 & msk) == 0)
                    continue;

                if (i->op == Oblit0) {
                    r = def(ctx, sl1, mskbits(static_cast<int>(sl1.sz)), b, i, il);
                    if (req(r, Ref::none()))
                        return Ref::none();
                }

                if (off) {
                    int cls1 = cls;
                    if (op == Oshr && off + sl.sz > 4)
                        cls1 = Kl;
                    cast_ref(ctx, &r, cls1, il);
                    Ref r2 = getcon(ctx, 8 * off, ctx->load_curf);
                    r = iins(ctx, cls1, op, r, r2, il);
                }
                if ((msk1 & msk) != msk1 || off + sz < sl.sz)
                    mask_ref(ctx, cls, &r, msk1 & msk, il);
                if ((msk & ~msk1) != 0) {
                    Ref r2 = def(ctx, sl, msk & ~msk1, b, i, il);
                    if (req(r2, Ref::none()))
                        return Ref::none();
                    r = iins(ctx, cls, Oor, r, r2, il);
                }
                if (msk == msks)
                    cast_ref(ctx, &r, sl.cls, il);
                return r;
            }
            case MayAlias:
                if (is_load)
                    continue;
                return Ref::none();
            case NoAlias:
                continue;
            default:
                die("loadopt: alias() returned unexpected value");
        }
    }

    // Block head: check if ilog already has a phi for this block and slice
    for (Insert* ist = ctx->load_ilog; ist < &ctx->load_ilog[ctx->load_nlog]; ++ist) {
        if (!ist->isphi || ist->bid != b->id)
            continue;
        if (!req(ist->slice.ref, sl.ref))
            continue;
        if (ist->slice.off != sl.off)
            continue;
        if (ist->slice.sz != sl.sz)
            continue;
        Ref r = ist->phi_node->to;
        if (msk != msks)
            mask_ref(ctx, cls, &r, msk, il);
        else
            cast_ref(ctx, &r, sl.cls, il);
        return r;
    }

    // Check if real phi kills sl
    for (Phi* p = b->phi; p; p = p->link)
        if (killsl(ctx, p->to, sl))
            return Ref::none();

    // Predecessors
    if (b->npred == 0)
        return Ref::none();
    if (b->npred == 1) {
        Blk* bp = b->pred[0];
        assert(bp->loop >= il->blk->loop);
        Loc l = *il;
        if (bp->s2)
            l.type = Loc::LNoLoad;
        Ref r1 = def(ctx, sl, msk, bp, nullptr, &l);
        if (req(r1, Ref::none()))
            return Ref::none();
        return r1;
    }

    // Multiple predecessors: insert new phi into log, recurse on predecessors
    Ref r = newtmp(ctx, "ld", sl.cls, ctx->load_curf);
    Phi* p = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
    memset(p, 0, sizeof *p);
    vgrow(ctx, &ctx->load_ilog, ++ctx->load_nlog);
    Insert* ist = &ctx->load_ilog[ctx->load_nlog - 1];
    ist->isphi = 1;
    ist->bid = b->id;
    ist->slice = sl;
    ist->phi_node = p;
    p->to = r;
    p->cls = static_cast<short>(sl.cls);
    p->narg = b->npred;
    p->arg = static_cast<Ref*>(vnew(ctx, p->narg, sizeof(Ref), PFn));
    p->blk = static_cast<Blk**>(vnew(ctx, p->narg, sizeof(Blk*), PFn));
    for (uint np = 0; np < b->npred; ++np) {
        Blk* bp = b->pred[np];
        Loc l;
        if (!bp->s2 && il->type != Loc::LNoLoad && bp->loop < il->blk->loop)
            l.type = Loc::LLoad;
        else
            l.type = Loc::LNoLoad;
        l.blk = bp;
        l.off = bp->nins;
        Ref r1 = def(ctx, sl, msks, bp, nullptr, &l);
        if (req(r1, Ref::none()))
            return Ref::none();
        p->arg[np] = r1;
        p->blk[np] = bp;
        // XXX QBE note "multiplicity in predecessors" - same bp in b->pred multiple times not
        // handled.
    }
    if (msk != msks)
        mask_ref(ctx, cls, &r, msk, il);
    return r;
}

Ref def(Ctx* ctx, Slice sl, bits msk, Blk* b, Ins* i, Loc* il) {
    uint oldl = ctx->load_nlog;
    int oldt = ctx->load_curf->ntmp;
    Ref r = try_def(ctx, sl, msk, b, i, il);
    if (!req(r, Ref::none()))
        return r;
    // Rollback log and fresh tmp (vgrow does not shrink, capacity kept for reuse)
    ctx->load_nlog = oldl;
    ctx->load_curf->ntmp = oldt;
    if (il->type != Loc::LLoad)
        return Ref::none();
    return emit_load(ctx, sl, msk, il);
}

}  // namespace

void loadopt(Ctx* ctx, Fn* fn) {
    ctx->load_curf = fn;
    ctx->load_ilog = static_cast<Insert*>(vnew(ctx, 0, sizeof(Insert), PHeap));
    ctx->load_nlog = 0;
    ctx->load_inum = 0;

    // solve each load
    for (Blk* b = fn->start; b; b = b->link) {
        for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i) {
            if (!isload(i->op))
                continue;
            int sz = loadsz(i);
            Slice sl;
            sl.ref = i->arg[0];
            sl.off = 0;
            sl.sz = static_cast<short>(sz);
            sl.cls = static_cast<short>(i->cls);
            Loc l;
            l.type = Loc::LRoot;
            l.off = static_cast<uint>(i - b->ins);
            l.blk = b;
            rebase(ctx, &sl);
            i->arg[1] = def(ctx, sl, mskbits(sz), b, i, &l);
        }
    }

    // sort and rebuild each block
    qsort(ctx->load_ilog, ctx->load_nlog, sizeof(Insert), icmp);
    vgrow(ctx, &ctx->load_ilog, ctx->load_nlog + 1);
    ctx->load_ilog[ctx->load_nlog].bid = fn->nblk;  // sentinel: stops ist->bid == n check

    Ins* ib = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PHeap));
    Insert* ist = ctx->load_ilog;
    for (uint n = 0; n < fn->nblk; ++n) {
        Blk* b = fn->rpo[n];

        // Push block's phi inserts to b->phi head
        for (; ist->bid == n && ist->isphi; ++ist) {
            ist->phi_node->link = b->phi;
            b->phi = ist->phi_node;
        }

        uint ni = 0;
        uint nt = 0;
        for (;;) {
            Ins* i;
            if (ist->bid == n && ist->off == ni) {
                i = &ist->ins;
                ++ist;
            } else {
                if (ni == b->nins)
                    break;
                i = &b->ins[ni++];
                if (isload(i->op) && !req(i->arg[1], Ref::none())) {
                    // Solved load -> rewrite op + arg
                    int ext = Oextsb + (static_cast<int>(i->op) - Oloadsb);
                    switch (i->op) {
                        case Oloadsb:
                        case Oloadub:
                        case Oloadsh:
                        case Oloaduh:
                            i->op = static_cast<uint>(ext);
                            break;
                        case Oloadsw:
                        case Oloaduw:
                            if (i->cls == Kl) {
                                i->op = static_cast<uint>(ext);
                                break;
                            }
                            // fall through: under Kw sub-word is equivalent to full word -> direct
                            // copy
                            [[fallthrough]];
                        case Oload:
                            i->op = Ocopy;
                            break;
                        default:
                            die("loadopt: unreachable in solved-load rewrite");
                    }
                    i->arg[0] = i->arg[1];
                    i->arg[1] = Ref::none();
                }
            }
            vgrow(ctx, &ib, ++nt);
            ib[nt - 1] = *i;
        }
        idup(ctx, b, ib, nt);
    }
    vfree(ib);
    vfree(ctx->load_ilog);
    ctx->load_ilog = nullptr;

    if (debug['M']) {
        fprintf(ctx->dbg_buf, "\n> After load elimination:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
