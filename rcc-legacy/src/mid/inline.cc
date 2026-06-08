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

/// @file inline.cc
/// @brief Function inlining (with SCC check to prevent recursion + fixpoint iteration).
///
/// See docs/inline.md for architectural notes.

#include "rcc/all.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

constexpr int kInlineLeafThreshold = 64;
constexpr int kInlineSingleCallerThreshold = 256;
constexpr int kInlineNonLeafThreshold = 48;
constexpr int kFixpointMaxIters = 16;
constexpr int kConstArgDiscount = 8;

// Metrics

bool is_leaf(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (i->op == Ocall)
                return false;
    return true;
}

int count_real_ins(Fn* fn) {
    int n = 0;
    for (Blk* b = fn->start; b; b = b->link)
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (i->op != Onop)
                n++;
    // phi also counts towards cost, one phi equals one ins
    for (Blk* b = fn->start; b; b = b->link)
        for (Phi* p = b->phi; p; p = p->link)
            n++;
    return n;
}

Fn* lookup_callee(Ref fn_ref, Fn* caller, const std::vector<Fn*>& fns) {
    if (rtype(fn_ref) != RCon)
        return nullptr;
    Con* c = &caller->con[fn_ref.val];
    if (c->type != Con::CAddr)
        return nullptr;
    const char* name = str(c->sym.id);
    for (Fn* f : fns)
        if (std::strcmp(f->name, name) == 0)
            return f;
    return nullptr;
}

std::unordered_map<std::string, int> collect_call_counts(const std::vector<Fn*>& fns) {
    std::unordered_map<std::string, int> cnt;
    for (Fn* caller : fns) {
        for (Blk* b = caller->start; b; b = b->link) {
            for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
                if (i->op != Ocall)
                    continue;
                if (rtype(i->arg[0]) != RCon)
                    continue;
                Con* c = &caller->con[i->arg[0].val];
                if (c->type != Con::CAddr)
                    continue;
                cnt[str(c->sym.id)]++;
            }
        }
    }
    return cnt;
}

// Call Graph SCC

std::unordered_map<Fn*, std::unordered_set<Fn*>> build_call_graph(const std::vector<Fn*>& fns) {
    std::unordered_map<Fn*, std::unordered_set<Fn*>> g;
    for (Fn* caller : fns) {
        g[caller];  // Ensure node exists (even without outgoing edges)
        for (Blk* b = caller->start; b; b = b->link) {
            for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
                if (i->op != Ocall)
                    continue;
                Fn* callee = lookup_callee(i->arg[0], caller, fns);
                if (callee)
                    g[caller].insert(callee);
            }
        }
    }
    return g;
}

std::unordered_set<Fn*> find_cycle_members(
    const std::vector<Fn*>& fns, const std::unordered_map<Fn*, std::unordered_set<Fn*>>& g) {
    std::unordered_set<Fn*> cyclic;
    std::unordered_map<Fn*, int> idx_of, lowlink;
    std::unordered_set<Fn*> on_stack;
    std::vector<Fn*> stk;
    int counter = 0;

    std::function<void(Fn*)> strongconnect = [&](Fn* v) {
        idx_of[v] = counter;
        lowlink[v] = counter;
        counter++;
        stk.push_back(v);
        on_stack.insert(v);

        auto it = g.find(v);
        if (it != g.end()) {
            for (Fn* w : it->second) {
                if (idx_of.find(w) == idx_of.end()) {
                    strongconnect(w);
                    lowlink[v] = std::min(lowlink[v], lowlink[w]);
                } else if (on_stack.count(w)) {
                    lowlink[v] = std::min(lowlink[v], idx_of[w]);
                }
            }
        }

        if (lowlink[v] == idx_of[v]) {
            std::vector<Fn*> scc;
            Fn* w;
            do {
                w = stk.back();
                stk.pop_back();
                on_stack.erase(w);
                scc.push_back(w);
            } while (w != v);

            if (scc.size() >= 2) {
                for (Fn* f : scc)
                    cyclic.insert(f);
            } else {
                // Single-node SCC: only count if it has self-loop (direct recursion)
                Fn* f = scc[0];
                auto it2 = g.find(f);
                if (it2 != g.end() && it2->second.count(f))
                    cyclic.insert(f);
            }
        }
    };

    for (Fn* f : fns) {
        if (idx_of.find(f) == idx_of.end())
            strongconnect(f);
    }
    return cyclic;
}

