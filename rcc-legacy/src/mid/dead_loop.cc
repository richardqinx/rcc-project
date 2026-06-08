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

/// @file dead_loop.cc
/// @brief Dead Loop Elimination: removal of entire loops that have no side-effects, are provably
/// terminating, and have no live-outs.
///
/// See docs/dead_loop.md for architectural notes.

#include "rcc/all.h"
#include "rcc/loopinfo.h"

#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

bool has_side_effect_op(uint op) {
    if (isstore(op))
        return true;
    if (isalloc(op))
        return true;
    if (ispar(op))
        return true;
    if (isarg(op))
        return true;
    if (op == Ocall)
        return true;
    if (op == Oblit0 || op == Oblit1)
        return true;
    if (op == Ovastart || op == Ovaarg)
        return true;
    if (op == Odbgloc)
        return true;
    if (op == Osel0 || op == Osel1)
        return true;
    return false;
}

bool defined_outside(Fn* fn, const Loop& L, Ref r) {
    if (rtype(r) == RCon)
        return true;
    if (rtype(r) != RTmp)
        return false;
    if (r.val >= static_cast<uint>(fn->ntmp))
        return false;
    uint bid = fn->tmp[r.val].bid;
    return L.body_bid.find(bid) == L.body_bid.end();
}

Ins* find_cmp_in(Blk* b, Ref cond) {
    for (uint i = 0; i < b->nins; i++)
        if (req(b->ins[i].to, cond))
            return &b->ins[i];
    return nullptr;
}

int find_biv_idx(const std::vector<Biv>& bivs, Ref r) {
    for (size_t k = 0; k < bivs.size(); k++)
        if (req(bivs[k].to, r))
            return static_cast<int>(k);
    return -1;
}

void collect_body_defs(const Loop& L, std::vector<Ref>& defs) {
    for (Blk* b : L.body) {
        for (Phi* p = b->phi; p; p = p->link)
            if (rtype(p->to) == RTmp)
                defs.push_back(p->to);
        for (uint k = 0; k < b->nins; k++)
            if (rtype(b->ins[k].to) == RTmp)
                defs.push_back(b->ins[k].to);
    }
}

bool all_uses_inside(Fn* fn, const Loop& L, Ref tmp_ref) {
    if (rtype(tmp_ref) != RTmp)
        return true;
    if (tmp_ref.val >= static_cast<uint>(fn->ntmp))
        return true;
    Tmp* t = &fn->tmp[tmp_ref.val];
    for (uint u = 0; u < t->nuse; u++) {
        const Use& uu = t->use[u];
        if (L.body_bid.find(uu.bid) == L.body_bid.end())
            return false;
    }
    return true;
}

bool delete_loop_if_dead(Fn* fn, const Loop& L) {
    // Topology: unique exit_block + unique exiting_block
    Blk* exit_block = nullptr;
    Blk* exiting_block = nullptr;
    for (Blk* b : L.body) {
        Blk* succs[2] = {b->s1, b->s2};
        for (Blk* s : succs) {
            if (!s)
                continue;
            if (L.body_bid.find(s->id) != L.body_bid.end())
                continue;  // Internal edge
            // Exit loop edge
            if (!exit_block) {
                exit_block = s;
                exiting_block = b;
            } else if (exit_block != s || exiting_block != b) {
                return false;  // Multiple exits or multiple exiting blocks
            }
        }
    }
    if (!exit_block)
        return false;  // No exit -- infinite loop

    // Termination: at least one constant BIV + header Jjnz cmp(BIV, invariant)
    std::vector<Biv> bivs;
    detect_bivs(fn, L, bivs);
    if (bivs.empty())
        return false;
    if (L.header->jmp.type != Jjnz)
        return false;
    Ref cond = L.header->jmp.arg;
    if (rtype(cond) != RTmp)
        return false;
    Ins* cmp = find_cmp_in(L.header, cond);
    if (!cmp)
        return false;
    int bi = find_biv_idx(bivs, cmp->arg[0]);
    Ref other = cmp->arg[1];
    if (bi < 0) {
        bi = find_biv_idx(bivs, cmp->arg[1]);
        other = cmp->arg[0];
    }
    if (bi < 0)
        return false;
    if (!defined_outside(fn, L, other))
        return false;
    if (exiting_block != L.header)
        return false;

    // Side-effect scan
    for (Blk* b : L.body) {
        for (uint k = 0; k < b->nins; k++) {
            if (has_side_effect_op(b->ins[k].op))
                return false;
        }
    }

    // Live-out scan
    for (Phi* p = exit_block->phi; p; p = p->link) {
        for (uint k = 0; k < p->narg; k++) {
            if (p->blk[k] != exiting_block)
                continue;
            if (!defined_outside(fn, L, p->arg[k]))
                return false;
        }
    }
    std::vector<Ref> defs;
    collect_body_defs(L, defs);
    for (Ref r : defs) {
        if (!all_uses_inside(fn, L, r))
            return false;
    }

    for (Phi* p = exit_block->phi; p; p = p->link) {
        for (uint k = 0; k < p->narg; k++)
            if (p->blk[k] == L.preheader)
                return false;
    }

    // Rewrite
    // preheader's jmp outgoing edge: the one pointing to header is changed to point to exit_block;
    if (L.preheader->s1 == L.header)
        L.preheader->s1 = exit_block;
    if (L.preheader->s2 == L.header)
        L.preheader->s2 = exit_block;
    for (Phi* p = exit_block->phi; p; p = p->link) {
        for (uint k = 0; k < p->narg; k++)
            if (p->blk[k] == exiting_block)
                p->blk[k] = L.preheader;
    }

    return true;
}

}  // namespace

void dead_loop(Ctx* ctx, Fn* fn) {
    fillpreds(ctx, fn);
    fillcfg(ctx, fn);
    filluse(ctx, fn);

    std::vector<Loop> loops;
    find_loops(fn, loops);
    if (loops.empty())
        return;

    int deleted = 0;
    for (const Loop& L : loops) {
        if (delete_loop_if_dead(fn, L))
            deleted++;
    }

    if (debug['L']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "\n> dead_loop %s: deleted %d of %zu loop(s)\n", fn->name, deleted,
                loops.size());
    }
}

}  // namespace rcc
