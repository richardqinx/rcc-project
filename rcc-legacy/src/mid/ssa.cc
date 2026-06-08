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

/// @file ssa.cc
/// @brief Use chain population / SSA construction (Cytron-style) / SSA invariant validation.
///
/// See docs/ssa.md for architectural notes.

#include "rcc/all.h"

namespace rcc {

// Use chain append

namespace {

inline void use_append(Ctx* ctx, Tmp* tmp, decltype(Use::type) ty, Blk* b, Phi* phi, Ins* ins) {
    if (!tmp->use)
        return;
    uint n = tmp->nuse++;
    vgrow(ctx, &tmp->use, tmp->nuse);
    Use* u = &tmp->use[n];
    u->type = ty;
    u->bid = b->id;
    if (ty == Use::UPhi)
        u->u.phi = phi;
    else if (ty == Use::UIns)
        u->u.ins = ins;
    // UJmp has no extra payload
}

}  // namespace

void adduse_ins(Ctx* ctx, Tmp* tmp, Blk* b, Ins* ins) {
    use_append(ctx, tmp, Use::UIns, b, nullptr, ins);
}
void adduse_phi(Ctx* ctx, Tmp* tmp, Blk* b, Phi* phi) {
    use_append(ctx, tmp, Use::UPhi, b, phi, nullptr);
}
void adduse_jmp(Ctx* ctx, Tmp* tmp, Blk* b) {
    use_append(ctx, tmp, Use::UJmp, b, nullptr, nullptr);
}

// filluse - Recompute def / use / cls / width / phi-equiv-class for each tmp

namespace {

/// Derive width from def instruction op (sub-word width info, for spill storage selection).
int derive_width(Ins* i) {
    int op = i->op;
    if (isparbh(op))
        return Tmp::Wsb + (op - Oparsb);
    if (isload(op) && op != Oload)
        return Tmp::Wsb + (op - Oloadsb);
    if (isext(op))
        return Tmp::Wsb + (op - Oextsb);
    int kx, cx;
    if (iscmp(op, &kx, &cx))
        return Tmp::Wub;
    return Tmp::WFull;
}

}  // namespace

void filluse(Ctx* ctx, Fn* fn) {
    Tmp* tmp = fn->tmp;

    // Reset metadata for each user tmp; first call creates empty vnew head for use[].
    for (int t = Tmp0; t < fn->ntmp; t++) {
        tmp[t].def = nullptr;
        tmp[t].bid = ~0u;
        tmp[t].ndef = 0;
        tmp[t].nuse = 0;
        tmp[t].cls = 0;
        tmp[t].phi = 0;
        tmp[t].width = Tmp::WFull;
        if (!tmp[t].use)
            tmp[t].use = static_cast<Use*>(vnew(ctx, 0, sizeof(Use), PFn));
    }

    // Iterate blocks, register phi def/use, ins def/use, jmp arg use.
    for (Blk* b = fn->start; b; b = b->link) {
        // phi
        for (Phi* p = b->phi; p; p = p->link) {
            assert(rtype(p->to) == RTmp);
            int td = p->to.val;
            tmp[td].bid = b->id;
            tmp[td].ndef++;
            tmp[td].cls = p->cls;

            int td_cls = phicls(td, fn->tmp);
            for (uint a = 0; a < p->narg; a++) {
                if (rtype(p->arg[a]) != RTmp)
                    continue;
                int ta = p->arg[a].val;
                adduse_phi(ctx, &tmp[ta], b, p);
                int ta_cls = phicls(ta, fn->tmp);
                if (ta_cls != td_cls)
                    tmp[ta_cls].phi = td_cls;
            }
        }

        // Instruction def + double arg use
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
            if (!req(i->to, Ref::none())) {
                assert(rtype(i->to) == RTmp);
                int td = i->to.val;
                int w = derive_width(i);
                // Under Kw cls, Wsw/Wuw is equivalent to WFull (high bits don't exist anyway)
                if ((w == Tmp::Wsw || w == Tmp::Wuw) && i->cls == Kw)
                    w = Tmp::WFull;
                tmp[td].width = static_cast<decltype(tmp[td].width)>(w);
                tmp[td].def = i;
                tmp[td].bid = b->id;
                tmp[td].ndef++;
                tmp[td].cls = static_cast<short>(i->cls);
            }
            for (int m = 0; m < 2; m++) {
                if (rtype(i->arg[m]) != RTmp)
                    continue;
                int ta = i->arg[m].val;
                adduse_ins(ctx, &tmp[ta], b, i);
            }
        }

        // Terminating jump arg
        if (rtype(b->jmp.arg) == RTmp)
            adduse_jmp(ctx, &tmp[b->jmp.arg.val], b);
    }
}

