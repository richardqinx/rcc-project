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

/// @file sccp.cc
/// @brief Sparse Conditional Constant Propagation (Wegman-Zadeck).
///
/// See docs/sccp.md for architectural notes.

#include "rcc/all.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <utility>
#include <vector>

namespace rcc {

namespace {

enum LatState : uchar { Top = 0, Const = 1, Bot = 2 };

struct Lat {
    LatState state;
    Ref con;  // Only valid when state==Const, type==RCon
};

constexpr Lat kTop = {Top, {RTmp, 0}};
constexpr Lat kBot = {Bot, {RTmp, 0}};

bool con_eq(Fn* fn, Ref a, Ref b) {
    if (req(a, b))
        return true;
    if (rtype(a) != RCon || rtype(b) != RCon)
        return false;
    if (a.val >= static_cast<uint>(fn->ncon) || b.val >= static_cast<uint>(fn->ncon))
        return false;
    Con* ca = &fn->con[a.val];
    Con* cb = &fn->con[b.val];
    if (ca->type != cb->type)
        return false;
    if (ca->type == Con::CAddr && !symeq(ca->sym, cb->sym))
        return false;
    return ca->bits.i == cb->bits.i && ca->flt == cb->flt;
}

bool lat_eq(Fn* fn, Lat a, Lat b) {
    if (a.state != b.state)
        return false;
    if (a.state == Const)
        return con_eq(fn, a.con, b.con);
    return true;
}

// Check if unfoldable type.
bool op_is_unfoldable(uint op) {
    if (isstore(op))
        return true;  // store does not define tmp, but safe
    if (isload(op))
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
    if (op == Onop)
        return true;
    return false;
}

struct Sccp {
    Ctx* ctx;
    Fn* fn;
    std::vector<Lat> lat;             // ntmp
    std::vector<char> blk_reachable;  // nblk
    // edge_exec pairs.
    std::set<std::pair<uint, uint>> edge_exec;
    std::vector<Blk*> cfg_wl;
    std::vector<uint> ssa_wl;  // tmp val queue

    Lat arg_lat(Ref r) {
        if (rtype(r) == RCon)
            return {Const, r};
        if (rtype(r) == RTmp) {
            if (r.val >= lat.size())
                return kBot;
            return lat[r.val];
        }
        // RSlot / RType / RCall / RMem etc: do not participate in constant propagation
        return kBot;
    }

    void set_lat(uint t, Lat nv) {
        if (t >= lat.size() || t == 0)
            return;
        if (lat_eq(fn, lat[t], nv))
            return;
        // lattice monotonically descends: Top -> Const -> Bot
        lat[t] = nv;
        ssa_wl.push_back(t);
    }

    // Evaluate instruction using lattice.
    Lat eval_ins(Ins* i) {
        if (rtype(i->to) != RTmp)
            return kBot;
        if (op_is_unfoldable(i->op))
            return kBot;

        Lat a0 = arg_lat(i->arg[0]);
        Lat a1;
        if (req(i->arg[1], Ref::none()))
            a1 = {Const, Ref::con_z()};  // phantom arg[1] for unary op
        else
            a1 = arg_lat(i->arg[1]);

        // Ocopy special case: lattice equals arg[0]
        if (i->op == Ocopy)
            return a0;

        if (a0.state == Top || a1.state == Top)
            return kTop;
        if (a0.state == Bot || a1.state == Bot)
            return kBot;

        // Both sides are Const: try to fold
        Ins tmp = *i;
        tmp.arg[0] = a0.con;
        tmp.arg[1] = a1.con;
        Ref r = foldref(ctx, fn, &tmp);
        if (req(r, Ref::none()))
            return kBot;
        return {Const, r};
    }

    Lat eval_phi(Phi* p, Blk* in_blk) {
        Lat acc = kTop;
        for (uint k = 0; k < p->narg; k++) {
            Blk* pred = p->blk[k];
            if (!pred)
                continue;
            if (!edge_exec.count({pred->id, in_blk->id}))
                continue;  // Incoming edge unreachable - skip
            Lat a = arg_lat(p->arg[k]);
            if (a.state == Bot)
                return kBot;
            if (a.state == Top)
                continue;
            // Const
            if (acc.state == Top) {
                acc = a;
            } else if (!con_eq(fn, acc.con, a.con)) {
                return kBot;
            }
        }
        return acc;
    }

