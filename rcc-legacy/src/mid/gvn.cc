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

/// @file gvn.cc
/// @brief Global Value Numbering (GVN) - Cross-block deduplication + jnz simplification + dead
/// block rescue.
///
/// See docs/gvn.md for Global Value Numbering architectural notes.

#include "rcc/all.h"

namespace rcc {

namespace {

// Instruction hashing.

inline uint mix(uint x0, uint x1) {
    return x0 + 31 * x1;
}

inline uint rhash(Ref r) {
    return mix(r.type, r.val);
}

uint ihash(Ins* i) {
    uint h = mix(i->op, i->cls);
    h = mix(h, rhash(i->arg[0]));
    h = mix(h, rhash(i->arg[1]));
    return h;
}

bool ieq(Ins* a, Ins* b) {
    return a->op == b->op && a->cls == b->cls && req(a->arg[0], b->arg[0]) &&
           req(a->arg[1], b->arg[1]);
}

// Lookup instruction.
Ins* gvndup(Ctx* ctx, Ins* i, int insert) {
    uint idx = ihash(i) % ctx->gvn_tbln;
    for (Ins* ii; (ii = ctx->gvn_tbl[idx]);) {
        if (ieq(i, ii))
            return ii;
        if (++idx >= ctx->gvn_tbln)
            idx = 0;
    }
    if (insert)
        ctx->gvn_tbl[idx] = i;
    return nullptr;
}

// Use replacement.

void replaceuse(Ctx* ctx, Fn* fn, Use* u, Ref r1, Ref r2) {
    Tmp* t2 = (rtype(r2) == RTmp) ? &fn->tmp[r2.val] : nullptr;
    Blk* b = fn->rpo[u->bid];
    switch (u->type) {
        case Use::UPhi: {
            Phi* p = u->u.phi;
            for (Ref* pr = p->arg; pr < &p->arg[p->narg]; ++pr)
                if (req(*pr, r1))
                    *pr = r2;
            if (t2)
                adduse_phi(ctx, t2, b, p);
            break;
        }
        case Use::UIns: {
            Ins* i = u->u.ins;
            for (int n = 0; n < 2; ++n)
                if (req(i->arg[n], r1))
                    i->arg[n] = r2;
            if (t2)
                adduse_ins(ctx, t2, b, i);
            break;
        }
        case Use::UJmp:
            if (req(b->jmp.arg, r1))
                b->jmp.arg = r2;
            if (t2)
                adduse_jmp(ctx, t2, b);
            break;
        case Use::UXXX:
            die("unreachable");
    }
}

void replaceuses(Ctx* ctx, Fn* fn, Ref r1, Ref r2) {
    assert(rtype(r1) == RTmp);
    Tmp* t1 = &fn->tmp[r1.val];
    for (Use* u = t1->use; u < &t1->use[t1->nuse]; ++u)
        replaceuse(ctx, fn, u, r1, r2);
    t1->nuse = 0;
}

// Nullify instruction.
void killins(Ctx* ctx, Fn* fn, Ins* i, Ref r) {
    replaceuses(ctx, fn, i->to, r);
    *i = {};
    i->op = Onop;
}

// Handle degraded phi.
void dedupphi(Ctx* ctx, Fn* fn, Blk* b) {
    Phi** pp = &b->phi;
    for (Phi* p; (p = *pp);) {
        Ref r = phicopyref(ctx, fn, b, p);
        if (req(r, Ref::none())) {
            pp = &p->link;
        } else {
            replaceuses(ctx, fn, p->to, r);
            p->to = Ref::none();
            *pp = p->link;
        }
    }
}

// Instruction normalization.

int rcmp(Ref a, Ref b) {
    if (rtype(a) != rtype(b))
        return rtype(a) - rtype(b);
    return static_cast<int>(a.val) - static_cast<int>(b.val);
}

void normins(Ctx* ctx, Fn* fn, Ins* i) {
    int64_t v;
    // Truncate to 32 bits.
    for (int n = 0; n < 2; ++n)
        if (!KWIDE(argcls(i, n)) && isconbits(fn, i->arg[n], &v) && (v & 0xffffffffLL) != v)
            i->arg[n] = getcon(ctx, v & 0xffffffffLL, fn);
    // Commutative op sort.
    if (optab[i->op].commutes && rcmp(i->arg[0], i->arg[1]) > 0) {
        Ref r = i->arg[1];
        i->arg[1] = i->arg[0];
        i->arg[0] = r;
    }
}

// Negate constant.
int negcon(int cls, Con* c) {
    Con z = {};
    z.type = Con::CBits;
    z.bits.i = 0;
    return foldint(c, Osub, cls, &z, c);
}

// Merge constants.
void assoccon(Ctx* ctx, Fn* fn, Blk* b, Ins* i1) {
    int op = (i1->op == Osub) ? Oadd : static_cast<int>(i1->op);

    if (!optab[op].assoc || KBASE(i1->cls) != 0 || rtype(i1->arg[0]) != RTmp ||
        rtype(i1->arg[1]) != RCon)
        return;
    Con c1 = fn->con[i1->arg[1].val];

    Tmp* t2 = &fn->tmp[i1->arg[0].val];
    if (!t2->def)
        return;
    Ins* i2 = t2->def;

    if (op != (i2->op == Osub ? Oadd : static_cast<int>(i2->op)) || rtype(i2->arg[1]) != RCon)
        return;
    Con c2 = fn->con[i2->arg[1].val];

    assert(KBASE(i2->cls) == 0);
    assert(KWIDE(i2->cls) >= KWIDE(i1->cls));

    // Unify as add.
    if (i1->op == Osub && negcon(i1->cls, &c1))
        return;
    if (i2->op == Osub && negcon(i2->cls, &c2))
        return;
    Con c;
    if (foldint(&c, op, i1->cls, &c1, &c2))
        return;

    // Revert to sub.
    if (op == Oadd && c.type == Con::CBits &&
        ((i1->cls == Kl && c.bits.i < 0) ||
         (i1->cls == Kw && static_cast<int32_t>(c.bits.i) < 0))) {
        int fail = negcon(i1->cls, &c);
        assert(fail == 0);
        (void)fail;
        op = Osub;
    }

    i1->op = static_cast<uint>(op);
    i1->arg[0] = i2->arg[0];
    i1->arg[1] = newcon(ctx, &c, fn);
    adduse_ins(ctx, &fn->tmp[i1->arg[0].val], b, i1);
}

// Deduplication.
void dedupins(Ctx* ctx, Fn* fn, Blk* b, Ins* i) {
    normins(ctx, fn, i);
    if (i->op == Onop || pinned(i))
        return;
    // sel skip.
    assert(i->op != Osel0);
    assert(!req(i->to, Ref::none()));
    assoccon(ctx, fn, b, i);

    Ref r = copyref(ctx, fn, b, i);
    if (!req(r, Ref::none())) {
        killins(ctx, fn, i, r);
        return;
    }
    r = foldref(ctx, fn, i);
    if (!req(r, Ref::none())) {
        killins(ctx, fn, i, r);
        return;
    }
    Ins* i1 = gvndup(ctx, i, 1);
    if (i1)
        killins(ctx, fn, i, i1->to);
}

// jnz simplification.

// Check cmp eq/ne 0.
int cmpeqz(Fn* fn, Ref r, Ref* arg, int* cls, int* eqval) {
    if (rtype(r) != RTmp)
        return 0;
    Ins* i = fn->tmp[r.val].def;
    if (i && optab[i->op].cmpeqwl && req(i->arg[1], Ref::con_z())) {
        *arg = i->arg[0];
        *cls = argcls(i, 0);
        *eqval = optab[i->op].eqval;
        return 1;
    }
    return 0;
}

// Check branch dominance.
int branchdom(Fn* fn, Blk* bif, Blk* bbr1, Blk* bbr2, Blk* b) {
    assert(bif->jmp.type == Jjnz);
    return b != bif && dom(bbr1, b) && !reachesnotvia(fn, bbr2, b, bif);
}

// Check 0-branch.
int domzero(Fn* fn, Blk* d, Blk* b, int* z) {
    if (branchdom(fn, d, d->s1, d->s2, b)) {
        *z = 0;
        return 1;
    }
    if (branchdom(fn, d, d->s2, d->s1, b)) {
        *z = 1;
        return 1;
    }
    return 0;
}

// Actual width class.
int usecls(Use* u, Ref r, int cls) {
    switch (u->type) {
        case Use::UIns: {
            int k = Kx;  // widest
            if (req(u->u.ins->arg[0], r))
                k = argcls(u->u.ins, 0);
            if (req(u->u.ins->arg[1], r) && (k == Kx || !KWIDE(k)))
                k = argcls(u->u.ins, 1);
            return k == Kx ? cls : k;
        }
        case Use::UPhi:
            if (req(u->u.phi->to, Ref::none()))
                return cls;  // eliminated
            return u->u.phi->cls;
        case Use::UJmp:
            return Kw;
        default:
            break;
    }
    die("unreachable");
}

// Replace with 0.
void propjnz0(Ctx* ctx, Fn* fn, Blk* bif, Blk* s0, Blk* snon0, Ref r, int cls) {
    if (s0->npred != 1 || rtype(r) != RTmp)
        return;
    Tmp* t = &fn->tmp[r.val];
    for (Use* u = t->use; u < &t->use[t->nuse]; ++u) {
        Blk* b = fn->rpo[u->bid];
        // Match width.
        if (usecls(u, r, cls) == cls && branchdom(fn, bif, s0, snon0, b))
            replaceuse(ctx, fn, u, r, Ref::con_z());
    }
}

void dedupjmp(Ctx* ctx, Fn* fn, Blk* b) {
    if (b->jmp.type != Jjnz)
        return;

    // Propagate jmp arg.
    propjnz0(ctx, fn, b, b->s2, b->s1, b->jmp.arg, Kw);
    // Propagate underlying arg.
    Ref arg;
    int cls, eqval;
    if (cmpeqz(fn, b->jmp.arg, &arg, &cls, &eqval)) {
        Blk* ps[2] = {b->s1, b->s2};
        propjnz0(ctx, fn, b, ps[eqval ^ 1], ps[eqval], arg, cls);
    }

    // Collapse jnz.
    int64_t v = 1;
    int z = 0;
    if (b->s1 == b->s2 || isconbits(fn, b->jmp.arg, &v) || zeroval(fn, b, b->jmp.arg, Kw, &z)) {
        if (v == 0 || z)
            b->s1 = b->s2;
        b->s2 = nullptr;  // Dead branch live instructions are rescued later in rebuildcfg
        b->jmp.type = Jjmp;
        b->jmp.arg = Ref::none();
    }
}

// Rebuild CFG.

void rebuildcfg(Ctx* ctx, Fn* fn) {
    uint nblk = fn->nblk;
    Blk** rpo = static_cast<Blk**>(emalloc(nblk * sizeof rpo[0]));
    memcpy(rpo, fn->rpo, nblk * sizeof rpo[0]);

    fillcfg(ctx, fn);  // Dead blocks are marked with id == ~0u here

    Blk* s = fn->start;
    for (uint n = 0; n < nblk; ++n) {
        Blk* b = rpo[n];
        if (b->id != ~0u)
            continue;  // Still reachable after GVN, skip
        assert(b != s);
        for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i)
            if (!optab[i->op].pinned && gvndup(ctx, i, 0) == i)
                addins(ctx, &s->ins, &s->nins, i);
    }
    free(rpo);
}

}  // namespace

