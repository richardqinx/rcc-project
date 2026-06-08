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

/// @file unswitch.cc
/// @brief Loop Unswitching.
///
/// See docs/unswitch.md for architectural notes.

#include "rcc/all.h"

#include <cassert>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

constexpr int kUnswitchMaxBodyIns = 64;
constexpr int kUnswitchFixpointIters = 3;

// Loop Identification
struct LoopShape {
    Blk* header;
    Blk* preheader;
    Blk* latch;
    Blk* exit_bb;
    std::vector<Blk*> body;  // includes header / latch
};

// Find simple loops (derived from unroll.cc).
std::vector<LoopShape> find_simple_loops(Fn* fn) {
    std::vector<LoopShape> out;
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = ~0u;

    for (uint i = 0; i < fn->nblk; i++) {
        Blk* hd = fn->rpo[i];
        if (hd->id == ~0u)
            continue;

        int back_cnt = 0, pre_cnt = 0;
        uint latch_idx = 0, pre_idx = 0;
        for (uint k = 0; k < hd->npred; k++) {
            Blk* p = hd->pred[k];
            if (p->id == ~0u)
                continue;
            if (p->id >= i) {
                back_cnt++;
                latch_idx = k;
            } else {
                pre_cnt++;
                pre_idx = k;
            }
        }
        if (back_cnt != 1 || pre_cnt != 1)
            continue;

        Blk* latch = hd->pred[latch_idx];
        Blk* preheader = hd->pred[pre_idx];

        // Reverse DFS to collect body
        std::vector<Blk*> body = {hd};
        std::vector<Blk*> stack = {latch};
        hd->visit = hd->id;
        while (!stack.empty()) {
            Blk* n = stack.back();
            stack.pop_back();
            if (n->visit == hd->id)
                continue;
            n->visit = hd->id;
            body.push_back(n);
            for (uint k = 0; k < n->npred; k++)
                stack.push_back(n->pred[k]);
        }

        // single-exit gate
        if (hd->jmp.type != Jjnz)
            continue;
        Blk* exit_bb = hd->s2;
        if (!exit_bb || !hd->s1)
            continue;

        auto in_body = [&](Blk* x) -> bool {
            for (Blk* bb : body)
                if (bb == x)
                    return true;
            return false;
        };
        if (in_body(exit_bb))
            continue;
        if (!in_body(hd->s1))
            continue;

        bool ok = true;
        for (Blk* bb : body) {
            if (bb == hd)
                continue;
            if (isret(bb->jmp.type)) {
                ok = false;
                break;
            }
            auto check = [&](Blk* s) -> bool { return !s || in_body(s); };
            if (!check(bb->s1) || !check(bb->s2)) {
                ok = false;
                break;
            }
        }
        if (!ok)
            continue;

        LoopShape L;
        L.header = hd;
        L.preheader = preheader;
        L.latch = latch;
        L.exit_bb = exit_bb;
        L.body = std::move(body);
        out.push_back(std::move(L));
    }
    return out;
}

// Loop Invariance
bool ref_is_invariant(Fn* fn, const LoopShape& L, Ref r) {
    if (rtype(r) == RCon)
        return true;
    if (rtype(r) != RTmp)
        return false;
    if (r.val >= static_cast<uint>(fn->ntmp))
        return false;
    uint bid = fn->tmp[r.val].bid;
    if (bid == ~0u)
        return true;
    for (Blk* b : L.body)
        if (b->id == bid)
            return false;
    return true;
}

// Find a hoistable Jjnz branch. Returns nullptr if no candidate.
Blk* find_unswitch_blk(Fn* fn, const LoopShape& L) {
    for (Blk* b : L.body) {
        if (b == L.header)
            continue;  // header.Jjnz is loop exit, cannot hoist
        if (b == L.latch)
            continue;  // Jjnz on latch is usually back-edge directed, skip to simplify analysis
        if (b->jmp.type != Jjnz)
            continue;
        if (rtype(b->jmp.arg) != RTmp)
            continue;  // RCon left to simplcfg
        if (!ref_is_invariant(fn, L, b->jmp.arg))
            continue;
        if (!b->s1 || !b->s2 || b->s1 == b->s2)
            continue;
        bool s1_in = false, s2_in = false;
        for (Blk* bb : L.body) {
            if (bb == b->s1)
                s1_in = true;
            if (bb == b->s2)
                s2_in = true;
        }
        if (!s1_in || !s2_in)
            continue;
        return b;
    }
    return nullptr;
}

int count_body_ins(const LoopShape& L) {
    int n = 0;
    for (Blk* b : L.body) {
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (i->op != Onop)
                n++;
        for (Phi* p = b->phi; p; p = p->link)
            (void)p, n++;
    }
    return n;
}

