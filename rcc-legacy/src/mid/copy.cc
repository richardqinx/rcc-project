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

/// @file copy.cc
/// @brief copy equivalence check + narrow parameter sinking - for GVN, no independent driver.
///
/// See docs/copy.md for architectural notes.

#include "rcc/all.h"

#include <algorithm>

namespace rcc {

namespace {

struct Ext {
    char zext;
    char nopw;
    char usew;
};

bool decode_ext(int op, Ext* e) {
    switch (op) {
        case Oextsb:
            *e = Ext{0, 7, 8};
            return true;
        case Oextub:
            *e = Ext{1, 8, 8};
            return true;
        case Oextsh:
            *e = Ext{0, 15, 16};
            return true;
        case Oextuh:
            *e = Ext{1, 16, 16};
            return true;
        case Oextsw:
            *e = Ext{0, 31, 32};
            return true;
        case Oextuw:
            *e = Ext{1, 32, 32};
            return true;
    }
    return false;
}

int bitwidth(uint64_t v) {
    return v ? 64 - __builtin_clzll(v) : 0;
}

template <typename Walk>
int walk_phi_memo(Fn* fn, Ref r, int w, Walk walk) {
    int ret = walk(fn, r, w);
    for (Blk* b = fn->start; b; b = b->link)
        for (Phi* p = b->phi; p; p = p->link)
            p->visit = 0;
    return ret;
}

int uwl(Fn* fn, Ref r, int w);

bool phi_use_ok(Fn* fn, Phi* p, int w) {
    if (p->visit || req(p->to, Ref::none()))
        return true;
    p->visit = 1;
    return uwl(fn, p->to, w) != 0;
}

bool ins_use_ok(Fn* fn, Ins* i, Ref r, int w) {
    Ext e;
    if (i->op == Ocopy && uwl(fn, i->to, w))
        return true;
    if (decode_ext(i->op, &e)) {
        if (e.usew <= w)
            return true;
        return uwl(fn, i->to, w) != 0;
    }
    if (i->op == Oand) {
        Ref rc;
        if (req(r, i->arg[0])) {
            rc = i->arg[1];
        } else {
            assert(req(r, i->arg[1]));
            rc = i->arg[0];
        }
        int64_t v;
        return isconbits(fn, rc, &v) && bitwidth(static_cast<uint64_t>(v)) <= w;
    }
    return false;
}

int uwl(Fn* fn, Ref r, int w) {
    assert(rtype(r) == RTmp);
    Tmp* t = &fn->tmp[r.val];
    for (Use* u = &t->use[0]; u < &t->use[t->nuse]; ++u) {
        bool ok;
        switch (u->type) {
            case Use::UPhi:
                ok = phi_use_ok(fn, u->u.phi, w);
                break;
            case Use::UIns:
                assert(u->u.ins);
                ok = ins_use_ok(fn, u->u.ins, r, w);
                break;
            default:
                ok = false;
                break;
        }
        if (!ok)
            return 0;
    }
    return 1;
}

/* no more than w bits are used */
int usewidthle(Fn* fn, Ref r, int w) {
    return walk_phi_memo(fn, r, w, uwl);
}

int dwl(Fn* fn, Ref r, int w) {
    int64_t v;
    if (isconbits(fn, r, &v) && bitwidth(static_cast<uint64_t>(v)) <= w)
        return 1;
    if (w <= 0)
        return 0;
    if (rtype(r) != RTmp)
        return 0;
    Tmp* t = &fn->tmp[r.val];
    if (t->cls != Kw)
        return 0;

    if (!t->def) {
        Phi* p = fn->rpo[t->bid]->phi;
        for (; p; p = p->link)
            if (req(p->to, r))
                break;
        assert(p);
        if (p->visit && p->visit <= w)
            return 1;
        p->visit = w;
        for (uint n = 0; n < p->narg; ++n)
            if (!dwl(fn, p->arg[n], w))
                return 0;
        return 1;
    }

    Ins* i = t->def;
    if (i->op == Ocopy)
        return dwl(fn, i->arg[0], w);

    if (i->op == Oshr || i->op == Osar) {
        if (isconbits(fn, i->arg[1], &v) && 0 < v && v <= 32) {
            if (i->op == Oshr && w + v >= 32)
                return 1;
            if (w < 32) {
                int cap = (i->op == Osar) ? 31 : 32;
                w = std::min(cap, static_cast<int>(w + v));
            }
        }
        return dwl(fn, i->arg[0], w);
    }

    int x;
    if (iscmp(i->op, &x, &x))
        return w >= 1;

    if (i->op == Oand)
        return (dwl(fn, i->arg[0], w) || dwl(fn, i->arg[1], w)) ? 1 : 0;

    if (i->op == Oor || i->op == Oxor)
        return (dwl(fn, i->arg[0], w) && dwl(fn, i->arg[1], w)) ? 1 : 0;

    Ext e;
    if (decode_ext(i->op, &e)) {
        if (e.zext && e.usew <= w)
            return 1;
        w = std::min(w, static_cast<int>(e.nopw));
        return dwl(fn, i->arg[0], w);
    }

    return 0;
}

/* is the ref narrower than w bits */
int defwidthle(Fn* fn, Ref r, int w) {
    return walk_phi_memo(fn, r, w, dwl);
}

int isw1(Fn* fn, Ref r) {
    return defwidthle(fn, r, 1);
}

bool phieq(Phi* pa, Phi* pb) {
    assert(pa->narg == pb->narg);
    for (uint n = 0; n < pa->narg; ++n)
        if (!req(pa->arg[n], phiarg(pb, pa->blk[n])))
            return false;
    return true;
}

}  // namespace

Ref copyref(Ctx* ctx, Fn* fn, Blk* b, Ins* i) {
    static const bits extcpy[] = {
        /*WFull*/ 0,
        /*Wsb*/ BIT(Tmp::Wsb) | BIT(Tmp::Wsh) | BIT(Tmp::Wsw),
        /*Wub*/ BIT(Tmp::Wub) | BIT(Tmp::Wuh) | BIT(Tmp::Wuw),
        /*Wsh*/ BIT(Tmp::Wsh) | BIT(Tmp::Wsw),
        /*Wuh*/ BIT(Tmp::Wuh) | BIT(Tmp::Wuw),
        /*Wsw*/ BIT(Tmp::Wsw),
        /*Wuw*/ BIT(Tmp::Wuw),
    };
    MAKESURE(extcpy_covers_all_widths, sizeof(extcpy) / sizeof(extcpy[0]) == Tmp::Wuw + 1);

    if (i->op == Ocopy)
        return i->arg[0];

    const Op& od = optab[i->op];

    if (od.hasid && KBASE(i->cls) == 0 && req(i->arg[1], ctx->gvn_con01[od.idval]) &&
        (!od.cmpeqwl || isw1(fn, i->arg[0])))
        return i->arg[0];

    if (od.idemp && req(i->arg[0], i->arg[1]))
        return i->arg[0];

    if ((od.cmpeqwl || od.cmplgtewl) && req(i->arg[0], i->arg[1]))
        return ctx->gvn_con01[od.eqval];

    int z;
    if (od.cmpeqwl && req(i->arg[1], Ref::con_z()) && zeroval(fn, b, i->arg[0], argcls(i, 0), &z))
        return ctx->gvn_con01[od.eqval ^ z ^ 1];

    int64_t v;
    if (i->op == Oand && isconbits(fn, i->arg[1], &v) && v > 0 && ((v + 1) & v) == 0 &&
        defwidthle(fn, i->arg[0], bitwidth(static_cast<uint64_t>(v))))
        return i->arg[0];

    if (i->cls == Kw && (i->op == Oextsw || i->op == Oextuw))
        return i->arg[0];

    Ext e;
    if (decode_ext(i->op, &e) && rtype(i->arg[0]) == RTmp) {
        Tmp* t = &fn->tmp[i->arg[0].val];
        assert(KBASE(t->cls) == 0);

        if (KWIDE(i->cls) > KWIDE(t->cls))
            return Ref::none();

        int w = Tmp::Wsb + (static_cast<int>(i->op) - Oextsb);
        if (BIT(w) & extcpy[t->width])
            return i->arg[0];

        if ((!t->def || !ispar(t->def->op)) && usewidthle(fn, i->to, e.usew))
            return i->arg[0];

        if (defwidthle(fn, i->arg[0], e.nopw))
            return i->arg[0];
    }

    return Ref::none();
}

Ref phicopyref(Ctx* ctx, Fn* fn, Blk* b, Phi* p) {
    Ref r = Ref::none();
    uint n = 0;
    for (; n < p->narg; ++n)
        if (!req(p->arg[n], p->to)) {
            if (req(r, Ref::none()))
                r = p->arg[n];
            else if (!req(p->arg[n], r))
                break;
        }
    if (n == p->narg)
        return r;

    for (Phi* p1 = b->phi; p1 != p; p1 = p1->link) {
        assert(p1);
        if (phieq(p1, p))
            return p1->to;
    }

    Blk* d = b->idom;
    if (p->narg != 2 || d->jmp.type != Jjnz || !isw1(fn, d->jmp.arg))
        return Ref::none();

    Blk* s[2] = {nullptr, nullptr};
    for (uint k = 0; k < 2; ++k)
        for (uint c = 0; c < 2; ++c)
            if (req(p->arg[k], ctx->gvn_con01[c]))
                s[c] = p->blk[k];

    if (d->s1 == s[1] && d->s2 == s[0] && d->s1->jmp.type == Jjmp && d->s2->jmp.type == Jjmp)
        return d->jmp.arg;

    return Ref::none();
}

void narrowpars(Ctx* ctx, Fn* fn) {
    bool has_loop = false;
    for (Blk* b = fn->start; b; b = b->link)
        if (b->loop > 1) {
            has_loop = true;
            break;
        }
    if (!has_loop)
        return;

    Blk* b = fn->start;

    uint npar = 0;
    for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i) {
        if (!ispar(i->op))
            break;
        ++npar;
    }
    if (npar == 0)
        return;

    uint nins = b->nins + npar;
    Ins* ins = static_cast<Ins*>(vnew(ctx, nins, sizeof(Ins), PFn));
    icpy(ins, b->ins, npar);
    icpy(ins + 2 * npar, b->ins + npar, b->nins - npar);
    b->ins = ins;
    b->nins = nins;

    for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i) {
        if (!ispar(i->op))
            break;
        Ins extins = {};
        extins.op = Onop;
        if (i->cls == Kw && usewidthle(fn, i->to, 16)) {
            extins.op = usewidthle(fn, i->to, 8) ? Oextub : Oextuh;
            Ref r = newtmp(ctx, "vw", i->cls, fn);
            extins.cls = i->cls;
            extins.to = i->to;
            extins.arg[0] = r;
            i->to = r;
        }
        *(i + npar) = extins;
    }
}

}  // namespace rcc