// Name Pool - per-Ctx, emalloc nodes reused across fn, drained at ~Ctx

struct Name {
    Ref r;
    Blk* b;
    Name* up;
};

namespace {

Name* nnew(Ctx* ctx, Ref r, Blk* b, Name* up) {
    Name* n;
    if (ctx->ssa_namel) {
        n = ctx->ssa_namel;
        ctx->ssa_namel = n->up;
    } else {
        // Reused across fn, so not in SubPool (SubPool frees with fn, list would dangle)
        n = static_cast<Name*>(emalloc(sizeof(Name)));
    }
    n->r = r;
    n->b = b;
    n->up = up;
    return n;
}

void nfree(Ctx* ctx, Name* n) {
    n->up = ctx->ssa_namel;
    ctx->ssa_namel = n;
}

}  // namespace

void ssa_drain_namel(Ctx* ctx) {
    Name* n = ctx->ssa_namel;
    while (n) {
        Name* nxt = n->up;
        free(n);
        n = nxt;
    }
    ctx->ssa_namel = nullptr;
}

// Rename helpers: rendef registers new def, getstk queries currently visible name, renblk DFS along
// dom tree

namespace {

Ref fresh_rename(Ctx* ctx, int t, Fn* fn) {
    return newtmp(ctx, fn->tmp[t].name, fn->tmp[t].cls, fn);
}

inline bool needs_rename(Fn* fn, int t) {
    return fn->tmp[t].visit != 0;
}

/// Change a def to a fresh name.
void rendef(Ctx* ctx, Ref* r, Blk* b, Name** stk, Fn* fn) {
    if (req(*r, Ref::none()))
        return;
    int t = r->val;
    if (!needs_rename(fn, t))
        return;
    Ref r1 = fresh_rename(ctx, t, fn);
    fn->tmp[r1.val].visit = t;  // Record "this fresh name originates from original tmp t"
    stk[t] = nnew(ctx, r1, b, stk[t]);
    *r = r1;
}

/// Query currently visible name of tmp t at b.
Ref getstk(Ctx* ctx, int t, Blk* b, Name** stk) {
    Name* n = stk[t];
    while (n && !dom(n->b, b)) {
        Name* dead = n;
        n = n->up;
        nfree(ctx, dead);
    }
    stk[t] = n;
    return n ? n->r : Ref::undef();
}

void renblk(Ctx* ctx, Blk* b, Name** stk, Fn* fn) {
    // Rename phi-def
    for (Phi* p = b->phi; p; p = p->link)
        rendef(ctx, &p->to, b, stk, fn);

    // Instructions: rename args first (read old -> write new), then rendef def
    for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
        for (int m = 0; m < 2; m++) {
            if (rtype(i->arg[m]) != RTmp)
                continue;
            int t = i->arg[m].val;
            if (needs_rename(fn, t))
                i->arg[m] = getstk(ctx, t, b, stk);
        }
        rendef(ctx, &i->to, b, stk, fn);
    }

    // jmp arg
    if (rtype(b->jmp.arg) == RTmp) {
        int t = b->jmp.arg.val;
        if (needs_rename(fn, t))
            b->jmp.arg = getstk(ctx, t, b, stk);
    }

    // Append arg from this block to phi of each successor s
    Blk* succs[2] = {b->s1, (b->s2 == b->s1) ? nullptr : b->s2};
    for (Blk* s : succs) {
        if (!s)
            continue;
        for (Phi* p = s->phi; p; p = p->link) {
            int orig_t = p->to.val;
            int src_t = fn->tmp[orig_t].visit;  // phi.to was inserted by phiins, visit=source tmp
            if (!src_t)
                continue;
            uint k = p->narg++;
            vgrow(ctx, &p->arg, p->narg);
            vgrow(ctx, &p->blk, p->narg);
            p->arg[k] = getstk(ctx, src_t, b, stk);
            p->blk[k] = b;
        }
    }