// Splice

struct RemapTables {
    std::vector<Ref> tmp;   // callee tmp id → caller Ref (RTmp)
    std::vector<Ref> con;   // callee con id → caller Ref (RCon)
    std::vector<Blk*> blk;  // callee blk id → cloned Blk* in caller
};

Ref remap_ref(Ref r, const RemapTables& m) {
    switch (rtype(r)) {
        case RTmp:
            if (r.val == 0)
                return r;  // Ref::none()
            assert(r.val < m.tmp.size());
            return m.tmp[r.val];
        case RCon:
            assert(r.val < m.con.size());
            return m.con[r.val];
        default:
            return r;  // RInt/RType/RSlot/RMem/RCall do not need remap
    }
}

struct RetEntry {
    Blk* blk;  // cloned ret block in caller
    Ref val;   // ret value (remapped), Ref::none() if void
};

void clone_blk_body(Ctx* ctx, Fn* caller, Blk* src, Blk* dst, const RemapTables& m,
                    const std::vector<Ref>& par_args, Blk* b_post, std::vector<RetEntry>& rets) {
    // phi
    Phi** ptail = &dst->phi;
    *ptail = nullptr;
    for (Phi* sp = src->phi; sp; sp = sp->link) {
        Phi* np = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
        np->to = remap_ref(sp->to, m);
        np->cls = sp->cls;
        np->visit = 0;
        np->narg = sp->narg;
        np->arg = static_cast<Ref*>(vnew(ctx, sp->narg, sizeof(Ref), PFn));
        np->blk = static_cast<Blk**>(vnew(ctx, sp->narg, sizeof(Blk*), PFn));
        for (uint k = 0; k < sp->narg; k++) {
            np->arg[k] = remap_ref(sp->arg[k], m);
            np->blk[k] = m.blk[sp->blk[k]->id];
        }
        np->link = nullptr;
        *ptail = np;
        ptail = &np->link;
    }

    // ins: Opar -> Ocopy from par_args[k]
    dst->ins = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PFn));
    dst->nins = 0;
    int par_idx = 0;
    for (Ins* si = src->ins; si < &src->ins[src->nins]; si++) {
        Ins ni;
        if (ispar(si->op)) {
            // Replace all uses of par tmp with arg ref in caller. Produces an Ocopy to maintain SSA
            // def, later copy/gvn will eliminate.
            assert(par_idx < static_cast<int>(par_args.size()));
            ni.op = Ocopy;
            ni.cls = si->cls;
            ni.to = remap_ref(si->to, m);
            ni.arg[0] = par_args[static_cast<size_t>(par_idx++)];
            ni.arg[1] = Ref::none();
        } else {
            ni.op = si->op;
            ni.cls = si->cls;
            ni.to = remap_ref(si->to, m);
            ni.arg[0] = remap_ref(si->arg[0], m);
            ni.arg[1] = remap_ref(si->arg[1], m);
        }
        addins(ctx, &dst->ins, &dst->nins, &ni);
    }

    // jmp: change isret to Jjmp pointing to b_post; pass through other jmps (s1/s2 updated by
    // m.blk)
    if (isret(src->jmp.type)) {
        rets.push_back({dst, src->jmp.type == Jret0 ? Ref::none() : remap_ref(src->jmp.arg, m)});
        dst->jmp.type = Jjmp;
        dst->jmp.arg = Ref::none();
        dst->s1 = b_post;
        dst->s2 = nullptr;
    } else {
        dst->jmp.type = src->jmp.type;
        dst->jmp.arg = remap_ref(src->jmp.arg, m);
        dst->s1 = src->s1 ? m.blk[src->s1->id] : nullptr;
        dst->s2 = src->s2 ? m.blk[src->s2->id] : nullptr;
    }
    // pred/rpo/dom etc. left for fillcfg to rebuild
    (void)caller;
}

