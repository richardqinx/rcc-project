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

/// @file cfg.cc
/// @brief CFG structure analysis and simplification passes.
///
/// See docs/cfg.md for architectural notes.

#include "rcc/all.h"

#include <utility>

namespace rcc {

// Block construction

Blk* newblk(Ctx* ctx) {
    auto* b = static_cast<Blk*>(alloc(ctx, sizeof(Blk)));
    memset(b, 0, sizeof *b);
    b->ins = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PFn));
    b->pred = static_cast<Blk**>(vnew(ctx, 0, sizeof(Blk*), PFn));
    return b;
}

// Reverse Post-Order (RPO) numbering + dead block elimination

namespace {

void post_visit(Blk* b, uint* counter) {
    if (!b || b->visit)
        return;
    b->visit = 1;
    // Heuristic: recurse outer branch first.
    Blk* a = b->s1;
    Blk* c = b->s2;
    if (a && c && a->loop > c->loop)
        std::swap(a, c);
    post_visit(a, counter);
    post_visit(c, counter);
    b->id = (*counter)++;
}

void fillrpo(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = 0;

    uint cnt = 0;
    post_visit(fn->start, &cnt);
    fn->nblk = cnt;

    vensure(ctx, &fn->rpo, fn->nblk, sizeof(Blk*), PFn);

    // Second pass: remove unvisited, invert post-order.
    Blk** slot = &fn->start;
    while (Blk* b = *slot) {
        if (!b->visit) {
            b->id = ~0u;
            *slot = b->link;
        } else {
            b->id = fn->nblk - 1 - b->id;
            fn->rpo[b->id] = b;
            slot = &b->link;
        }
    }
}

}  // namespace

// Predecessor sets

void fillpreds(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        b->npred = 0;

    for (Blk* src = fn->start; src; src = src->link) {
        if (src->s1)
            src->s1->npred++;
        if (src->s2 && src->s2 != src->s1)
            src->s2->npred++;
    }

    for (Blk* b = fn->start; b; b = b->link) {
        if (b->npred)
            vensure(ctx, &b->pred, b->npred, sizeof(Blk*), PFn);
        b->npred = 0;
    }

    for (Blk* src = fn->start; src; src = src->link) {
        if (src->s1)
            src->s1->pred[src->s1->npred++] = src;
        if (src->s2 && src->s2 != src->s1)
            src->s2->pred[src->s2->npred++] = src;
    }
}

// phi cleanup.

namespace {

void fixphis(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        for (Phi* p = b->phi; p; p = p->link) {
            uint keep = 0;
            for (uint i = 0; i < p->narg; i++) {
                Blk* src = p->blk[i];
                if (src->id == ~0u)
                    continue;
                if (src->s1 != b && src->s2 != b)
                    continue;
                if (keep != i) {
                    p->blk[keep] = src;
                    p->arg[keep] = p->arg[i];
                }
                keep++;
            }
            assert(keep > 0 && "phi has no live incoming edge");
            p->narg = keep;
        }
    }
}

}  // namespace

void fillcfg(Ctx* ctx, Fn* fn) {
    fillrpo(ctx, fn);
    fillpreds(ctx, fn);
    fixphis(fn);
}

// Dominator Tree (Cooper-Harvey-Kennedy)

namespace {

/// Walk up idom chain to find the lowest common ancestor of x and y in dom-tree.
/// Invariant: Larger RPO id means a deeper node in the tree, move the deeper one up by one step
/// each time.
Blk* dom_meet(Blk* x, Blk* y) {
    if (!x)
        return y;
    while (x != y) {
        if (x->id < y->id)
            std::swap(x, y);
        x = x->idom;
        assert(x);
    }
    return x;
}

}  // namespace

void filldom(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        b->idom = nullptr;
        b->dom = nullptr;
        b->dlink = nullptr;
    }

    // Fixpoint iteration.
    for (bool changed = true; changed;) {
        changed = false;
        for (uint i = 1; i < fn->nblk; i++) {
            Blk* b = fn->rpo[i];
            Blk* d = nullptr;
            for (uint k = 0; k < b->npred; k++) {
                Blk* p = b->pred[k];
                if (p->idom || p == fn->start)
                    d = dom_meet(d, p);
            }
            if (d != b->idom) {
                b->idom = d;
                changed = true;
            }
        }
    }

    // Reverse chain: attach dom-children.
    for (Blk* b = fn->start; b; b = b->link) {
        if (Blk* d = b->idom) {
            assert(d != b);
            b->dlink = d->dom;
            d->dom = b;
        }
    }
}

int sdom(Blk* a, Blk* b) {
    assert(a && b);
    if (a == b)
        return 0;
    while (b->id > a->id)
        b = b->idom;
    return a == b;
}

int dom(Blk* a, Blk* b) {
    return a == b || sdom(a, b);
}

// Dominance Frontier

