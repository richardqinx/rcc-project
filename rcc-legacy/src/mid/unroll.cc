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

/// @file unroll.cc
/// @brief Loop Unrolling.
///
/// See docs/unroll.md for architectural notes.

#include "rcc/all.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

constexpr int kFullUnrollTripMax = 16;
constexpr int kFullUnrollBudget = 256;
constexpr int kUnrollFixpointIters = 4;
constexpr int kPartialFactor = 4;
constexpr int kPartialMaxBodyIns = 64;  // F * body_ins <= 256 total instruction budget

// Loop Identification

struct LoopShape {
    Blk* header;
    Blk* preheader;
    Blk* latch;
    Blk* exit_bb;
    uint preheader_idx;      // index in header->pred where preheader sits
    uint latch_idx;          // index in header->pred where latch sits
    std::vector<Blk*> body;  // includes header, latch
};

// Helper: find_simple_loops (see docs/unroll.md)
std::vector<LoopShape> find_simple_loops(Fn* fn) {
    std::vector<LoopShape> out;

    for (Blk* b = fn->start; b; b = b->link)
        b->visit = ~0u;

    for (uint i = 0; i < fn->nblk; i++) {
        Blk* hd = fn->rpo[i];

        // single back-edge
        int back_cnt = 0;
        uint latch_pred_idx = 0;
        uint pre_pred_idx = 0;
        int pre_cnt = 0;
        for (uint k = 0; k < hd->npred; k++) {
            if (hd->pred[k]->id >= i && hd->pred[k]->id != ~0u) {
                back_cnt++;
                latch_pred_idx = k;
            } else {
                pre_cnt++;
                pre_pred_idx = k;
            }
        }
        if (back_cnt != 1 || pre_cnt != 1)
            continue;  // Require single latch + single preheader

        Blk* latch = hd->pred[latch_pred_idx];
        Blk* preheader = hd->pred[pre_pred_idx];

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

        // single exit (see docs/unroll.md)
        if (hd->jmp.type != Jjnz)
            continue;
        Blk* exit_bb = hd->s2;
        if (!exit_bb)
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
                continue;  // header already checked
            auto check_succ = [&](Blk* s) -> bool {
                if (!s)
                    return true;
                return in_body(s);
            };
            if (!check_succ(bb->s1) || !check_succ(bb->s2)) {
                ok = false;
                break;
            }
            // Forbid Ocall / nested ret jmp in body
            if (isret(bb->jmp.type)) {
                ok = false;
                break;
            }
            for (Ins* ii = bb->ins; ii < &bb->ins[bb->nins]; ii++) {
                if (ii->op == Ocall) {
                    ok = false;
                    break;
                }
            }
            if (!ok)
                break;
        }
        if (!ok)
            continue;
        // header itself cannot have Ocall either (although Jjnz is not ret)
        for (Ins* ii = hd->ins; ii < &hd->ins[hd->nins]; ii++) {
            if (ii->op == Ocall) {
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
        L.preheader_idx = pre_pred_idx;
        L.latch_idx = latch_pred_idx;
        L.body = std::move(body);
        out.push_back(std::move(L));
    }

    return out;
}

// Induction Variable Detection + Trip Count

struct Induction {
    int64_t init;
    int64_t step;      // > 0
    int64_t bound;     // Valid only when bound_const is true
    Ref bound_ref;     // cmp's arg[1]: RCon or loop-invariant RTmp
    bool bound_const;  // bound_ref is compile-time constant
    int cmp_op;        // Ocsltw/Ocsltl/Ocslew/Ocslel
    int cmp_cls;       // cls of cmp ins (for copying guard in partial unroll)
    Ref iv_ref;        // induction variable header phi.to (used to find corresponding new phi in
                       // main.head)
    int64_t trip;      // Calculated when bound_const is true; -1 for runtime bounds
};

bool ref_is_const(Fn* fn, Ref r, int64_t& out_val) {
    if (rtype(r) != RCon)
        return false;
    if (r.val >= fn->ncon)
        return false;
    Con* c = &fn->con[r.val];
    if (c->type != Con::CBits)
        return false;
    if (c->flt)
        return false;
    out_val = c->bits.i;
    return true;
}

bool is_one_of(int op, std::initializer_list<int> opts) {
    for (int o : opts)
        if (op == o)
            return true;
    return false;
}

// Find def of cmp. Returns Ins* or nullptr.
Ins* find_def_ins(Fn* fn, Ref r) {
    if (rtype(r) != RTmp)
        return nullptr;
    if (r.val >= fn->ntmp)
        return nullptr;
    return fn->tmp[r.val].def;
}

// Check if ref is defined outside the loop (including constants). Uses Tmp.bid (set by filluse).
bool def_outside_body(Fn* fn, const LoopShape& L, Ref r) {
    if (rtype(r) != RTmp)
        return true;  // Constants / non-tmps are loop-invariant
    uint bid = fn->tmp[r.val].bid;
    for (Blk* bb : L.body)
        if (bb->id == bid)
            return false;
    return true;
}

// Resolve self-loop phis to their loop-invariant preheader value.
Ref resolve_loop_invariant(Fn* fn, const LoopShape& L, Ref r) {
    for (int guard = 0; guard < 8; guard++) {
        if (def_outside_body(fn, L, r))
            return r;
        // Defined inside loop: can only resolve further if it's a self-loop phi in header
        if (rtype(r) != RTmp || fn->tmp[r.val].bid != L.header->id)
            return Ref::none();
        Phi* p = nullptr;
        for (Phi* q = L.header->phi; q; q = q->link)
            if (req(q->to, r)) {
                p = q;
                break;
            }
        if (!p)
            return Ref::none();  // Inside header but not a phi (it's a regular ins) -> true
                                 // variable
        Ref pre_arg = Ref::none();
        bool selfish = true;
        for (uint k = 0; k < p->narg; k++) {
            if (p->blk[k] == L.preheader)
                pre_arg = p->arg[k];
            else if (!req(p->arg[k], p->to))
                selfish = false;  // Non-self-loop incoming edge -> actually varying
        }
        if (!selfish || req(pre_arg, Ref::none()))
            return Ref::none();
        r = pre_arg;  // Resolved one level, continue (preheader value might also be a self-loop
                      // phi)
    }
    return Ref::none();
}

bool detect_induction(Fn* fn, const LoopShape& L, Induction& out) {
    // Header must end with Jjnz, arg = cmp tmp
    if (rtype(L.header->jmp.arg) != RTmp)
        return false;
    Ins* cmp = find_def_ins(fn, L.header->jmp.arg);
    if (!cmp)
        return false;
    if (!is_one_of(cmp->op, {Ocsltw, Ocsltl, Ocslew, Ocslel}))
        return false;

    // Parse bound (see docs/unroll.md)
    if (rtype(cmp->arg[0]) != RTmp)
        return false;
    Ref bound_ref = resolve_loop_invariant(fn, L, cmp->arg[1]);
    if (req(bound_ref, Ref::none()))
        return false;
    int64_t bound_v = 0;
    bool bound_const = ref_is_const(fn, bound_ref, bound_v);

    // Find matching header phi
    Phi* ind_phi = nullptr;
    for (Phi* p = L.header->phi; p; p = p->link) {
        if (req(p->to, cmp->arg[0])) {
            ind_phi = p;
            break;
        }
    }
    if (!ind_phi)
        return false;

    // Get init (preheader side) and i_next (latch side)
    if (ind_phi->narg != L.header->npred)
        return false;  // Defensive check
    Ref init_ref = Ref::none(), i_next_ref = Ref::none();
    for (uint k = 0; k < ind_phi->narg; k++) {
        if (ind_phi->blk[k] == L.preheader)
            init_ref = ind_phi->arg[k];
        else if (ind_phi->blk[k] == L.latch)
            i_next_ref = ind_phi->arg[k];
    }
    int64_t init_v;
    if (!ref_is_const(fn, init_ref, init_v))
        return false;
    if (rtype(i_next_ref) != RTmp)
        return false;

    // i_next def must be Oadd(i, step_const) or Oadd(step_const, i)
    Ins* next_def = find_def_ins(fn, i_next_ref);
    if (!next_def)
        return false;
    if (next_def->op != Oadd)
        return false;
    int64_t step_v = 0;
    if (req(next_def->arg[0], ind_phi->to) && ref_is_const(fn, next_def->arg[1], step_v)) {
        // i_next = i + step
    } else if (req(next_def->arg[1], ind_phi->to) && ref_is_const(fn, next_def->arg[0], step_v)) {
        // i_next = step + i (commutative)
    } else {
        return false;
    }
    if (step_v <= 0)
        return false;  // Only positive step supported for now

    out.init = init_v;
    out.step = step_v;
    out.bound = bound_v;
    out.bound_ref = bound_ref;
    out.bound_const = bound_const;
    out.cmp_op = cmp->op;
    out.cmp_cls = cmp->cls;
    out.iv_ref = ind_phi->to;

    if (!bound_const) {
        out.trip = -1;  // Runtime bound: only usable for partial unroll
        return true;
    }

    // trip count depends on cmp_op
    int64_t span = bound_v - init_v;
    if (cmp->op == Ocslew || cmp->op == Ocslel)
        span += 1;
    if (span <= 0) {
        out.trip = 0;
        return true;
    }
    out.trip = (span + step_v - 1) / step_v;
    return true;
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

// Clone / Unroll

struct CopyState {
    std::unordered_map<uint, Blk*> blk_map;  // body blk.id -> clone
    std::unordered_map<uint, Ref> tmp_map;   // body-defined tmp.val -> clone Ref
    Blk* first_blk;                          // = blk_map[L.header.id]
    Blk* last_blk;                           // = blk_map[L.latch.id]
};

Ref remap_tmp(Ref r, const std::unordered_map<uint, Ref>& m) {
    if (rtype(r) != RTmp)
        return r;
    auto it = m.find(r.val);
    if (it == m.end())
        return r;
    return it->second;
}

// Deep copy a body block into dst. Discard header phis, map header Jjnz to Jjmp.
void clone_body_blk(Ctx* ctx, const CopyState& st, Blk* src, Blk* dst, bool is_header,
                    bool is_latch) {
    // ── phi ──
    dst->phi = nullptr;
    if (!is_header) {
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
    }

    // ── ins ──
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

    // ── jmp ──
    if (is_header) {
        // Jjnz cmp, body_first, exit -> Jjmp blk_map[body_first.id]
        dst->jmp.type = Jjmp;
        dst->jmp.arg = Ref::none();
        auto it = st.blk_map.find(src->s1->id);
        assert(it != st.blk_map.end());
        dst->s1 = it->second;
        dst->s2 = nullptr;
    } else if (is_latch) {
        // Kept for now, caller rewrites when linking across copies
        dst->jmp.type = Jjmp;
        dst->jmp.arg = Ref::none();
        dst->s1 = nullptr;
        dst->s2 = nullptr;
    } else {
        dst->jmp.type = src->jmp.type;
        dst->jmp.arg = remap_tmp(src->jmp.arg, st.tmp_map);
        dst->s1 = nullptr;
        dst->s2 = nullptr;
        if (src->s1) {
            auto it = st.blk_map.find(src->s1->id);
            dst->s1 = (it != st.blk_map.end()) ? it->second : src->s1;
        }
        if (src->s2) {
            auto it = st.blk_map.find(src->s2->id);
            dst->s2 = (it != st.blk_map.end()) ? it->second : src->s2;
        }
    }
}

// Clone body N times sequentially to replace original loop. Matching by Blk* pointer is the only
// robust way.
Ref phi_arg_for_pred(Phi* p, Blk* pred) {
    for (uint k = 0; k < p->narg; k++)
        if (p->blk[k] == pred)
            return p->arg[k];
    die("phi_arg_for_pred: pred not found in phi.blk[]");
}

// Prerequisites: detect_induction successful, N = induction.trip.
void full_unroll(Ctx* ctx, Fn* fn, const LoopShape& L, int64_t N) {
    // Collect body-defined tmps
    std::unordered_set<uint> body_defined;
    for (Blk* b : L.body) {
        for (Phi* p = b->phi; p; p = p->link)
            if (rtype(p->to) == RTmp)
                body_defined.insert(p->to.val);
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (rtype(i->to) == RTmp && i->to.val != 0)
                body_defined.insert(i->to.val);
    }

    // Collect header phi list (in linked list order, to re-define them later)
    std::vector<Phi*> header_phis;
    for (Phi* p = L.header->phi; p; p = p->link)
        header_phis.push_back(p);

    // Clone for each iteration
    std::vector<CopyState> copies(static_cast<size_t>(N));

    // Map previous latch tmps
    std::unordered_map<uint, Ref> prev_latch_tmp_map;

    static int unroll_seq = 0;
    int my_seq = unroll_seq++;

    for (int64_t k = 0; k < N; k++) {
        CopyState& st = copies[static_cast<size_t>(k)];

        // Allocate new blk for each body block
        for (Blk* b : L.body) {
            Blk* nb = newblk(ctx);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "u%d.%lld.%s", my_seq, static_cast<long long>(k),
                          b->name ? b->name : "blk");
            nb->name = strf(ctx, PFn, "%s", buf);
            nb->id = fn->nblk++;
            st.blk_map[b->id] = nb;
        }
        st.first_blk = st.blk_map[L.header->id];
        st.last_blk = st.blk_map[L.latch->id];

        // Assign fresh ids to body-defined tmps; then populate header phi.tos with entry values
        for (uint tv : body_defined) {
            const char* prfx = fn->tmp[tv].name ? fn->tmp[tv].name : "u";
            st.tmp_map[tv] = newtmp(ctx, prfx, fn->tmp[tv].cls, fn);
        }
        for (Phi* p : header_phis) {
            Ref entry_val;
            if (k == 0) {
                entry_val = phi_arg_for_pred(p, L.preheader);
            } else {
                Ref prev_latch_ref = phi_arg_for_pred(p, L.latch);
                entry_val = remap_tmp(prev_latch_ref, prev_latch_tmp_map);
            }
            if (rtype(p->to) == RTmp)
                st.tmp_map[p->to.val] = entry_val;
        }

        // Clone each body block
        for (Blk* b : L.body) {
            Blk* dst = st.blk_map[b->id];
            bool is_h = (b == L.header);
            bool is_l = (b == L.latch);
            // Single-block loop specialization
            clone_body_blk(ctx, st, b, dst, is_h, is_l && !is_h);
        }

        // Save current tmp_map for next round
        prev_latch_tmp_map = st.tmp_map;
    }

    // Link copies: copy_k's latch_clone.jmp = Jjmp copy_{k+1}.first
    for (int64_t k = 0; k < N - 1; k++) {
        Blk* lc = copies[static_cast<size_t>(k)].last_blk;
        Blk* nh = copies[static_cast<size_t>(k + 1)].first_blk;
        // Single-block loop self-loop fix: rewrite last_blk's Jjmp to point to next copy
        lc->jmp.type = Jjmp;
        lc->jmp.arg = Ref::none();
        lc->s1 = nh;
        lc->s2 = nullptr;
    }

    // Last round's latch_clone: append Ocopy to re-def header phi.tos, jmp points to exit_bb
    {
        CopyState& last = copies[static_cast<size_t>(N - 1)];
        Blk* lc = last.last_blk;
        for (Phi* p : header_phis) {
            Ref final_val = remap_tmp(phi_arg_for_pred(p, L.latch), last.tmp_map);
            Ins re_def;
            re_def.op = Ocopy;
            re_def.cls = static_cast<uint>(p->cls) & 3u;
            re_def.to = p->to;  // Original header phi.to (now dead, SSA def monopolized)
            re_def.arg[0] = final_val;
            re_def.arg[1] = Ref::none();
            addins(ctx, &lc->ins, &lc->nins, &re_def);
        }
        lc->jmp.type = Jjmp;
        lc->jmp.arg = Ref::none();
        lc->s1 = L.exit_bb;
        lc->s2 = nullptr;
    }

    // Point preheader to copy_0.first; clear original header's pred references (leave for fillcfg
    // to rebuild)
    Blk* new_entry = copies[0].first_blk;
    if (L.preheader->s1 == L.header)
        L.preheader->s1 = new_entry;
    if (L.preheader->s2 == L.header)
        L.preheader->s2 = new_entry;

    // Fix exit_bb phi incoming edges: replace header with last copy's latch_clone
    Blk* new_last = copies[static_cast<size_t>(N - 1)].last_blk;
    for (Phi* p = L.exit_bb->phi; p; p = p->link) {
        for (uint kk = 0; kk < p->narg; kk++) {
            if (p->blk[kk] == L.header)
                p->blk[kk] = new_last;
        }
    }

    // Link chain: preheader -> copy_0_blocks -> ... -> copy_{N-1}_blocks -> original preheader.link
    Blk* saved_link = L.preheader->link;
    Blk** plink = &L.preheader->link;
    for (int64_t k = 0; k < N; k++) {
        const CopyState& st = copies[static_cast<size_t>(k)];
        // Link in L.body order
        for (Blk* b : L.body) {
            Blk* nb = st.blk_map.at(b->id);
            *plink = nb;
            plink = &nb->link;
        }
    }
    *plink = saved_link;

    if (debug['U']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> unroll full N=%lld in %s (header b%u, body=%zu, ins=%d)\n",
                static_cast<long long>(N), fn->name, L.header->id, L.body.size(),
                count_body_ins(L));
    }
}