    // Recurse dom-tree children
    for (Blk* s = b->dom; s; s = s->dlink)
        renblk(ctx, s, stk, fn);
}

// phiins - Cytron-style phi insertion

/// Scan all blocks, handle intra-block multi-defs of tmp t.
short collect_real_defs(Ctx* ctx, Fn* fn, int t, BSet* on_worklist, Blk** be, Blk*** bp_io) {
    short k = Kx;
    Ref t_ref = Ref::tmp(static_cast<uint>(t));

    for (Blk* b = fn->start; b; b = b->link) {
        b->visit = 0;
        Ref running = Ref::none();

        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
            // Rewrite uses appearing after running to running name
            if (!req(running, Ref::none())) {
                if (req(i->arg[0], t_ref))
                    i->arg[0] = running;
                if (req(i->arg[1], t_ref))
                    i->arg[1] = running;
            }
            if (!req(i->to, t_ref))
                continue;

            if (!bshas(b->out, static_cast<uint>(t))) {
                // Def that doesn't escape block at the end: fresh naming in-place
                running = fresh_rename(ctx, t, fn);
                i->to = running;
            } else {
                // Truly escaping def: push b to worklist
                if (!bshas(on_worklist, b->id)) {
                    bsset(on_worklist, b->id);
                    *--(*bp_io) = b;
                }
                if (clsmerge(&k, static_cast<short>(i->cls)))
                    die("ssa: incompatible classes for %%%s", fn->tmp[t].name);
            }
        }
        (void)be;

        // Block tail jmp arg also follows running
        if (!req(running, Ref::none()) && req(b->jmp.arg, t_ref))
            b->jmp.arg = running;
    }
    return k;
}

void phiins(Ctx* ctx, Fn* fn) {
    BSet on_worklist[1], original_defs[1];
    bsinit(ctx, on_worklist, fn->nblk);
    bsinit(ctx, original_defs, fn->nblk);

    // worklist is a stack, growing backwards from blist end. bp == be means empty.
    Blk** blist = static_cast<Blk**>(emalloc(fn->nblk * sizeof(Blk*)));
    Blk** be = blist + fn->nblk;

    for (int t = Tmp0; t < fn->ntmp; t++) {
        fn->tmp[t].visit = 0;
        if (fn->tmp[t].phi != 0)
            continue;  // Not head of phi-equiv-class, skip (meaning tmp is already handled by its
                       // representative)

        // Fast path: single def and (def at entry, or all uses in same block as def) -> no phi and
        // rename needed
        if (fn->tmp[t].ndef == 1) {
            bool same = true;
            uint defb = fn->tmp[t].bid;
            Use* uses = fn->tmp[t].use;
            for (uint n = 0; n < fn->tmp[t].nuse; n++)
                if (uses[n].bid != defb) {
                    same = false;
                    break;
                }
            if (same || defb == fn->start->id)
                continue;
        }

        bszero(on_worklist);
        Blk** bp = be;
        short k = collect_real_defs(ctx, fn, t, on_worklist, be, &bp);

        // original_defs is used later to check "did phi block originally have def of t"
        bscopy(original_defs, on_worklist);

        // Phase 2: IDF main loop
        while (bp != be) {
            fn->tmp[t].visit = t;  // Mark: t actually entered renaming
            Blk* b = *bp++;
            bsclr(on_worklist, b->id);

            for (uint n = 0; n < b->nfron; n++) {
                Blk* a = b->fron[n];
                if (a->visit++ != 0)
                    continue;  // Already inserted phi for a in this round
                if (!bshas(a->in, static_cast<uint>(t)))
                    continue;  // t not live at a entry, no phi needed

                Phi* p = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
                memset(p, 0, sizeof(Phi));
                p->cls = k;
                p->to = Ref::tmp(static_cast<uint>(t));
                p->link = a->phi;
                p->arg = static_cast<Ref*>(vnew(ctx, 0, sizeof(Ref), PFn));
                p->blk = static_cast<Blk**>(vnew(ctx, 0, sizeof(Blk*), PFn));
                a->phi = p;

                // a is now also a def block of t (phi form), push a to worklist to continue IDF
                if (bshas(original_defs, a->id))
                    continue;
                if (bshas(on_worklist, a->id))
                    continue;
                bsset(on_worklist, a->id);
                *--bp = a;
            }
        }
    }

    free(blist);
}

}  // namespace