void splice_one(Ctx* ctx, Fn* caller, Blk* b, int call_idx, Fn* callee) {
    Ins* call_ins = &b->ins[call_idx];
    Ref call_to = call_ins->to;
    int call_cls = static_cast<int>(call_ins->cls);
    (void)call_cls;

    // Collect arg_refs: scan backwards until non-isarg
    int arg_start = call_idx;
    while (arg_start > 0 && isarg(b->ins[arg_start - 1].op))
        arg_start--;
    std::vector<Ref> arg_refs;
    arg_refs.reserve(static_cast<size_t>(call_idx - arg_start));
    for (int k = arg_start; k < call_idx; k++)
        arg_refs.push_back(b->ins[k].arg[0]);

    // Three remap tables
    RemapTables m;
    m.tmp.assign(static_cast<size_t>(callee->ntmp), Ref::none());
    m.con.assign(static_cast<size_t>(callee->ncon), Ref::none());
    m.blk.assign(static_cast<size_t>(callee->nblk), nullptr);

    // Tmp: 0=none, [1..R] skipped (reg slots), use newtmp. User tmp starts at 32.
    constexpr int kFirstUserTmp =
        0;  // rcc IRGen does not use reg-tmp reservations, starts at 0; tmp[0]=R
    for (int t = 1; t < callee->ntmp; t++) {
        const char* prfx = callee->tmp[t].name ? callee->tmp[t].name : "il";
        m.tmp[static_cast<size_t>(t)] = newtmp(ctx, prfx, callee->tmp[t].cls, caller);
    }
    (void)kFirstUserTmp;
    // Con: 0=undef (fixed), 1=const_zero (rcc convention), others deep-copy
    m.con[0] = Ref::con(0);
    for (int c = 1; c < callee->ncon; c++)
        m.con[static_cast<size_t>(c)] = newcon(ctx, &callee->con[c], caller);
    // Blk: all newblk. inline_module is called before run_midend, callee->rpo not built yet,
    // traverse start->link chain; blk->id from IRGen used as remap key.
    static int splice_seq = 0;
    std::vector<Blk*> callee_blks;  // Align with traversal order for phase 4
    callee_blks.reserve(callee->nblk);
    for (Blk* cb = callee->start; cb; cb = cb->link) {
        Blk* nb = newblk(ctx);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "i%d.%s", splice_seq, cb->name ? cb->name : "blk");
        nb->name = strf(ctx, PFn, "%s", buf);
        nb->id = caller->nblk++;
        assert(cb->id < callee->nblk);
        m.blk[cb->id] = nb;
        callee_blks.push_back(cb);
    }
    ++splice_seq;

    // Cut b: new b_post, takes over b->ins[call_idx+1..] / b->jmp / s1/s2 / link
    Blk* b_post = newblk(ctx);
    {
        static int post_seq = 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "iret.%d", post_seq++);
        b_post->name = strf(ctx, PFn, "%s", buf);
        b_post->id = caller->nblk++;
    }
    b_post->phi = nullptr;
    // Take over tail ins
    int tail_n = static_cast<int>(b->nins) - call_idx - 1;
    if (tail_n > 0) {
        for (int k = call_idx + 1; k < static_cast<int>(b->nins); k++)
            addins(ctx, &b_post->ins, &b_post->nins, &b->ins[k]);
    }
    b_post->jmp = b->jmp;
    b_post->s1 = b->s1;
    b_post->s2 = b->s2;
    Blk* original_b_link = b->link;
    // Truncate b to b_pre (up to arg_start), jmp = Jjmp to callee.start_copy
    b->nins = static_cast<uint>(arg_start);
    b->jmp.type = Jjmp;
    b->jmp.arg = Ref::none();
    b->s1 = m.blk[callee->start->id];
    b->s2 = nullptr;

    // Deep copy each callee blk
    std::vector<RetEntry> rets;
    for (Blk* src : callee_blks) {
        Blk* dst = m.blk[src->id];
        clone_blk_body(ctx, caller, src, dst, m, arg_refs, b_post, rets);
    }

    // If not void, insert phi at head of b_post to merge ret value
    if (!req(call_to, Ref::none()) && !rets.empty()) {
        Phi* np = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
        np->to = call_to;
        np->cls = static_cast<short>(call_ins->cls);
        np->visit = 0;
        np->narg = static_cast<uint>(rets.size());
        np->arg = static_cast<Ref*>(vnew(ctx, np->narg, sizeof(Ref), PFn));
        np->blk = static_cast<Blk**>(vnew(ctx, np->narg, sizeof(Blk*), PFn));
        for (size_t k = 0; k < rets.size(); k++) {
            np->arg[k] = rets[k].val;
            np->blk[k] = rets[k].blk;
        }
        np->link = b_post->phi;
        b_post->phi = np;
    }

    // Fix external phi refs: phi inputs in original b's successors from b -> b_post
    auto fix_phis_in = [&](Blk* succ) {
        if (!succ)
            return;
        for (Phi* p = succ->phi; p; p = p->link)
            for (uint k = 0; k < p->narg; k++)
                if (p->blk[k] == b)
                    p->blk[k] = b_post;
    };
    fix_phis_in(b_post->s1);
    fix_phis_in(b_post->s2);

    // Link chain: b -> callee blks -> b_post -> original_b_link
    Blk* last_callee_blk = nullptr;
    b->link = m.blk[callee->start->id];  // start_copy immediately follows b
    for (Blk* src : callee_blks) {
        Blk* cur = m.blk[src->id];
        if (last_callee_blk)
            last_callee_blk->link = cur;
        last_callee_blk = cur;
    }
    assert(last_callee_blk);
    last_callee_blk->link = b_post;
    b_post->link = original_b_link;
}