// Clone Infrastructure (derived from unroll.cc with minor adaptations)
struct CopyState {
    std::unordered_map<uint, Blk*> blk_map;  // body blk.id -> clone
    std::unordered_map<uint, Ref> tmp_map;   // body-defined tmp.val -> clone Ref
};

Ref remap_tmp(Ref r, const std::unordered_map<uint, Ref>& m) {
    if (rtype(r) != RTmp)
        return r;
    auto it = m.find(r.val);
    return (it != m.end()) ? it->second : r;
}

// Generic block clone.
void clone_blk(Ctx* ctx, const CopyState& st, Blk* src, Blk* dst) {
    dst->phi = nullptr;
    Phi** tail = &dst->phi;
    for (Phi* sp = src->phi; sp; sp = sp->link) {
        Phi* np = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
        np->to = remap_tmp(sp->to, st.tmp_map);
        np->cls = sp->cls;
        np->visit = 0;
        np->narg = sp->narg;
        np->arg = static_cast<Ref*>(vnew(ctx, sp->narg, sizeof(Ref), PFn));
        np->blk = static_cast<Blk**>(vnew(ctx, sp->narg, sizeof(Blk*), PFn));
        for (uint k = 0; k < sp->narg; k++) {
            np->arg[k] = remap_tmp(sp->arg[k], st.tmp_map);
            auto it = st.blk_map.find(sp->blk[k]->id);
            np->blk[k] = (it != st.blk_map.end()) ? it->second : sp->blk[k];
        }
        np->link = nullptr;
        *tail = np;
        tail = &np->link;
    }

    dst->ins = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PFn));
    dst->nins = 0;
    for (Ins* si = src->ins; si < &src->ins[src->nins]; si++) {
        Ins ni;
        ni.op = si->op;
        ni.cls = si->cls;
        ni.to = remap_tmp(si->to, st.tmp_map);
        ni.arg[0] = remap_tmp(si->arg[0], st.tmp_map);
        ni.arg[1] = remap_tmp(si->arg[1], st.tmp_map);
        addins(ctx, &dst->ins, &dst->nins, &ni);
    }

    auto map_succ = [&](Blk* s) -> Blk* {
        if (!s)
            return nullptr;
        auto it = st.blk_map.find(s->id);
        return (it != st.blk_map.end()) ? it->second : s;
    };
    dst->jmp.type = src->jmp.type;
    dst->jmp.arg = remap_tmp(src->jmp.arg, st.tmp_map);
    dst->s1 = map_succ(src->s1);
    dst->s2 = map_succ(src->s2);
}

CopyState clone_loop(Ctx* ctx, Fn* fn, const LoopShape& L, const char* tag) {
    CopyState st;

    std::unordered_set<uint> body_defined;
    for (Blk* b : L.body) {
        for (Phi* p = b->phi; p; p = p->link)
            if (rtype(p->to) == RTmp)
                body_defined.insert(p->to.val);
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (rtype(i->to) == RTmp && i->to.val != 0)
                body_defined.insert(i->to.val);
    }

    for (Blk* b : L.body) {
        Blk* nb = newblk(ctx);
        nb->name = strf(ctx, PFn, "%s.%s", tag, b->name ? b->name : "blk");
        nb->id = fn->nblk++;
        st.blk_map[b->id] = nb;
    }

    for (uint tv : body_defined) {
        const char* prfx = fn->tmp[tv].name ? fn->tmp[tv].name : "u";
        st.tmp_map[tv] = newtmp(ctx, prfx, fn->tmp[tv].cls, fn);
    }

    for (Blk* b : L.body)
        clone_blk(ctx, st, b, st.blk_map[b->id]);

    return st;
}

// Remove dangling phi incoming edges from deactivated successors.
void remove_phi_entry(Blk* succ, Blk* old_pred) {
    for (Phi* p = succ->phi; p; p = p->link) {
        uint w = 0;
        for (uint k = 0; k < p->narg; k++) {
            if (p->blk[k] == old_pred)
                continue;
            if (w != k) {
                p->arg[w] = p->arg[k];
                p->blk[w] = p->blk[k];
            }
            w++;
        }
        p->narg = w;
    }
}