    // Activate CFG edge.
    void activate_edge(Blk* pred, Blk* succ) {
        if (!succ)
            return;
        auto key = std::make_pair(pred->id, succ->id);
        if (!edge_exec.insert(key).second)
            return;  // Already activated
        // Enqueue succ if needed.
        cfg_wl.push_back(succ);
        if (!blk_reachable[succ->id])
            blk_reachable[succ->id] = 1;
    }

    void eval_jmp(Blk* b) {
        int t = b->jmp.type;
        if (isret(t) || t == Jhlt)
            return;
        if (t == Jjmp) {
            activate_edge(b, b->s1);
            return;
        }
        if (INRANGE(t, Jjf, Jjf1)) {
            // Conditional jumps.
            activate_edge(b, b->s1);
            activate_edge(b, b->s2);
            return;
        }
        if (t == Jjnz) {
            Lat c = arg_lat(b->jmp.arg);
            if (c.state == Top)
                return;
            if (c.state == Bot) {
                activate_edge(b, b->s1);
                activate_edge(b, b->s2);
                return;
            }
            // Const: check if non-zero
            if (rtype(c.con) != RCon || c.con.val >= static_cast<uint>(fn->ncon)) {
                activate_edge(b, b->s1);
                activate_edge(b, b->s2);
                return;
            }
            Con* cc = &fn->con[c.con.val];
            if (cc->type != Con::CBits) {
                // CAddr is treated as truthy (non-null address)
                activate_edge(b, b->s1);
                return;
            }
            if (cc->bits.i != 0)
                activate_edge(b, b->s1);
            else
                activate_edge(b, b->s2);
            return;
        }
        // Other unknown jmp types: activate both sides
        activate_edge(b, b->s1);
        activate_edge(b, b->s2);
    }

    void visit_block(Blk* b) {
        // Re-eval phis.
        for (Phi* p = b->phi; p; p = p->link) {
            if (rtype(p->to) != RTmp)
                continue;
            Lat nv = eval_phi(p, b);
            set_lat(p->to.val, nv);
        }
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            if (rtype(i->to) != RTmp)
                continue;
            Lat nv = eval_ins(i);
            set_lat(i->to.val, nv);
        }
        eval_jmp(b);
    }

    // Process tmp uses.
    void visit_uses(uint t) {
        if (t >= static_cast<uint>(fn->ntmp))
            return;
        Tmp* tt = &fn->tmp[t];
        for (uint u = 0; u < tt->nuse; u++) {
            Use& uu = tt->use[u];
            if (uu.bid >= blk_reachable.size())
                continue;
            if (!blk_reachable[uu.bid])
                continue;  // In unreachable block, ignore for now
            Blk* ub = fn->rpo[uu.bid];
            if (uu.type == Use::UPhi) {
                Phi* p = uu.u.phi;
                if (rtype(p->to) == RTmp) {
                    Lat nv = eval_phi(p, ub);
                    set_lat(p->to.val, nv);
                }
            } else if (uu.type == Use::UIns) {
                Ins* i = uu.u.ins;
                if (rtype(i->to) == RTmp) {
                    Lat nv = eval_ins(i);
                    set_lat(i->to.val, nv);
                }
            } else if (uu.type == Use::UJmp) {
                eval_jmp(ub);
            }
        }
    }

    void solve() {
        // Init state.
        lat.assign(static_cast<size_t>(fn->ntmp), kTop);
        blk_reachable.assign(static_cast<size_t>(fn->nblk), 0);
        edge_exec.clear();
        cfg_wl.clear();
        ssa_wl.clear();

        blk_reachable[fn->start->id] = 1;
        cfg_wl.push_back(fn->start);
        // Enqueue start.

        while (!cfg_wl.empty() || !ssa_wl.empty()) {
            // Process worklists.
            while (!cfg_wl.empty()) {
                Blk* b = cfg_wl.back();
                cfg_wl.pop_back();
                visit_block(b);
            }
            while (!ssa_wl.empty()) {
                uint t = ssa_wl.back();
                ssa_wl.pop_back();
                visit_uses(t);
            }
        }
    }

    // Apply: Commit lattice to IR.
    Ref maybe_const(Ref r) {
        if (rtype(r) != RTmp)
            return r;
        if (r.val >= lat.size())
            return r;
        Lat& l = lat[r.val];
        if (l.state == Const)
            return l.con;
        return r;
    }

