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

/// @file alias.cc
/// @brief Alias analysis: computes a conservative Alias descriptor for each tmp, serving downstream
/// load/mem optimizations.
///
/// See docs/alias.md for architectural notes.

#include "rcc/all.h"

#include <cstring>

namespace rcc {

void getalias(Alias* a, Ref r, Fn* fn) {
    switch (rtype(r)) {
        case RTmp:
            *a = fn->tmp[r.val].alias;
            // astack: follow to root slot to get latest type (slot might have been upgraded to AEsc
            // by esc())
            if (astack(a->type))
                a->type = a->slot->type;
            assert(a->type != Alias::ABot);
            return;
        case RCon: {
            Con* c = &fn->con[r.val];
            if (c->type == Con::CAddr) {
                a->type = Alias::ASym;
                a->u.sym = c->sym;
            } else {
                a->type = Alias::ACon;
            }
            a->offset = c->bits.i;
            a->offset_unknown = false;
            a->slot = nullptr;
            a->base = 0;
            return;
        }
        default:
            die("getalias: unreachable ref kind");
    }
}

int alias(Ref p, int op, int sp, Ref q, int sq, int* delta, Fn* fn) {
    Alias ap, aq;
    getalias(&ap, p, fn);
    getalias(&aq, q, fn);
    ap.offset += op;

    // sp, sq are guaranteed by caller to be <= 2^28, offset difference won't overflow int
    *delta = static_cast<int>(ap.offset - aq.offset);
    bool ovlap = ap.offset < aq.offset + sq && aq.offset < ap.offset + sp;
    // When offset_unknown, overlap is untrustworthy, all "same base + overlap" judgements must
    // degrade
    bool ovlap_known = !ap.offset_unknown && !aq.offset_unknown;

    bool p_stack = astack(ap.type);
    bool q_stack = astack(aq.type);

    if (p_stack && q_stack)
        return (ap.base == aq.base && ovlap) ? MustAlias : NoAlias;

    if (ap.type == Alias::ASym && aq.type == Alias::ASym) {
        if (!symeq(ap.u.sym, aq.u.sym))
            return NoAlias;
        if (!ovlap_known)
            return MayAlias;
        return ovlap ? MustAlias : NoAlias;
    }

    bool both_con = (ap.type == Alias::ACon && aq.type == Alias::ACon);
    bool same_base = (ap.type == aq.type && ap.base == aq.base);
    if (both_con || same_base) {
        assert(ap.type == Alias::ACon || ap.type == Alias::AUnk);
        if (!ovlap_known)
            return MayAlias;
        return ovlap ? MustAlias : NoAlias;
    }

    if (ap.type == Alias::AUnk && aq.type != Alias::ALoc)
        return MayAlias;
    if (aq.type == Alias::AUnk && ap.type != Alias::ALoc)
        return MayAlias;

    return NoAlias;
}

int escapes(Ref r, Fn* fn) {
    if (rtype(r) != RTmp)
        return 1;
    Alias* a = &fn->tmp[r.val].alias;
    // astack and slot is still ALoc -> didn't escape; otherwise (not astack or slot is AEsc) it
    // escapes
    return !astack(a->type) || a->slot->type == Alias::AEsc;
}

namespace {

void esc(Ref r, Fn* fn) {
    assert(rtype(r) <= RType);
    if (rtype(r) != RTmp)
        return;
    Alias* a = &fn->tmp[r.val].alias;
    if (astack(a->type))
        a->slot->type = Alias::AEsc;
}

void store(Ref r, int sz, Fn* fn) {
    if (rtype(r) != RTmp)
        return;
    Alias* a = &fn->tmp[r.val].alias;
    if (!a->slot)
        return;
    assert(astack(a->type));
    int64_t off = a->offset;
    bits m;
    if (sz >= static_cast<int>(NBit) || off < 0 || off >= static_cast<int64_t>(NBit))
        m = static_cast<bits>(-1);
    else
        m = (BIT(sz) - 1) << off;
    a->slot->u.loc.m |= m;
}

bool getalias_safe(Alias* a, Ref r, Fn* fn) {
    switch (rtype(r)) {
        case RTmp:
            *a = fn->tmp[r.val].alias;
            if (a->type == Alias::ABot)
                return false;
            if (astack(a->type))
                a->type = a->slot->type;
            return true;
        case RCon: {
            Con* c = &fn->con[r.val];
            if (c->type == Con::CAddr) {
                a->type = Alias::ASym;
                a->u.sym = c->sym;
            } else {
                a->type = Alias::ACon;
            }
            a->offset = c->bits.i;
            a->offset_unknown = false;
            a->slot = nullptr;
            a->base = 0;
            return true;
        }
        default:
            die("getalias_safe: unreachable ref kind");
    }
}

bool alias_eq(const Alias& a, const Alias& b) {
    if (a.type != b.type)
        return false;
    if (a.offset != b.offset)
        return false;
    if (a.offset_unknown != b.offset_unknown)
        return false;
    if (a.slot != b.slot)
        return false;
    if (a.base != b.base)
        return false;
    if (a.type == Alias::ASym) {
        if (a.u.sym.type != b.u.sym.type)
            return false;
        if (a.u.sym.id != b.u.sym.id)
            return false;
    } else if (astack(a.type)) {
        if (a.u.loc.sz != b.u.loc.sz)
            return false;
        if (a.u.loc.m != b.u.loc.m)
            return false;
    }
    return true;
}

bool alias_meet(Alias* a, const Alias& b) {
    Alias old = *a;

    if (a->type == Alias::ABot) {
        *a = b;
    } else if (b.type == Alias::ABot) {
        // Unchanged
    } else {
        // Same type + same identity: offset compatible
        bool same_id = false;
        if (a->type == b.type) {
            if (astack(a->type))
                same_id = (a->slot == b.slot);
            else if (a->type == Alias::ASym)
                same_id = symeq(a->u.sym, b.u.sym);
            else if (a->type == Alias::ACon)
                same_id = true;  // ACon has no base/sym, purely based on offset
            else if (a->type == Alias::AUnk)
                same_id = (a->base == b.base);
        }
        if (same_id) {
            // ASym path legitimately accepts offset_unknown; ALoc/AEsc do not (store-mask
            // assumes offset is known), degrade to AUnk on offset mismatch.
            bool both_offset_known = !a->offset_unknown && !b.offset_unknown;
            bool offset_match = both_offset_known && a->offset == b.offset;
            if (astack(a->type) && !offset_match) {
                a->type = Alias::AUnk;
                a->slot = nullptr;
                a->offset = 0;
                a->offset_unknown = false;
                // base fixed by caller
            } else {
                if (!offset_match)
                    a->offset_unknown = true;
            }
        } else {
            // Type or identity mismatch -> AUnk
            a->type = Alias::AUnk;
            a->slot = nullptr;
            a->offset = 0;
            a->offset_unknown = false;
            // base fixed by caller
        }
    }

    return !alias_eq(*a, old);
}

bool compute_phi(Phi* p, Fn* fn) {
    assert(rtype(p->to) == RTmp);
    Alias* a = &fn->tmp[p->to.val].alias;
    Alias old = *a;

    Alias acc;
    std::memset(&acc, 0, sizeof acc);
    acc.type = Alias::ABot;

    for (uint k = 0; k < p->narg; k++) {
        Alias arg;
        if (!getalias_safe(&arg, p->arg[k], fn))
            continue;  // This arg is still ABot, skip and wait for next round
        alias_meet(&acc, arg);
    }

    if (acc.type == Alias::ABot) {
        // All args still ABot: retain ABot, doesn't count as change
        return false;
    }

    // If meet degrades to AUnk, set base to phi's own tmp id (uniqueness, avoids sharing base with
    // other AUnks)
    if (acc.type == Alias::AUnk)
        acc.base = p->to.val;

    if (!alias_eq(*a, acc)) {
        *a = acc;
        return true;
    }
    (void)old;
    return false;
}

struct InsResult {
    bool consumed_next;
    bool changed;
};

InsResult process_ins(Ins* i, Ins* end, Fn* fn) {
    InsResult res{false, false};
    Alias* a = nullptr;
    Alias old;
    bool has_def = !req(i->to, Ref::none());

    if (has_def) {
        assert(rtype(i->to) == RTmp);
        a = &fn->tmp[i->to.val].alias;
        old = *a;

        if (isalloc(i->op)) {
            // alloc: install only on first time (ABot), afterwards type might upgrade to AEsc via
            // esc, but slot pointer, loc.sz, loc.m must be preserved (loc.m accumulates with OR
            // across rounds).
            if (a->type == Alias::ABot) {
                a->type = Alias::ALoc;
                a->slot = a;       // Self reference = this tmp is the root slot
                a->u.loc.sz = -1;  // Unknown size placeholder
                a->u.loc.m = 0;
                if (rtype(i->arg[0]) == RCon) {
                    Con* c = &fn->con[i->arg[0].val];
                    int64_t x = c->bits.i;
                    if (c->type == Con::CBits && 0 <= x && x <= static_cast<int64_t>(NBit))
                        a->u.loc.sz = static_cast<int>(x);
                }
                a->base = i->to.val;
                a->offset = 0;
                a->offset_unknown = false;
            }
        } else if (i->op == Ocopy || i->op == Oaddr) {
            // Oaddr passes through alias: IRGen wraps `$sym` as `%t = addr $sym`, here
            // we must propagate ASym identity from RCon CAddr to %t, otherwise alias() sees
            // load/store addr as AUnk Tmp, losing "different globals don't alias".
            Alias src;
            if (getalias_safe(&src, i->arg[0], fn)) {
                *a = src;
            }
            // src still ABot: don't touch *a, wait for next round
        } else if (i->op == Oadd) {
            Alias a0, a1;
            bool ok0 = getalias_safe(&a0, i->arg[0], fn);
            bool ok1 = getalias_safe(&a1, i->arg[1], fn);
            if (ok0 && ok1) {
                Alias newval;
                std::memset(&newval, 0, sizeof newval);
                newval.type = Alias::AUnk;  // Default
                newval.base = i->to.val;

                if (a0.type == Alias::ACon) {
                    newval = a1;
                    newval.offset += a0.offset;
                } else if (a1.type == Alias::ACon) {
                    newval = a0;
                    newval.offset += a1.offset;
                } else if (a0.type == Alias::ASym) {
                    newval = a0;
                    newval.offset_unknown = true;
                } else if (a1.type == Alias::ASym) {
                    newval = a1;
                    newval.offset_unknown = true;
                }
                // Both are neither ACon nor ASym -> keep default AUnk
                *a = newval;
            }
            // Any operand still ABot: don't touch *a, wait for next round
        } else {
            // Other ops: def is always AUnk, one-time init
            if (a->type == Alias::ABot) {
                a->type = Alias::AUnk;
                a->slot = nullptr;
                a->base = i->to.val;
                a->offset = 0;
                a->offset_unknown = false;
            }
        }

        if (!alias_eq(*a, old))
            res.changed = true;
    }

    // 3. esc mark: when no def or def is already AUnk, arg is treated as escaping local
    // abstraction.
    //    Skip on ABot -- def undetermined, decide next round.
    //    Multiple iterations: esc is monotonically accumulated (slot type one-way ALoc->AEsc),
    //    idempotent.
    bool def_known_unk = has_def && a->type == Alias::AUnk;
    if (!has_def || def_known_unk) {
        if (i->op != Oblit0) {
            if (!isload(i->op))
                esc(i->arg[0], fn);
            if (!isstore(i->op) && i->op != Oargc)
                esc(i->arg[1], fn);
        }
    }

    // 4. Memory write accounting: Oblit0/Oblit1 pair / normal store (multiple rounds OR accumulate
    // idempotent)
    if (i->op == Oblit0) {
        Ins* i1 = i + 1;
        assert(i1 < end && i1->op == Oblit1);
        (void)end;
        assert(rtype(i1->arg[0]) == RInt);
        int sz = std::abs(rsval(i1->arg[0]));
        store(i->arg[1], sz, fn);
        res.consumed_next = true;
    }
    if (isstore(i->op))
        store(i->arg[1], storesz(i), fn);

    return res;
}

void escape_phi_args(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        for (Phi* p = b->phi; p; p = p->link)
            for (uint n = 0; n < p->narg; n++)
                esc(p->arg[n], fn);
}

}  // namespace

void fillalias(Fn* fn) {
    for (int t = 0; t < fn->ntmp; t++) {
        std::memset(&fn->tmp[t].alias, 0, sizeof(Alias));
        fn->tmp[t].alias.type = Alias::ABot;
    }

    int iter = 0;
    bool changed = true;
    for (; iter < 16 && changed; ++iter) {
        changed = false;
        for (uint n = 0; n < fn->nblk; ++n) {
            Blk* b = fn->rpo[n];
            for (Phi* p = b->phi; p; p = p->link)
                if (compute_phi(p, fn))
                    changed = true;
            for (Ins* i = b->ins; i < &b->ins[b->nins];) {
                InsResult r = process_ins(i, &b->ins[b->nins], fn);
                if (r.changed)
                    changed = true;
                i += r.consumed_next ? 2 : 1;
            }
            // Block tail jump arg considered escaped (Jretc exception: ret-by-copy pointer doesn't
            // escape). Multiple rounds esc idempotent.
            if (b->jmp.type != Jretc)
                esc(b->jmp.arg, fn);
        }
    }

    for (int t = 0; t < fn->ntmp; t++) {
        Alias* a = &fn->tmp[t].alias;
        if (a->type == Alias::ABot) {
            a->type = Alias::AUnk;
            a->base = t;
            a->offset = 0;
            a->offset_unknown = false;
            a->slot = nullptr;
        }
    }

    escape_phi_args(fn);
}

}  // namespace rcc