namespace {

void push_fron(Ctx* ctx, Blk* host, Blk* node) {
    for (uint i = 0; i < host->nfron; i++)
        if (host->fron[i] == node)
            return;
    vensure(ctx, &host->fron, host->nfron + 1, sizeof(Blk*), PFn);
    host->fron[host->nfron++] = node;
}

/// For edge (src -> dst): walk up idom chain from src, collect dst into the dominance frontier of
/// all ancestors that do not strictly dominate dst.
void contribute_fron(Ctx* ctx, Blk* src, Blk* dst) {
    for (Blk* a = src; a && !sdom(a, dst); a = a->idom)
        push_fron(ctx, a, dst);
}

}  // namespace

void fillfron(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        b->nfron = 0;
    for (Blk* b = fn->start; b; b = b->link) {
        if (b->s1)
            contribute_fron(ctx, b, b->s1);
        if (b->s2)
            contribute_fron(ctx, b, b->s2);
    }
}

// Natural loop identification / Loop weights

namespace {

void mark_loop_body(Blk* hd, Blk* node, void (*cb)(Blk*, Blk*)) {
    if (node->id < hd->id || node->visit == hd->id)
        return;
    node->visit = hd->id;
    cb(hd, node);
    for (uint k = 0; k < node->npred; k++)
        mark_loop_body(hd, node->pred[k], cb);
}

void loop_x10(Blk* hd, Blk* b) {
    (void)hd;
    b->loop *= 10;
}

}  // namespace

void loopiter(Fn* fn, void (*cb)(Blk*, Blk*)) {
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = ~0u;
    for (uint i = 0; i < fn->nblk; i++) {
        Blk* hd = fn->rpo[i];
        for (uint k = 0; k < hd->npred; k++)
            if (hd->pred[k]->id >= i)
                mark_loop_body(hd, hd->pred[k], cb);
    }
}

void fillloop(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        b->loop = 1;
    loopiter(fn, loop_x10);
}

// dom-tree depth + lowest common ancestor

void filldepth(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        b->depth = -1;
    fn->start->depth = 0;

    // Walk up idom chain.
    for (Blk* b = fn->start; b; b = b->link) {
        if (b->depth != -1)
            continue;
        Blk* d = b->idom;
        int steps = 1;
        while (d->depth == -1) {
            d = d->idom;
            steps++;
        }
        steps += d->depth;
        b->depth = steps;
        for (Blk* a = b->idom; a->depth == -1; a = a->idom)
            a->depth = --steps;
    }
}

Blk* lca(Blk* a, Blk* b) {
    if (!a)
        return b;
    if (!b)
        return a;
    while (a->depth > b->depth)
        a = a->idom;
    while (b->depth > a->depth)
        b = b->idom;
    while (a != b) {
        a = a->idom;
        b = b->idom;
    }
    return a;
}

// simpljmp: fold empty transit blocks.

namespace {

/// Union-find query with path compression: find root along uf[], redirecting each node on the chain
/// to root.
Blk* uf_find(Blk** uf, Blk* x) {
    Blk* root = x;
    while (uf[root->id])
        root = uf[root->id];
    while (uf[x->id] && uf[x->id] != root) {
        Blk* nxt = uf[x->id];
        uf[x->id] = root;
        x = nxt;
    }
    return root;
}

}  // namespace

void simpljmp(Ctx* ctx, Fn* fn) {
    Blk* exit_blk = newblk(ctx);
    exit_blk->id = fn->nblk++;
    exit_blk->jmp.type = Jret0;
    exit_blk->name = strf(ctx, PFn, "ret");

    Blk** uf = static_cast<Blk**>(emalloc(fn->nblk * sizeof(Blk*)));
    memset(uf, 0, fn->nblk * sizeof(Blk*));

    for (Blk* b = fn->start; b; b = b->link) {
        assert(!b->phi && "simpljmp requires phi-free CFG");
        if (b->jmp.type == Jret0) {
            b->jmp.type = Jjmp;
            b->s1 = exit_blk;
        }
        if (b->nins == 0 && b->jmp.type == Jjmp) {
            Blk* r = uf_find(uf, b->s1);
            if (r != b)
                uf[b->id] = r;
        }
    }

    Blk** tail = &fn->start;
    for (Blk* b; (b = *tail); tail = &b->link) {
        if (b->s1)
            b->s1 = uf_find(uf, b->s1);
        if (b->s2)
            b->s2 = uf_find(uf, b->s2);
        if (b->s1 && b->s1 == b->s2) {
            b->jmp.type = Jjmp;
            b->s2 = nullptr;
        }
    }
    *tail = exit_blk;

    free(uf);
}

// Reachability / if-graph pattern recognition

namespace {

int dfs_can_reach(Blk* from, Blk* to) {
    if (from == to)
        return 1;
    if (!from || from->visit)
        return 0;
    from->visit = 1;
    if (dfs_can_reach(from->s1, to))
        return 1;
    if (dfs_can_reach(from->s2, to))
        return 1;
    return 0;
}

}  // namespace

int reaches(Fn* fn, Blk* from, Blk* to) {
    assert(to);
    int r = dfs_can_reach(from, to);
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = 0;
    return r;
}

int reachesnotvia(Fn* fn, Blk* from, Blk* to, Blk* excl) {
    excl->visit = 1;
    return reaches(fn, from, to);
}