// zeroval infer.
int zeroval(Fn* fn, Blk* b, Ref r, int cls, int* z) {
    Ref arg;
    int cls1, eqval;
    for (Blk* d = b->idom; d; d = d->idom) {
        if (d->jmp.type != Jjnz)
            continue;
        if (req(r, d->jmp.arg) && cls == Kw && domzero(fn, d, b, z))
            return 1;
        if (cmpeqz(fn, d->jmp.arg, &arg, &cls1, &eqval) && req(r, arg) && cls == cls1 &&
            domzero(fn, d, b, z)) {
            *z ^= eqval;
            return 1;
        }
    }
    return 0;
}

// gvn entry.
void gvn(Ctx* ctx, Fn* fn) {
    ctx->gvn_con01[0] = getcon(ctx, 0, fn);
    ctx->gvn_con01[1] = getcon(ctx, 1, fn);

    // Zero out width memo.
    for (Blk* b = fn->start; b; b = b->link)
        for (Phi* p = b->phi; p; p = p->link)
            p->visit = 0;

    fillloop(fn);
    narrowpars(ctx, fn);
    filluse(ctx, fn);
    ssacheck(fn);

    uint nins = 0;
    for (Blk* b = fn->start; b; b = b->link) {
        b->visit = 0;
        nins += b->nins;
    }

    ctx->gvn_tbln = nins + nins / 2;
    ctx->gvn_tbl = static_cast<Ins**>(emalloc(ctx->gvn_tbln * sizeof ctx->gvn_tbl[0]));
    for (uint n = 0; n < fn->nblk; ++n) {
        Blk* b = fn->rpo[n];
        dedupphi(ctx, fn, b);
        for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i)
            dedupins(ctx, fn, b, i);
        dedupjmp(ctx, fn, b);
    }
    rebuildcfg(ctx, fn);
    free(ctx->gvn_tbl);
    ctx->gvn_tbl = nullptr;
    ctx->gvn_tbln = 0;

    if (debug['G']) {
        fprintf(ctx->dbg_buf, "\n> After GVN:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