void do_unswitch(Ctx* ctx, Fn* fn, const LoopShape& L, Blk* unsw_blk) {
    Ref cond = unsw_blk->jmp.arg;
    Blk* orig_s1 = unsw_blk->s1;
    Blk* orig_s2 = unsw_blk->s2;

    CopyState A = clone_loop(ctx, fn, L, "uA");
    CopyState B = clone_loop(ctx, fn, L, "uB");

    Blk* header_A = A.blk_map.at(L.header->id);
    Blk* header_B = B.blk_map.at(L.header->id);
    Blk* unsw_A = A.blk_map.at(unsw_blk->id);
    Blk* unsw_B = B.blk_map.at(unsw_blk->id);

    // In A, collapse to true branch (orig.s1)
    unsw_A->jmp.type = Jjmp;
    unsw_A->jmp.arg = Ref::none();
    unsw_A->s1 = A.blk_map.at(orig_s1->id);
    unsw_A->s2 = nullptr;
    // orig.s2 clone in A is no longer reached by unsw_A; must remove phi edges from unsw_A
    remove_phi_entry(A.blk_map.at(orig_s2->id), unsw_A);

    // In B, collapse to false branch (orig.s2)
    unsw_B->jmp.type = Jjmp;
    unsw_B->jmp.arg = Ref::none();
    unsw_B->s1 = B.blk_map.at(orig_s2->id);
    unsw_B->s2 = nullptr;
    remove_phi_entry(B.blk_map.at(orig_s1->id), unsw_B);

    // guard block: Jjnz cond, header_A, header_B
    Blk* guard = newblk(ctx);
    guard->name = strf(ctx, PFn, "uG.%s", L.header->name ? L.header->name : "");
    guard->id = fn->nblk++;
    guard->phi = nullptr;
    guard->ins = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PFn));
    guard->nins = 0;
    guard->jmp.type = Jjnz;
    guard->jmp.arg = cond;
    guard->s1 = header_A;
    guard->s2 = header_B;

    // preheader -> guard (replaces original preheader -> header)
    if (L.preheader->s1 == L.header)
        L.preheader->s1 = guard;
    if (L.preheader->s2 == L.header)
        L.preheader->s2 = guard;

    // Update header_A / header_B phi.blk[k]==preheader to guard
    auto fix_hdr_pred = [&](Blk* hdr_clone) {
        for (Phi* p = hdr_clone->phi; p; p = p->link)
            for (uint k = 0; k < p->narg; k++)
                if (p->blk[k] == L.preheader)
                    p->blk[k] = guard;
    };
    fix_hdr_pred(header_A);
    fix_hdr_pred(header_B);

    // exit_bb phi: split blk==L.header incoming edges into two
    Blk* exit = L.exit_bb;
    for (Phi* p = exit->phi; p; p = p->link) {
        uint header_entries = 0;
        for (uint k = 0; k < p->narg; k++)
            if (p->blk[k] == L.header)
                header_entries++;
        if (header_entries == 0)
            continue;

        uint new_narg = p->narg + header_entries;  // Expand each into 2 = +1
        Ref* new_arg = static_cast<Ref*>(vnew(ctx, new_narg, sizeof(Ref), PFn));
        Blk** new_blk = static_cast<Blk**>(vnew(ctx, new_narg, sizeof(Blk*), PFn));
        uint w = 0;
        for (uint k = 0; k < p->narg; k++) {
            if (p->blk[k] == L.header) {
                new_arg[w] = remap_tmp(p->arg[k], A.tmp_map);
                new_blk[w] = header_A;
                w++;
                new_arg[w] = remap_tmp(p->arg[k], B.tmp_map);
                new_blk[w] = header_B;
                w++;
            } else {
                new_arg[w] = p->arg[k];
                new_blk[w] = p->blk[k];
                w++;
            }
        }
        p->narg = new_narg;
        p->arg = new_arg;
        p->blk = new_blk;
    }

    // link chain: preheader -> guard -> A.body -> B.body -> original preheader.link
    Blk* saved = L.preheader->link;
    L.preheader->link = guard;
    Blk** plink = &guard->link;
    for (Blk* b : L.body) {
        *plink = A.blk_map.at(b->id);
        plink = &(*plink)->link;
    }
    for (Blk* b : L.body) {
        *plink = B.blk_map.at(b->id);
        plink = &(*plink)->link;
    }
    *plink = saved;

    if (debug['W']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> unswitch %s: hdr b%u, unsw b%u, body=%zu\n", fn->name, L.header->id,
                unsw_blk->id, L.body.size());
    }
}

bool unswitch_pass(Ctx* ctx, Fn* fn) {
    auto loops = find_simple_loops(fn);
    for (auto& L : loops) {
        Blk* u = find_unswitch_blk(fn, L);
        if (!u)
            continue;
        if (count_body_ins(L) > kUnswitchMaxBodyIns)
            continue;
        do_unswitch(ctx, fn, L, u);
        return true;  // Process one per pass to avoid scanning mutated CFG
    }
    return false;
}

}  // namespace

void unswitch(Ctx* ctx, Fn* fn) {
    // Caller should have done fillcfg + filluse; filldom not needed (only Tmp.bid is used).
    int iter = 0;
    for (; iter < kUnswitchFixpointIters; iter++) {
        if (!unswitch_pass(ctx, fn))
            break;
        fillcfg(ctx, fn);
        filluse(ctx, fn);
    }
    if (debug['W'] && iter > 0) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> unswitch %s: %d round%s\n", fn->name, iter, iter == 1 ? "" : "s");
    }
}

}  // namespace rcc