// Pass driver

bool inline_pass(Ctx* ctx, std::vector<Fn*>& fns, const std::unordered_set<Fn*>& in_cycle) {
    auto call_counts = collect_call_counts(fns);
    bool any_change = false;

    for (Fn* caller : fns) {
        for (Blk* b = caller->start; b; b = b->link) {
            // Splice modifies b->ins and b->link. After a splice, b becomes b_pre, and tail ins go
            // to b_post. We only scan each b once, breaking on hit (remaining calls handled in next
            // fixpoint round, ensuring b->ins pointer stability).
            bool spliced_in_b = false;
            for (int k = 0; k < static_cast<int>(b->nins); k++) {
                if (b->ins[k].op != Ocall)
                    continue;
                Fn* callee = lookup_callee(b->ins[k].arg[0], caller, fns);
                if (!callee)
                    continue;  // extern (sylib)
                if (callee == caller)
                    continue;  // Direct self-call
                if (callee->vararg)
                    continue;
                if (in_cycle.count(callee))
                    continue;  // Direct/indirect recursion: cannot be callee
                int cost = count_real_ins(callee);
                // const-arg discount: scan backward [arg_start..k-1] for Oarg with RCon
                int arg_start = k;
                while (arg_start > 0 && isarg(b->ins[arg_start - 1].op))
                    arg_start--;
                int const_args = 0;
                for (int j = arg_start; j < k; j++)
                    if (rtype(b->ins[j].arg[0]) == RCon)
                        const_args++;
                cost = std::max(0, cost - const_args * kConstArgDiscount);
                bool single = false;
                auto it = call_counts.find(callee->name);
                if (it != call_counts.end() && it->second == 1)
                    single = true;
                int threshold;
                if (single)
                    threshold = kInlineSingleCallerThreshold;
                else if (is_leaf(callee))
                    threshold = kInlineLeafThreshold;
                else
                    threshold = kInlineNonLeafThreshold;
                if (cost > threshold)
                    continue;

                splice_one(ctx, caller, b, k, callee);
                any_change = true;
                spliced_in_b = true;
                if (debug['I']) {
                    FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
                    fprintf(out, "> inline %s into %s (cost=%d, threshold=%d)\n", callee->name,
                            caller->name, cost, threshold);
                }
                break;
            }
            if (spliced_in_b) {
                // b is now b_pre, followed by callee blks and b_post. Continue safely handles this.
                continue;
            }
        }
    }
    return any_change;
}

}  // namespace

void inline_module(Ctx* ctx, std::vector<Fn*>& fns) {
    auto graph = build_call_graph(fns);
    auto in_cycle = find_cycle_members(fns, graph);
    if (debug['I']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> inline_module: call-graph %zu fn, %zu in cycle\n", fns.size(),
                in_cycle.size());
    }
    int iter = 0;
    for (; iter < kFixpointMaxIters; iter++) {
        if (!inline_pass(ctx, fns, in_cycle))
            break;
    }
    if (debug['I']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "> inline_module: %d round%s\n", iter, iter == 1 ? "" : "s");
    }
}

}  // namespace rcc