// Partial Unroll

Blk* partial_unroll(Ctx* ctx, Fn* fn, const LoopShape& L, const Induction& ind, int F) {
    // body-defined tmps
    std::unordered_set<uint> body_defined;
    for (Blk* b : L.body) {
        for (Phi* p = b->phi; p; p = p->link)
            if (rtype(p->to) == RTmp)
                body_defined.insert(p->to.val);
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (rtype(i->to) == RTmp && i->to.val != 0)
                body_defined.insert(i->to.val);
    }

    // header phi list
    std::vector<Phi*> header_phis;
    for (Phi* p = L.header->phi; p; p = p->link)
        header_phis.push_back(p);

    static int unroll_seq_p = 0;
    int my_seq = unroll_seq_p++;

    // Create main_head, build its phi list first (loopback edge filled later)
    Blk* main_head = newblk(ctx);
    main_head->name = strf(ctx, PFn, "u%d.main.head", my_seq);
    main_head->id = fn->nblk++;

    std::unordered_map<uint, Ref> head_to_main;  // original phi.to.val -> main_head phi.to
    std::vector<Phi*> main_phis;
    Phi** ptail = &main_head->phi;
    for (Phi* p : header_phis) {
        Phi* pm = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
        pm->cls = p->cls;
        pm->visit = 0;
        const char* prfx =
            (rtype(p->to) == RTmp && fn->tmp[p->to.val].name) ? fn->tmp[p->to.val].name : "u";
        pm->to = newtmp(ctx, prfx, p->cls, fn);
        pm->narg = 2;
        pm->arg = static_cast<Ref*>(vnew(ctx, 2, sizeof(Ref), PFn));
        pm->blk = static_cast<Blk**>(vnew(ctx, 2, sizeof(Blk*), PFn));
        pm->arg[0] = phi_arg_for_pred(p, L.preheader);
        pm->blk[0] = L.preheader;
        pm->arg[1] = Ref::none();
        pm->blk[1] = nullptr;
        pm->link = nullptr;
        *ptail = pm;
        ptail = &pm->link;
        main_phis.push_back(pm);
        if (rtype(p->to) == RTmp)
            head_to_main[p->to.val] = pm->to;
    }

    // Clone body F times, same as full_unroll (except clone0 entry uses main_head phi.to)
    std::vector<CopyState> copies(static_cast<size_t>(F));
    std::unordered_map<uint, Ref> prev_latch_tmp_map;
    for (int k = 0; k < F; k++) {
        CopyState& st = copies[static_cast<size_t>(k)];
        for (Blk* b : L.body) {
            Blk* nb = newblk(ctx);
            nb->name = strf(ctx, PFn, "u%d.%d.%s", my_seq, k, b->name ? b->name : "blk");
            nb->id = fn->nblk++;
            st.blk_map[b->id] = nb;
        }
        st.first_blk = st.blk_map[L.header->id];
        st.last_blk = st.blk_map[L.latch->id];
        for (uint tv : body_defined) {
            const char* prfx = fn->tmp[tv].name ? fn->tmp[tv].name : "u";
            st.tmp_map[tv] = newtmp(ctx, prfx, fn->tmp[tv].cls, fn);
        }
        for (Phi* p : header_phis) {
            Ref entry_val;
            if (k == 0) {
                auto it = head_to_main.find(p->to.val);
                entry_val =
                    (it != head_to_main.end()) ? it->second : phi_arg_for_pred(p, L.preheader);
            } else {
                entry_val = remap_tmp(phi_arg_for_pred(p, L.latch), prev_latch_tmp_map);
            }
            if (rtype(p->to) == RTmp)
                st.tmp_map[p->to.val] = entry_val;
        }
        for (Blk* b : L.body) {
            Blk* dst = st.blk_map[b->id];
            bool is_h = (b == L.header);
            bool is_l = (b == L.latch);
            clone_body_blk(ctx, st, b, dst, is_h, is_l && !is_h);
        }
        prev_latch_tmp_map = st.tmp_map;
    }

    // Link: copy_k.last -> copy_{k+1}.first
    for (int k = 0; k < F - 1; k++) {
        Blk* lc = copies[static_cast<size_t>(k)].last_blk;
        lc->jmp.type = Jjmp;
        lc->jmp.arg = Ref::none();
        lc->s1 = copies[static_cast<size_t>(k + 1)].first_blk;
        lc->s2 = nullptr;
    }
    // Last latch -> main_head (back-edge)
    Blk* last_latch = copies[static_cast<size_t>(F - 1)].last_blk;
    last_latch->jmp.type = Jjmp;
    last_latch->jmp.arg = Ref::none();
    last_latch->s1 = main_head;
    last_latch->s2 = nullptr;

    // Fill main_head phi's loopback incoming edge
    for (size_t idx = 0; idx < header_phis.size(); idx++) {
        Phi* p = header_phis[idx];
        Phi* pm = main_phis[idx];
        pm->arg[1] =
            remap_tmp(phi_arg_for_pred(p, L.latch), copies[static_cast<size_t>(F - 1)].tmp_map);
        pm->blk[1] = last_latch;
    }

    // main_head guard: adj_bound = bound - (F-1)*step; cmp(i_main, adj)
    main_head->ins = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PFn));
    main_head->nins = 0;
    int kls = (ind.cmp_op == Ocsltl || ind.cmp_op == Ocslel) ? Kl : Kw;
    int64_t C = static_cast<int64_t>(F - 1) * ind.step;
    Ref adj_ref;
    if (ind.bound_const) {
        adj_ref = getcon(ctx, ind.bound - C, fn);
    } else {
        Ref adj_tmp = newtmp(ctx, "u", kls, fn);
        Ins sub;
        sub.op = Osub;
        sub.cls = static_cast<uint>(kls) & 3u;
        sub.to = adj_tmp;
        sub.arg[0] = ind.bound_ref;
        sub.arg[1] = getcon(ctx, C, fn);
        addins(ctx, &main_head->ins, &main_head->nins, &sub);
        adj_ref = adj_tmp;
    }
    Ref iv_main = head_to_main.at(ind.iv_ref.val);
    Ref gcmp = newtmp(ctx, "u", Kw, fn);
    Ins cmpi;
    cmpi.op = static_cast<uint>(ind.cmp_op);
    cmpi.cls = static_cast<uint>(ind.cmp_cls) & 3u;
    cmpi.to = gcmp;
    cmpi.arg[0] = iv_main;
    cmpi.arg[1] = adj_ref;
    addins(ctx, &main_head->ins, &main_head->nins, &cmpi);
    main_head->jmp.type = Jjnz;
    main_head->jmp.arg = gcmp;
    main_head->s1 = copies[0].first_blk;
    main_head->s2 = L.header;  // remainder loop

    // preheader -> main_head
    if (L.preheader->s1 == L.header)
        L.preheader->s1 = main_head;
    if (L.preheader->s2 == L.header)
        L.preheader->s2 = main_head;

    // Remainder header phi's preheader edge: replace with main_head, value is pm.to
    for (size_t idx = 0; idx < header_phis.size(); idx++) {
        Phi* p = header_phis[idx];
        Phi* pm = main_phis[idx];
        for (uint kk = 0; kk < p->narg; kk++) {
            if (p->blk[kk] == L.preheader) {
                p->blk[kk] = main_head;
                p->arg[kk] = pm->to;
            }
        }
    }

    // link chain: preheader -> main_head -> clones -> original preheader.link
    Blk* saved_link = L.preheader->link;
    L.preheader->link = main_head;
    Blk** plink = &main_head->link;
    for (int k = 0; k < F; k++) {
        for (Blk* b : L.body) {
            Blk* nb = copies[static_cast<size_t>(k)].blk_map.at(b->id);
            *plink = nb;
            plink = &nb->link;
        }
    }
    *plink = saved_link;

    if (debug['U']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> unroll partial F=%d in %s (header b%u, body=%zu, runtime_bound=%d)\n", F,
                fn->name, L.header->id, L.body.size(), ind.bound_const ? 0 : 1);
    }
    return main_head;
}

