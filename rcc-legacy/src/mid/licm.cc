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

/// @file licm.cc
/// @brief Phase A Loop Invariant Code Motion (LICM).
///
/// See docs/licm.md for architectural notes.

#include "rcc/all.h"

#include <vector>

namespace rcc {

namespace {

struct LoopInfo {
    Blk* header;
    std::vector<Blk*> body;  // includes header
    Blk* preheader;          // nullptr if no unique non-back-edge predecessor
};

Ins make_nop() {
    Ins i = {};
    i.op = Onop;
    return i;
}

// Loop Identification.
void build_loops(Fn* fn, std::vector<LoopInfo>& loops, std::vector<int>& blk_loop) {
    loops.clear();
    blk_loop.assign(static_cast<size_t>(fn->nblk), -1);

    for (Blk* b = fn->start; b; b = b->link)
        b->visit = ~0u;

    auto refine_inner = [&](uint bid, int lidx) {
        int cur = blk_loop[bid];
        if (cur < 0 || loops[static_cast<size_t>(cur)].header->id <
                           loops[static_cast<size_t>(lidx)].header->id)
            blk_loop[bid] = lidx;
    };

    for (uint i = 0; i < fn->nblk; i++) {
        Blk* hd = fn->rpo[i];
        bool has_back = false;
        for (uint k = 0; k < hd->npred; k++) {
            if (hd->pred[k]->id >= i) {
                has_back = true;
                break;
            }
        }
        if (!has_back)
            continue;

        int lidx = static_cast<int>(loops.size());
        loops.push_back({hd, {hd}, nullptr});
        refine_inner(hd->id, lidx);

        // DFS body collect.
        std::vector<Blk*> stack;
        for (uint k = 0; k < hd->npred; k++)
            if (hd->pred[k]->id >= i)
                stack.push_back(hd->pred[k]);

        while (!stack.empty()) {
            Blk* n = stack.back();
            stack.pop_back();
            if (n->id < hd->id || n->visit == hd->id)
                continue;
            n->visit = hd->id;
            loops[static_cast<size_t>(lidx)].body.push_back(n);
            refine_inner(n->id, lidx);
            for (uint k = 0; k < n->npred; k++)
                stack.push_back(n->pred[k]);
        }
    }

    // Find preheader.
    for (auto& L : loops) {
        Blk* phdr = nullptr;
        bool unique = true;
        for (uint k = 0; k < L.header->npred; k++) {
            Blk* p = L.header->pred[k];
            bool in_body = false;
            for (Blk* b : L.body)
                if (b == p) {
                    in_body = true;
                    break;
                }
            if (in_body)
                continue;  // back-edge
            if (phdr) {
                unique = false;
                break;
            }
            phdr = p;
        }
        L.preheader = unique ? phdr : nullptr;
    }
}

// Invariance
bool blk_in(Blk* x, const std::vector<Blk*>& body) {
    for (Blk* b : body)
        if (b == x)
            return true;
    return false;
}

bool is_invariant(Fn* fn, Ref r, const LoopInfo& L) {
    if (rtype(r) == RCon)
        return true;
    if (rtype(r) != RTmp)
        return false;
    Tmp* t = &fn->tmp[r.val];
    if (t->bid == ~0u)
        return true;  // Undefined (treat as param/global-like) - equivalent to "outside loop"
    return !blk_in(fn->rpo[t->bid], L.body);
}

// Check safety.
bool clobbered_in_loop(Fn* fn, Ins* load_ins, Ref addr, int sz, const LoopInfo& L) {
    for (Blk* b : L.body) {
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
            if (i == load_ins)
                continue;
            if (i->op == Ocall)
                return true;  // Conservative: call is treated as writing all escapable memory
            if (isstore(i->op)) {
                int ssz = storesz(i);
                int delta;
                if (alias(addr, 0, sz, i->arg[1], ssz, &delta, fn) != NoAlias)
                    return true;
            }
        }
    }
    return false;
}

}  // namespace

void licm(Ctx* ctx, Fn* fn) {
    fillloop(fn);

    // Phase A hoisted load.
    int total_hoisted = 0;
    int iter = 0;
    for (; iter < 16; iter++) {
        std::vector<LoopInfo> loops;
        std::vector<int> blk_loop;
        build_loops(fn, loops, blk_loop);
        if (loops.empty())
            break;

        int round_hoisted = 0;
        for (Blk* b = fn->start; b; b = b->link) {
            if (b->id >= blk_loop.size())
                continue;
            int lidx = blk_loop[b->id];
            if (lidx < 0)
                continue;
            LoopInfo& L = loops[static_cast<size_t>(lidx)];
            if (!L.preheader)
                continue;

            for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
                if (!isload(i->op))
                    continue;
                if (!is_invariant(fn, i->arg[0], L))
                    continue;
                int sz = loadsz(i);
                if (clobbered_in_loop(fn, i, i->arg[0], sz, L))
                    continue;

                // Hit: move to preheader.
                addins(ctx, &L.preheader->ins, &L.preheader->nins, i);
                *i = make_nop();
                round_hoisted++;
            }
        }

        total_hoisted += round_hoisted;
        if (round_hoisted == 0)
            break;
        // Reset loop state.
        filluse(ctx, fn);
    }

    if (debug['L']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "\n> LICM hoisted %d load(s) in %s (in %d round%s)\n", total_hoisted, fn->name,
                iter, iter == 1 ? "" : "s");
    }
}

}  // namespace rcc
