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

/// @file abi.cc
/// @brief RISC-V lp64d ABI lowering.

#include "rcc/rv64.h"

namespace rcc {

namespace {

// ABI classification flags (OR-able).
enum : int {
    kCptr = 1,   // Pointer to large struct.
    kCstk1 = 2,  // First XLEN on stack.
    kCstk2 = 4,  // Second XLEN on stack.
    kCstk = kCstk1 | kCstk2,
    kCfpint = 8,  // Float in GP reg.
};

// Argument class.
struct ArgClass {
    int flags;  // OR of kCptr / kCstk1 / kCstk2 / kCfpint
    Typ* type;
    int reg[2];
    int cls[2];
    int off[2];
    int ngp;  // Written by typclass
    int nfp;
    int nreg;
};

struct Insl {
    Insl* link;
    Ins i;
};

struct Params {
    int ngp;
    int nfp;
    int stk;
};

// Registers. Sentinels of 0 are used to prevent out-of-bounds reads.
constexpr int kGpReg[10] = {A0, A1, A2, A3, A4, A5, A6, A7, 0, 0};
constexpr int kFpReg[10] = {FA0, FA1, FA2, FA3, FA4, FA5, FA6, FA7, 0, 0};

}  // namespace

// RCall encoding.

bits rv64_retregs(Ref r, int p[2]) {
    assert(rtype(r) == RCall);
    int ngp = static_cast<int>(r.val & 3);
    int nfp = static_cast<int>((r.val >> 2) & 3);
    if (p) {
        p[0] = ngp;
        p[1] = nfp;
    }
    bits b = 0;
    for (int i = 0; i < ngp; ++i)
        b |= BIT(A0 + i);
    for (int i = 0; i < nfp; ++i)
        b |= BIT(FA0 + i);
    return b;
}

bits rv64_argregs(Ref r, int p[2]) {
    assert(rtype(r) == RCall);
    int ngp = static_cast<int>((r.val >> 4) & 15);
    int nfp = static_cast<int>((r.val >> 8) & 15);
    int t5 = static_cast<int>((r.val >> 12) & 1);
    if (p) {
        p[0] = ngp + t5;
        p[1] = nfp;
    }
    bits b = 0;
    for (int i = 0; i < ngp; ++i)
        b |= BIT(A0 + i);
    for (int i = 0; i < nfp; ++i)
        b |= BIT(FA0 + i);
    if (t5)
        b |= BIT(T5);
    return b;
}

namespace {

// HFA-style floating-point struct classification.

int classify_fpstruct(Typ* t, int off, ArgClass* c) {
    if (t->isunion)
        return -1;

    for (Field* f = *t->fields; f->type != Field::FEnd; ++f) {
        if (f->type == Field::FPad) {
            off += static_cast<int>(f->len);
            continue;
        }
        if (f->type == Field::FTyp) {
            if (classify_fpstruct(&typ[f->len], off, c) == -1)
                return -1;
            continue;
        }

        int slot = c->nfp + c->ngp;
        if (slot == 2)
            return -1;

        switch (f->type) {
            case Field::Fb:
            case Field::Fh:
            case Field::Fw:
                c->cls[slot] = Kw;
                ++c->ngp;
                break;
            case Field::Fl:
                c->cls[slot] = Kl;
                ++c->ngp;
                break;
            case Field::Fs:
                c->cls[slot] = Ks;
                ++c->nfp;
                break;
            case Field::Fd:
                c->cls[slot] = Kd;
                ++c->nfp;
                break;
            default:
                die("classify_fpstruct: unexpected field type");
        }
        c->off[slot] = off;
        off += static_cast<int>(f->len);
    }
    return c->nfp;
}

// Determine ABI class for struct t.
void typclass(ArgClass* c, Typ* t, int fpabi, const int* gp, const int* fp) {
    c->type = t;
    c->flags = 0;
    c->ngp = 0;
    c->nfp = 0;

    if (t->align > 4)
        err(const_cast<char*>("alignments larger than 16 are not supported"));

    if (t->isdark || t->size > 16 || t->size == 0) {
        // Large or dark: pass by pointer.
        c->flags |= kCptr;
        c->cls[0] = Kl;
        c->off[0] = 0;
        c->ngp = 1;
    } else if (!fpabi || classify_fpstruct(t, 0, c) <= 0) {
        // GP-only word packing.
        uint n = 0;
        while (8 * n < t->size) {
            c->cls[n] = Kl;
            c->off[n] = static_cast<int>(8 * n);
            ++n;
        }
        c->nfp = 0;
        c->ngp = static_cast<int>(n);
    }

    c->nreg = c->nfp + c->ngp;
    for (int i = 0; i < c->nreg; ++i) {
        if (KBASE(c->cls[i]) == 0)
            c->reg[i] = *gp++;
        else
            c->reg[i] = *fp++;
    }
}

// Store tmp to mem+off.
void sttmps(Ctx* ctx, Ref tmp[], int ntmp, ArgClass* c, Ref mem, Fn* fn) {
    static constexpr auto kStOp = [] {
        std::array<int, 4> a{};
        a[Kw] = Ostorew;
        a[Kl] = Ostorel;
        a[Ks] = Ostores;
        a[Kd] = Ostored;
        return a;
    }();
    assert(ntmp > 0 && ntmp <= 2);
    for (int i = 0; i < ntmp; ++i) {
        tmp[i] = newtmp(ctx, "abi", c->cls[i], fn);
        Ref addr = newtmp(ctx, "abi", Kl, fn);
        emit(ctx, kStOp[static_cast<size_t>(c->cls[i])], 0, Ref::none(), tmp[i], addr);
        emit(ctx, Oadd, Kl, addr, mem, getcon(ctx, c->off[i], fn));
    }
}

// Load mem+off to regs.
void ldregs(Ctx* ctx, ArgClass* c, Ref mem, Fn* fn) {
    for (int i = 0; i < c->nreg; ++i) {
        Ref addr = newtmp(ctx, "abi", Kl, fn);
        emit(ctx, Oload, c->cls[i], Ref::tmp(static_cast<uint>(c->reg[i])), addr, Ref::none());
        emit(ctx, Oadd, Kl, addr, mem, getcon(ctx, c->off[i], fn));
    }
}

// Return value lowering.

void selret(Ctx* ctx, Blk* b, Fn* fn) {
    int j = b->jmp.type;
    if (!isret(j) || j == Jret0)
        return;

    Ref r = b->jmp.arg;
    b->jmp.type = Jret0;

    int cty;
    if (j == Jretc) {
        ArgClass cr{};
        typclass(&cr, &typ[fn->retty], 1, kGpReg, kFpReg);
        if (cr.flags & kCptr) {
            assert(rtype(fn->retr) == RTmp);
            emit(ctx, Oblit1, 0, Ref::none(), Ref::integer(static_cast<int>(cr.type->size)),
                 Ref::none());
            emit(ctx, Oblit0, 0, Ref::none(), r, fn->retr);
            cty = 0;
        } else {
            ldregs(ctx, &cr, r, fn);
            cty = (cr.nfp << 2) | cr.ngp;
        }
    } else {
        int k = j - Jretw;
        if (KBASE(k) == 0) {
            emit(ctx, Ocopy, k, Ref::tmp(A0), r, Ref::none());
            cty = 1;
        } else {
            emit(ctx, Ocopy, k, Ref::tmp(FA0), r, Ref::none());
            cty = 1 << 2;
        }
    }

    b->jmp.arg = Ref::call(static_cast<uint>(cty));
}

// Arg/par classification.

int argsclass(Ins* i0, Ins* i1, ArgClass* carg, int retptr) {
    const int* gp = kGpReg;
    const int* fp = kFpReg;
    int ngp = 8, nfp = 8;
    bool vararg = false, envc = false;

    if (retptr) {
        ++gp;
        --ngp;
    }

    ArgClass* c = carg;
    for (Ins* i = i0; i < i1; ++i, ++c) {
        switch (i->op) {
            case Opar:
            case Oarg:
                c->cls[0] = static_cast<int>(i->cls);
                if (!vararg && KBASE(i->cls) == 1 && nfp > 0) {
                    --nfp;
                    c->reg[0] = *fp++;
                } else if (ngp > 0) {
                    if (KBASE(i->cls) == 1)
                        c->flags |= kCfpint;
                    --ngp;
                    c->reg[0] = *gp++;
                } else {
                    c->flags |= kCstk1;
                }
                break;

            case Oargv:
                vararg = true;
                break;

            case Oparc:
            case Oargc: {
                Typ* t = &typ[i->arg[0].val];
                typclass(c, t, 1, gp, fp);
                if (c->nfp > 0 && (c->nfp >= nfp || c->ngp >= ngp)) {
                    // Downgrade to GP-only.
                    typclass(c, t, 0, gp, fp);
                }
                assert(c->nfp <= nfp);
                if (c->ngp <= ngp) {
                    ngp -= c->ngp;
                    nfp -= c->nfp;
                    gp += c->ngp;
                    fp += c->nfp;
                } else if (ngp > 0) {
                    // First word in GP, second on stack.
                    assert(c->ngp == 2);
                    assert(c->flags == 0);
                    c->flags |= kCstk2;
                    c->nreg = 1;
                    --ngp;
                    ++gp;
                } else {
                    c->flags |= kCstk1;
                    if (c->nreg > 1)
                        c->flags |= kCstk2;
                    c->nreg = 0;
                }
                break;
            }

            case Opare:
            case Oarge:
                c->reg[0] = T5;
                c->cls[0] = Kl;
                envc = true;
                break;
        }
    }

    int passgp = static_cast<int>(gp - kGpReg);
    int passfp = static_cast<int>(fp - kFpReg);
    return ((envc ? 1 : 0) << 12) | (passgp << 4) | (passfp << 8);
}

// Prepend Oalloc to start block.
void stkblob(Ctx* ctx, Ref r, Typ* t, Fn* fn, Insl** ilp) {
    Insl* il = static_cast<Insl*>(alloc(ctx, sizeof(*il)));
    int al = t->align - 2;  // alignment logic.
    if (al < 0)
        al = 0;
    uint64_t sz = (t->size + 7) & ~static_cast<uint64_t>(7);

    Ins ins = {};
    ins.op = static_cast<uint>(Oalloc + al);
    ins.cls = Kl;
    ins.to = r;
    ins.arg[0] = getcon(ctx, static_cast<int64_t>(sz), fn);
    il->i = ins;
    il->link = *ilp;
    *ilp = il;
}

// Call lowering.

void selcall(Ctx* ctx, Fn* fn, Ins* i0, Ins* i1, Insl** ilp) {
    ArgClass* ca =
        static_cast<ArgClass*>(alloc(ctx, static_cast<size_t>(i1 - i0) * sizeof(ArgClass)));
    for (Ins* z = i0; z < i1; ++z)
        ca[z - i0] = {};
    ArgClass cr{};

    if (!req(i1->arg[1], Ref::none())) {
        typclass(&cr, &typ[i1->arg[1].val], 1, kGpReg, kFpReg);
    }

    int cty = argsclass(i0, i1, ca, cr.flags & kCptr);

    // compute stk and replace large struct args with pointer.
    uint64_t stk = 0;
    {
        ArgClass* c = ca;
        for (Ins* i = i0; i < i1; ++i, ++c) {
            if (i->op == Oargv)
                continue;
            if (c->flags & kCptr) {
                i->arg[0] = newtmp(ctx, "abi", Kl, fn);
                stkblob(ctx, i->arg[0], c->type, fn, ilp);
                i->op = Oarg;
            }
            if (c->flags & kCstk1)
                stk += 8;
            if (c->flags & kCstk2)
                stk += 8;
        }
        stk += stk & 15;  // 16B align tail
    }
    if (stk)
        emit(ctx, Osalloc, Kl, Ref::none(), getcon(ctx, -static_cast<int64_t>(stk), fn),
             Ref::none());

    // Emit instructions backwards.
    Ref tmp[2];
    if (!req(i1->arg[1], Ref::none())) {
        stkblob(ctx, i1->to, cr.type, fn, ilp);
        cty |= (cr.nfp << 2) | cr.ngp;
        if (cr.flags & kCptr) {
            // Dummy Ocopy.
            emit(ctx, Ocopy, Kw, Ref::none(), Ref::tmp(A0), Ref::none());
        } else {
            sttmps(ctx, tmp, cr.nreg, &cr, i1->to, fn);
            for (int j = 0; j < cr.nreg; ++j) {
                Ref rreg = Ref::tmp(static_cast<uint>(cr.reg[j]));
                emit(ctx, Ocopy, cr.cls[j], tmp[j], rreg, Ref::none());
            }
        }
    } else if (KBASE(i1->cls) == 0) {
        emit(ctx, Ocopy, i1->cls, i1->to, Ref::tmp(A0), Ref::none());
        cty |= 1;
    } else {
        emit(ctx, Ocopy, i1->cls, i1->to, Ref::tmp(FA0), Ref::none());
        cty |= 1 << 2;
    }

    emit(ctx, Ocall, 0, Ref::none(), i1->arg[0], Ref::call(static_cast<uint>(cty)));

    if (cr.flags & kCptr) {
        // Cptr return logic.
        emit(ctx, Ocopy, Kl, Ref::tmp(A0), i1->to, Ref::none());
    }

    // move args to regs.
    {
        ArgClass* c = ca;
        for (Ins* i = i0; i < i1; ++i, ++c) {
            if (i->op == Oargv || (c->flags & kCstk1))
                continue;
            if (i->op == Oargc) {
                ldregs(ctx, c, i->arg[1], fn);
            } else if (c->flags & kCfpint) {
                int k = KWIDE(c->cls[0]) ? Kl : Kw;
                Ref r = newtmp(ctx, "abi", k, fn);
                emit(ctx, Ocopy, k, Ref::tmp(static_cast<uint>(c->reg[0])), r, Ref::none());
                c->reg[0] = static_cast<int>(r.val);
            } else {
                emit(ctx, Ocopy, c->cls[0], Ref::tmp(static_cast<uint>(c->reg[0])), i->arg[0],
                     Ref::none());
            }
        }
    }

    // casts and blits.
    {
        ArgClass* c = ca;
        for (Ins* i = i0; i < i1; ++i, ++c) {
            if (c->flags & kCfpint) {
                int k = KWIDE(c->cls[0]) ? Kl : Kw;
                emit(ctx, Ocast, k, Ref::tmp(static_cast<uint>(c->reg[0])), i->arg[0], Ref::none());
            }
            if (c->flags & kCptr) {
                emit(ctx, Oblit1, 0, Ref::none(), Ref::integer(static_cast<int>(c->type->size)),
                     Ref::none());
                emit(ctx, Oblit0, 0, Ref::none(), i->arg[1], i->arg[0]);
            }
        }
    }

    if (!stk)
        return;

    // move stack args to slot.
    uint64_t off = 0;
    Ref sp = newtmp(ctx, "abi", Kl, fn);
    {
        ArgClass* c = ca;
        for (Ins* i = i0; i < i1; ++i, ++c) {
            if (i->op == Oargv || !(c->flags & kCstk))
                continue;
            if (i->op == Oarg) {
                Ref addr = newtmp(ctx, "abi", Kl, fn);
                emit(ctx, Ostorew + i->cls, Kw, Ref::none(), i->arg[0], addr);
                if (i->cls == Kw) {
                    // TODO: check if sext is needed.
                    ctx->curi->op = Ostorel;
                    ctx->curi->arg[0] = newtmp(ctx, "abi", Kl, fn);
                    emit(ctx, Oextsw, Kl, ctx->curi->arg[0], i->arg[0], Ref::none());
                }
                emit(ctx, Oadd, Kl, addr, sp, getcon(ctx, static_cast<int64_t>(off), fn));
                off += 8;
            }
            if (i->op == Oargc) {
                if (c->flags & kCstk1) {
                    Ref addr = newtmp(ctx, "abi", Kl, fn);
                    Ref val = newtmp(ctx, "abi", Kl, fn);
                    emit(ctx, Ostorel, 0, Ref::none(), val, addr);
                    emit(ctx, Oadd, Kl, addr, sp, getcon(ctx, static_cast<int64_t>(off), fn));
                    emit(ctx, Oload, Kl, val, i->arg[1], Ref::none());
                    off += 8;
                }
                if (c->flags & kCstk2) {
                    Ref addr = newtmp(ctx, "abi", Kl, fn);
                    Ref val = newtmp(ctx, "abi", Kl, fn);
                    emit(ctx, Ostorel, 0, Ref::none(), val, addr);
                    emit(ctx, Oadd, Kl, addr, sp, getcon(ctx, static_cast<int64_t>(off), fn));
                    Ref addr2 = newtmp(ctx, "abi", Kl, fn);
                    emit(ctx, Oload, Kl, val, addr2, Ref::none());
                    emit(ctx, Oadd, Kl, addr2, i->arg[1], getcon(ctx, 8, fn));
                    off += 8;
                }
            }
        }
    }
    emit(ctx, Osalloc, Kl, sp, getcon(ctx, static_cast<int64_t>(stk), fn), Ref::none());
}

// Par lowering.

Params selpar(Ctx* ctx, Fn* fn, Ins* i0, Ins* i1) {
    ArgClass* ca =
        static_cast<ArgClass*>(alloc(ctx, static_cast<size_t>(i1 - i0) * sizeof(ArgClass)));
    for (Ins* z = i0; z < i1; ++z)
        ca[z - i0] = {};
    ArgClass cr{};
    ctx->curi = &ctx->insb[NIns];

    if (fn->retty >= 0) {
        typclass(&cr, &typ[fn->retty], 1, kGpReg, kFpReg);
        if (cr.flags & kCptr) {
            fn->retr = newtmp(ctx, "abi", Kl, fn);
            emit(ctx, Ocopy, Kl, fn->retr, Ref::tmp(A0), Ref::none());
        }
    }

    int cty = argsclass(i0, i1, ca, cr.flags & kCptr);
    fn->reg = rv64_argregs(Ref::call(static_cast<uint>(cty)), nullptr);

    // slot allocations and Ocast.
    Insl* il = nullptr;
    Ref tmp[17];
    Ref* t = tmp;
    {
        ArgClass* c = ca;
        for (Ins* i = i0; i < i1; ++i, ++c) {
            if (c->flags & kCfpint) {
                Ref to_orig = i->to;
                int k = c->cls[0];
                c->cls[0] = KWIDE(k) ? Kl : Kw;
                i->to = newtmp(ctx, "abi", k, fn);
                emit(ctx, Ocast, k, to_orig, i->to, Ref::none());
            }
            if (i->op == Oparc && !(c->flags & kCptr) && c->nreg != 0) {
                int nt = c->nreg;
                if (c->flags & kCstk2) {
                    c->cls[1] = Kl;
                    c->off[1] = 8;
                    assert(nt == 1);
                    nt = 2;
                }
                sttmps(ctx, t, nt, c, i->to, fn);
                stkblob(ctx, i->to, c->type, fn, &il);
                t += nt;
            }
        }
    }
    for (; il; il = il->link)
        emiti(ctx, il->i);

    // copy from regs or load from slot.
    t = tmp;
    int s = 2 + 8 * fn->vararg;
    {
        ArgClass* c = ca;
        for (Ins* i = i0; i < i1; ++i, ++c) {
            if (i->op == Oparc && !(c->flags & kCptr)) {
                if (c->nreg == 0) {
                    fn->tmp[i->to.val].slot = -s;
                    s += (c->flags & kCstk2) ? 2 : 1;
                    continue;
                }
                for (int j = 0; j < c->nreg; ++j) {
                    Ref rreg = Ref::tmp(static_cast<uint>(c->reg[j]));
                    emit(ctx, Ocopy, c->cls[j], *t++, rreg, Ref::none());
                }
                if (c->flags & kCstk2) {
                    emit(ctx, Oload, Kl, *t, Ref::slot(-s), Ref::none());
                    ++t;
                    ++s;
                }
            } else if (c->flags & kCstk1) {
                emit(ctx, Oload, c->cls[0], i->to, Ref::slot(-s), Ref::none());
                ++s;
            } else {
                emit(ctx, Ocopy, c->cls[0], i->to, Ref::tmp(static_cast<uint>(c->reg[0])),
                     Ref::none());
            }
        }
    }

    Params p;
    p.stk = s;
    p.ngp = (cty >> 4) & 15;
    p.nfp = (cty >> 8) & 15;
    return p;
}

// Varargs.

void selvaarg(Ctx* ctx, Fn* fn, Ins* i) {
    Ref loc = newtmp(ctx, "abi", Kl, fn);
    Ref newloc = newtmp(ctx, "abi", Kl, fn);
    emit(ctx, Ostorel, Kw, Ref::none(), newloc, i->arg[0]);
    emit(ctx, Oadd, Kl, newloc, loc, getcon(ctx, 8, fn));
    emit(ctx, Oload, i->cls, i->to, loc, Ref::none());
    emit(ctx, Oload, Kl, loc, i->arg[0], Ref::none());
}

void selvastart(Ctx* ctx, Fn* fn, Params p, Ref ap) {
    Ref rsave = newtmp(ctx, "abi", Kl, fn);
    emit(ctx, Ostorel, Kw, Ref::none(), rsave, ap);
    int s = p.stk > 2 + 8 * fn->vararg ? p.stk : 2 + p.ngp;
    emit(ctx, Oaddr, Kl, rsave, Ref::slot(-s), Ref::none());
}

}  // namespace

// Entry point.

void rv64_abi(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = 0;

    // Lower parameters.
    Blk* b = fn->start;
    Ins* end = &b->ins[b->nins];
    Ins* i = b->ins;
    while (i < end && ispar(i->op))
        ++i;
    Params p = selpar(ctx, fn, b->ins, i);

    int n0 = static_cast<int>(&ctx->insb[NIns] - ctx->curi);
    int ioff = static_cast<int>(i - b->ins);
    int n1 = static_cast<int>(b->nins) - ioff;
    vgrow(ctx, &b->ins, static_cast<ulong>(n0 + n1));
    icpy(b->ins + n0, b->ins + ioff, static_cast<ulong>(n1));
    icpy(b->ins, ctx->curi, static_cast<ulong>(n0));
    b->nins = static_cast<uint>(n0 + n1);

    // Lower calls/returns/varargs.
    Insl* il = nullptr;
    b = fn->start;
    do {
        b = b->link ? b->link : fn->start;
        if (b->visit)
            continue;
        ctx->curi = &ctx->insb[NIns];
        selret(ctx, b, fn);

        for (Ins* it = &b->ins[b->nins]; it != b->ins;) {
            --it;
            switch (it->op) {
                default:
                    emiti(ctx, *it);
                    break;
                case Ocall: {
                    Ins* lo = it;
                    while (lo > b->ins && isarg((lo - 1)->op))
                        --lo;
                    selcall(ctx, fn, lo, it, &il);
                    it = lo;
                    break;
                }
                case Ovastart:
                    selvastart(ctx, fn, p, it->arg[0]);
                    break;
                case Ovaarg:
                    selvaarg(ctx, fn, it);
                    break;
                case Oarg:
                case Oargc:
                    die("rv64_abi: Oarg/Oargc unreachable");
            }
        }

        if (b == fn->start) {
            for (; il; il = il->link)
                emiti(ctx, il->i);
        }
        idup(ctx, b, ctx->curi, static_cast<ulong>(&ctx->insb[NIns] - ctx->curi));
    } while (b != fn->start);

    if (debug['A']) {
        fprintf(ctx->dbg_buf, "\n> After ABI lowering:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