    int apply() {
        int n_ins = 0, n_phi = 0, n_jmp = 0, n_arg = 0;
        for (Blk* b = fn->start; b; b = b->link) {
            if (b->id >= blk_reachable.size() || !blk_reachable[b->id])
                continue;

            // Phi: Remove incoming edges from unreachable preds; replace arg operands
            for (Phi* p = b->phi; p; p = p->link) {
                uint w = 0;
                for (uint k = 0; k < p->narg; k++) {
                    Blk* pred = p->blk[k];
                    if (!pred || !edge_exec.count({pred->id, b->id})) {
                        // This incoming edge was not activated during SCCP -> unreachable -> delete
                        continue;
                    }
                    Ref na = maybe_const(p->arg[k]);
                    if (!req(na, p->arg[k]))
                        n_arg++;
                    if (w != k) {
                        p->arg[w] = na;
                        p->blk[w] = pred;
                    } else {
                        p->arg[w] = na;
                    }
                    w++;
                }
                p->narg = w;

                // Keep constant phi.to for DCE.
                if (rtype(p->to) == RTmp && p->to.val < lat.size() && lat[p->to.val].state == Const)
                    n_phi++;
            }

            // Ins: Change Const def to Ocopy; replace arg operands
            for (uint k = 0; k < b->nins; k++) {
                Ins* i = &b->ins[k];
                bool is_const_def = (rtype(i->to) == RTmp && i->to.val < lat.size() &&
                                     lat[i->to.val].state == Const);
                if (is_const_def && i->op != Ocopy && !op_is_unfoldable(i->op)) {
                    Ref cr = lat[i->to.val].con;
                    i->op = Ocopy;
                    i->arg[0] = cr;
                    i->arg[1] = Ref::none();
                    n_ins++;
                    continue;
                }
                // Otherwise just replace args
                Ref na0 = maybe_const(i->arg[0]);
                Ref na1 = maybe_const(i->arg[1]);
                if (!req(na0, i->arg[0])) {
                    i->arg[0] = na0;
                    n_arg++;
                }
                if (!req(na1, i->arg[1])) {
                    i->arg[1] = na1;
                    n_arg++;
                }
            }

            // jmp: Jjnz cond Const -> Jjmp; replace args
            if (b->jmp.type == Jjnz) {
                Lat c = arg_lat(b->jmp.arg);
                if (c.state == Const && rtype(c.con) == RCon &&
                    c.con.val < static_cast<uint>(fn->ncon)) {
                    Con* cc = &fn->con[c.con.val];
                    if (cc->type == Con::CBits) {
                        // Choose succ
                        Blk* taken = (cc->bits.i != 0) ? b->s1 : b->s2;
                        b->jmp.type = Jjmp;
                        b->jmp.arg = Ref::none();
                        b->s1 = taken;
                        b->s2 = nullptr;
                        n_jmp++;
                    } else if (cc->type == Con::CAddr) {
                        // Address is always truthy
                        b->jmp.type = Jjmp;
                        b->jmp.arg = Ref::none();
                        b->s1 = b->s1;
                        b->s2 = nullptr;
                        n_jmp++;
                    }
                } else {
                    Ref na = maybe_const(b->jmp.arg);
                    if (!req(na, b->jmp.arg)) {
                        b->jmp.arg = na;
                        n_arg++;
                    }
                }
            } else if (!isret(b->jmp.type) && b->jmp.type != Jhlt && b->jmp.type != Jjmp) {
                Ref na = maybe_const(b->jmp.arg);
                if (!req(na, b->jmp.arg)) {
                    b->jmp.arg = na;
                    n_arg++;
                }
            }
        }

        if (debug['S']) {
            FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
            fprintf(out, "> SCCP %s: const_ins=%d const_phi=%d jjnz_collapse=%d arg_subst=%d\n",
                    fn->name, n_ins, n_phi, n_jmp, n_arg);
        }
        return n_ins + n_phi + n_jmp + n_arg;
    }
};

}  // namespace

void sccp(Ctx* ctx, Fn* fn) {
    // Prerequisites: filluse and rpo ready.
    Sccp s;
    s.ctx = ctx;
    s.fn = fn;
    s.solve();
    s.apply();
}

}  // namespace rcc