// ssa() - Main entry: filldom -> fillfron -> filllive -> phiins -> renblk

void ssa(Ctx* ctx, Fn* fn) {
    // stk[] is allocated by "ntmp before rename".
    int ntmp0 = fn->ntmp;
    Name** stk = static_cast<Name**>(emalloc(static_cast<size_t>(ntmp0) * sizeof(Name*)));

    // Internal filllive call in ssa() is only for phiins, avoiding leak to -d L dump
    int saved_L = debug['L'];
    debug['L'] = 0;

    filldom(fn);

    if (debug['N']) {
        FILE* f = ctx->dbg_buf;
        fprintf(f, "\n> Dominators:\n");
        for (Blk* bb = fn->start; bb; bb = bb->link) {
            if (!bb->dom)
                continue;
            fprintf(f, "%10s:", bb->name);
            for (Blk* c = bb->dom; c; c = c->dlink)
                fprintf(f, " %s", c->name);
            fputc('\n', f);
        }
    }

    fillfron(ctx, fn);
    filllive(ctx, fn);
    phiins(ctx, fn);
    renblk(ctx, fn->start, stk, fn);

    // Reclaim all leftover Names on stk[] to free list (only walking ntmp0 range)
    for (int t = ntmp0; --t >= 0;) {
        Name* n = stk[t];
        while (n) {
            Name* nx = n->up;
            nfree(ctx, n);
            n = nx;
        }
    }
    free(stk);

    debug['L'] = static_cast<char>(saved_L);

    if (debug['N']) {
        fprintf(ctx->dbg_buf, "\n> After SSA construction:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

// ssacheck - SSA invariant validation

namespace {

bool phi_arg_dominated_by(Phi* p, Blk* def_blk, Ref defref) {
    for (uint n = 0; n < p->narg; n++) {
        if (!req(p->arg[n], defref))
            continue;
        if (!dom(def_blk, p->blk[n]))
            return false;
    }
    return true;
}

[[noreturn]] void ssa_err(Tmp* t, Blk* ub) {
    if (t->visit)
        die("ssa: %%%s violates SSA invariant", t->name);
    die("ssa: %%%s used undefined in @%s", t->name, ub->name);
}

/// Verify all uses of t are dominated by (def_blk, def_ins). def_ins = nullptr means phi-def.
void verify_uses(Fn* fn, Blk* def_blk, Ins* def_ins, Tmp* t, Ref defref) {
    for (Use* u = t->use; u < &t->use[t->nuse]; u++) {
        Blk* ub = fn->rpo[u->bid];

        if (u->type == Use::UPhi) {
            if (!phi_arg_dominated_by(u->u.phi, def_blk, defref))
                ssa_err(t, ub);
            continue;
        }

        if (ub == def_blk) {
            // Intra-block: ins-use must be after ins-def (pointer order; valid comparison in same
            // b->ins array)
            if (def_ins && u->type == Use::UIns && u->u.ins <= def_ins)
                ssa_err(t, ub);
            // UJmp is at block end, definitely after def; no extra check needed
        } else {
            if (!sdom(def_blk, ub))
                ssa_err(t, ub);
        }
    }
}

}  // namespace

void ssacheck(Fn* fn) {
    // Global check: each tmp <=1 def; use must have def
    for (Tmp* t = &fn->tmp[Tmp0]; t - fn->tmp < fn->ntmp; t++) {
        if (t->ndef > 1)
            die("ssa: temporary %%%s defined more than once", t->name);
        if (t->nuse > 0 && t->ndef == 0)
            ssa_err(t, fn->rpo[t->use[0].bid]);
    }

    // Each def's use must be dominated by it
    for (Blk* b = fn->start; b; b = b->link) {
        for (Phi* p = b->phi; p; p = p->link)
            verify_uses(fn, b, nullptr, &fn->tmp[p->to.val], p->to);
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
            if (rtype(i->to) != RTmp)
                continue;
            verify_uses(fn, b, i, &fn->tmp[i->to.val], i->to);
        }
    }
}

}  // namespace rcc