int ifgraph(Blk* head, Blk** pthen, Blk** pelse, Blk** pjoin) {
    if (head->jmp.type != Jjnz)
        return 0;

    Blk* lo = head->s1;
    Blk* hi = head->s2;
    bool swapped = false;
    if (lo->id > hi->id) {
        std::swap(lo, hi);
        swapped = true;
    }
    if (lo == hi)
        return 0;
    if (lo->jmp.type != Jjmp || lo->npred != 1)
        return 0;

    Blk** out_then = swapped ? pelse : pthen;
    Blk** out_else = swapped ? pthen : pelse;

    if (lo->s1 == hi) {
        // if-then (no else branch: head directly serves as else)
        if (hi->npred != 2)
            return 0;
        *out_then = lo;
        *out_else = head;
        *pjoin = hi;
        return 1;
    }

    if (hi->jmp.type != Jjmp || hi->npred != 1)
        return 0;
    if (lo->s1 != hi->s1 || lo->s1->npred != 2)
        return 0;

    assert(lo->s1 != head);
    *out_then = lo;
    *out_else = hi;
    *pjoin = lo->s1;
    return 1;
}

// simplcfg

namespace {

struct JmpSnap {
    short type;
    Ref arg;
    Blk* s1;
    Blk* s2;
};

bool snap_eq(const JmpSnap& a, const JmpSnap& b) {
    return a.type == b.type && req(a.arg, b.arg) && a.s1 == b.s1 && a.s2 == b.s2;
}

bool target_phi_free(const JmpSnap& j) {
    if (j.s1 && j.s1->phi)
        return false;
    if (j.s2 && j.s2->phi)
        return false;
    return true;
}

bool block_is_empty(Blk* b) {
    if (b->phi)
        return false;
    for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
        if (i->op != Onop && i->op != Odbgloc)
            return false;
    return true;
}

void phi_to_copies(Ctx* ctx, Blk* b) {
    assert(b->npred == 1);
    Blk* p = b->pred[0];
    Ins copy{};
    copy.op = Ocopy;
    for (Phi* phi = b->phi; phi; phi = phi->link) {
        copy.cls = static_cast<uint>(phi->cls);
        copy.to = phi->to;
        copy.arg[0] = phiarg(phi, p);
        copy.arg[1] = Ref::none();
        addins(ctx, &p->ins, &p->nins, &copy);
    }
    b->phi = nullptr;
}

}  // namespace

void simplcfg(Ctx* ctx, Fn* fn) {
    if (debug['C']) {
        fprintf(ctx->dbg_buf, "\n> Before CFG simplification:\n");
        printfn(fn, ctx->dbg_buf);
    }

    for (Blk* b = fn->start; b; b = b->link)
        if (b->npred == 1)
            phi_to_copies(ctx, b);

    auto* snap = static_cast<JmpSnap*>(emalloc(fn->nblk * sizeof(JmpSnap)));
    auto* empty = static_cast<bool*>(emalloc(fn->nblk * sizeof(bool)));
    for (Blk* b = fn->start; b; b = b->link) {
        snap[b->id] = {b->jmp.type, b->jmp.arg, b->s1, b->s2};
        empty[b->id] = block_is_empty(b);
    }

    bool dirty;
    do {
        dirty = false;
        for (Blk* b = fn->start; b; b = b->link) {
            if (b->id == ~0u)
                continue;
            JmpSnap* j = &snap[b->id];

            if (j->type == Jjmp && j->s1->npred == 1) {
                Blk* s = j->s1;
                assert(!s->phi);
                addbins(ctx, &b->ins, &b->nins, s);
                empty[b->id] = empty[b->id] && empty[s->id];

                JmpSnap* sj = &snap[s->id];
                Blk* succs[2] = {sj->s1, sj->s2};
                for (Blk* sb : succs) {
                    if (!sb)
                        continue;
                    for (Phi* p = sb->phi; p; p = p->link) {
                        uint n = phiargn(p, s);
                        if (n != ~0u)
                            p->blk[n] = b;
                    }
                }

                s->id = ~0u;
                *j = *sj;
                dirty = true;
            } else if (j->type == Jjnz && empty[j->s1->id] && empty[j->s2->id] &&
                       snap_eq(snap[j->s1->id], snap[j->s2->id]) &&
                       target_phi_free(snap[j->s1->id])) {
                *j = snap[j->s1->id];
                dirty = true;
            }
        }
    } while (dirty);

    for (Blk* b = fn->start; b; b = b->link) {
        if (b->id == ~0u)
            continue;
        JmpSnap* j = &snap[b->id];
        b->jmp.type = j->type;
        b->jmp.arg = j->arg;
        b->s1 = j->s1;
        b->s2 = j->s2;
        assert(!j->s1 || j->s1->id != ~0u);
        assert(!j->s2 || j->s2->id != ~0u);
    }

    fillcfg(ctx, fn);

    free(empty);
    free(snap);

    if (debug['C']) {
        fprintf(ctx->dbg_buf, "\n> After CFG simplification:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