// Pass driver

bool unroll_pass(Ctx* ctx, Fn* fn, std::unordered_set<Blk*>& unrolled) {
    auto loops = find_simple_loops(fn);
    bool any = false;

    for (auto& L : loops) {
        // Prevent infinite partial unrolling by skipping already processed main_heads.
        if (unrolled.count(L.header))
            continue;

        Induction ind;
        if (!detect_induction(fn, L, ind))
            continue;
        int64_t N = ind.trip;
        int body_ins = count_body_ins(L);

        // Prioritize full unroll: trip known and <= kFullUnrollTripMax, and N*body within budget
        if (N >= 1 && N <= kFullUnrollTripMax &&
            static_cast<int64_t>(body_ins) * N <= kFullUnrollBudget) {
            full_unroll(ctx, fn, L, N);
            any = true;
            break;
        }

        if (N == 0)
            continue;  // Loop doesn't execute, leave to simplcfg

        // Otherwise try partial unroll: N=-1 (runtime bound) or N > kFullUnrollTripMax
        // Skip if body is too large (F*body limit kPartialMaxBodyIns*F = 256 ins).
        if (body_ins <= 0 || body_ins > kPartialMaxBodyIns)
            continue;
        Blk* mh = partial_unroll(ctx, fn, L, ind, kPartialFactor);
        unrolled.insert(mh);
        any = true;
        break;
    }
    return any;
}

}  // namespace

void unroll(Ctx* ctx, Fn* fn) {
    // Caller guarantees fillcfg + filldom + filluse are ready.
    std::unordered_set<Blk*>
        unrolled;  // main_heads already partial-unrolled, persistent across fixpoints
    int iter = 0;
    for (; iter < kUnrollFixpointIters; iter++) {
        if (!unroll_pass(ctx, fn, unrolled))
            break;
        // CFG modified, rebuild for next round.
        fillcfg(ctx, fn);
        filluse(ctx, fn);
        filldom(fn);
    }
    if (debug['U'] && iter > 0) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> unroll %s: %d round%s\n", fn->name, iter, iter == 1 ? "" : "s");
    }
}

}  // namespace rcc
