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

/// @file reduction_unroll.cc
/// @brief Identifies inner loop integer reductions and unrolls them into K-way parallel
/// accumulators.
///
/// See docs/reduction_unroll.md for architectural notes.

#include "rcc/all.h"
#include "rcc/loopinfo.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

constexpr int kUnrollK = 4;

int load_bytes(int op, int cls) {
    switch (op) {
        case Oloadsb:
        case Oloadub:
            return 1;
        case Oloadsh:
        case Oloaduh:
            return 2;
        case Oloadsw:
        case Oloaduw:
            return 4;
        case Oload:
            return (cls == Kl || cls == Kd) ? 8 : 4;
        default:
            return 0;
    }
}

bool is_int_reduce_op(int op) {
    return op == Oadd || op == Omul || op == Oand || op == Oor || op == Oxor;
}

int64_t identity_for(int op) {
    switch (op) {
        case Oadd:
        case Oor:
        case Oxor:
            return 0;
        case Omul:
            return 1;
        case Oand:
            return -1;
        default:
            return 0;
    }
}

bool is_int_cls(int cls) {
    return cls == Kw || cls == Kl;
}

struct Match {
    PointerIV iv_p;  // pointer IV
    Phi* phi_p;      // pointer phi
    Phi* phi_s;      // accumulator phi
    Ref s_init;      // accumulator init (preheader side)
    Ref s_next;      // accumulator next (latch side, = s_op.to)
    Ins* load_ins;   // load inside body
    Ins* op_ins;     // reduce op inside body (s.next = OP s, v)
    Ins* p_inc_ins;  // pointer increment inside body
    Ref end;         // end ref for exit cmp
    Blk* after;      // exit block
    int op;          // op_ins->op
    int s_cls;       // accumulator and op cls
    int load_op;     // load_ins->op
    int load_cls;    // load_ins->cls
};

// Check latch body.
bool scan_body(Fn* /*fn*/, const Loop& L, const PointerIV& iv_p, Phi* phi_s, Match* m) {
    m->load_ins = nullptr;
    m->op_ins = nullptr;
    m->p_inc_ins = nullptr;

    Ref p = iv_p.phi_to;
    Ref s = phi_s->to;
    Ref s_next = Ref::none(), p_next = iv_p.next;
    for (uint k = 0; k < phi_s->narg; k++)
        if (phi_s->blk[k] == L.latch)
            s_next = phi_s->arg[k];
    if (rtype(s_next) != RTmp)
        return false;

    for (Blk* bb : L.body) {
        for (uint k = 0; k < bb->nins; k++) {
            Ins* i = &bb->ins[k];
            int op = i->op;
            // header allows exit cmp (guaranteed unique by check_exit_cmp)
            if (bb == L.header && req(i->to, L.header->jmp.arg))
                continue;
            if (op == Onop)
                continue;
            if (op == Ocall || op == Oalloc4 || op == Oalloc8 || op == Oalloc16 || op == Ovaarg ||
                op == Ovastart || op == Odbgloc || op == Osel0 || op == Osel1 || op == Oblit0 ||
                op == Oblit1 || op == Osalloc || op == Oudiv || op == Ourem || op == Odiv ||
                op == Orem)
                return false;
            // pointer increment?
            if (req(i->to, p_next)) {
                if (m->p_inc_ins)
                    return false;
                m->p_inc_ins = i;
                continue;
            }
            // accumulator increment?
            if (req(i->to, s_next)) {
                if (m->op_ins)
                    return false;
                if (!is_int_reduce_op(op))
                    return false;
                if (!is_int_cls(static_cast<int>(i->cls)))
                    return false;
                // Must be OP(s, v) or OP(v, s)
                if (!req(i->arg[0], s) && !req(i->arg[1], s))
                    return false;
                m->op_ins = i;
                continue;
            }
            // Is it a load?
            int lb = load_bytes(op, static_cast<int>(i->cls));
            if (lb > 0) {
                if (m->load_ins)
                    return false;
                if (!req(i->arg[0], p))
                    return false;
                m->load_ins = i;
                continue;
            }
            // Reject any other ins (reduction form requires a clean body)
            return false;
        }
    }
    if (!m->load_ins || !m->op_ins || !m->p_inc_ins)
        return false;
    // Loaded value must equal the other operand of the OP
    Ref v_ref = m->load_ins->to;
    if (!(req(m->op_ins->arg[0], v_ref) || req(m->op_ins->arg[1], v_ref)))
        return false;
    // class & width
    int lb = load_bytes(m->load_ins->op, static_cast<int>(m->load_ins->cls));
    if (lb != iv_p.step)
        return false;
    if (m->op_ins->cls != m->load_ins->cls)
        return false;
    m->s_next = s_next;
    return true;
}

bool try_match_loop(Fn* fn, const Loop& L, Match* m) {
    if (L.body.size() > 2)
        return false;
    if (!L.preheader)
        return false;
    if (L.preheader->jmp.type != Jjmp)
        return false;
    if (L.preheader->s1 != L.header || L.preheader->s2 != nullptr)
        return false;

    // Collect header phis.
    PointerIV iv_p{};
    Phi* phi_p = nullptr;
    Phi* phi_s = nullptr;
    int n_ptr = 0, n_acc = 0;
    for (Phi* ph = L.header->phi; ph; ph = ph->link) {
        PointerIV cand{};
        if (pick_pointer_iv(fn, L, ph, &cand)) {
            iv_p = cand;
            phi_p = ph;
            n_ptr++;
            continue;
        }
        if (ph->narg == 2 && is_int_cls(static_cast<int>(ph->cls))) {
            phi_s = ph;
            n_acc++;
            continue;
        }
        return false;  // Reject any third type of phi
    }
    if (n_ptr != 1 || n_acc != 1)
        return false;

    // accumulator phi incoming edges
    Ref s_init = Ref::none();
    for (uint k = 0; k < phi_s->narg; k++) {
        if (phi_s->blk[k] == L.preheader)
            s_init = phi_s->arg[k];
        else if (phi_s->blk[k] != L.latch)
            return false;
    }
    if (rtype(s_init) == -1)
        return false;

    Ref end_ref;
    Blk* after = nullptr;
    if (!check_exit_cmp(fn, L, iv_p.phi_to, &end_ref, &after))
        return false;

    if (!scan_body(fn, L, iv_p, phi_s, m))
        return false;

    // accumulator cls == op cls, must match load cls (same bitwidth)
    if (phi_s->cls != m->op_ins->cls)
        return false;

    if (has_outside_use(fn, L)) {
        // Check outside loop uses strictly.
        std::unordered_set<uint> loop_def;
        for (Blk* bb : L.body) {
            for (Phi* ph = bb->phi; ph; ph = ph->link)
                if (rtype(ph->to) == RTmp)
                    loop_def.insert(ph->to.val);
            for (uint k = 0; k < bb->nins; k++)
                if (rtype(bb->ins[k].to) == RTmp)
                    loop_def.insert(bb->ins[k].to.val);
        }
        loop_def.erase(phi_p->to.val);
        loop_def.erase(phi_s->to.val);
        for (Blk* b = fn->start; b; b = b->link) {
            if (L.body_bid.count(b->id))
                continue;
            for (Phi* ph = b->phi; ph; ph = ph->link)
                for (uint k = 0; k < ph->narg; k++)
                    if (rtype(ph->arg[k]) == RTmp && loop_def.count(ph->arg[k].val))
                        return false;
            for (uint k = 0; k < b->nins; k++) {
                Ins* i = &b->ins[k];
                for (int a = 0; a < 2; a++)
                    if (rtype(i->arg[a]) == RTmp && loop_def.count(i->arg[a].val))
                        return false;
            }
            if (rtype(b->jmp.arg) == RTmp && loop_def.count(b->jmp.arg.val))
                return false;
            if (rtype(b->jmp.arg2) == RTmp && loop_def.count(b->jmp.arg2.val))
                return false;
        }
    }

    m->iv_p = iv_p;
    m->phi_p = phi_p;
    m->phi_s = phi_s;
    m->s_init = s_init;
    m->end = end_ref;
    m->after = after;
    m->op = m->op_ins->op;
    m->s_cls = m->op_ins->cls;
    m->load_op = m->load_ins->op;
    m->load_cls = static_cast<int>(m->load_ins->cls);
    return true;
}

Phi* make_phi(Ctx* ctx, int cls, Ref to, Ref arg_pre, Blk* pre, Ref arg_back, Blk* back) {
    Phi* p = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
    p->cls = static_cast<short>(cls);
    p->visit = 0;
    p->to = to;
    p->narg = 2;
    p->arg = static_cast<Ref*>(vnew(ctx, 2, sizeof(Ref), PFn));
    p->blk = static_cast<Blk**>(vnew(ctx, 2, sizeof(Blk*), PFn));
    p->arg[0] = arg_pre;
    p->blk[0] = pre;
    p->arg[1] = arg_back;
    p->blk[1] = back;
    p->link = nullptr;
    return p;
}

static int rd_seq = 0;

void execute(Ctx* ctx, Fn* fn, const Loop& L, const Match& m) {
    int K = kUnrollK;
    int S = static_cast<int>(m.iv_p.step);
    int KS = K * S;
    int my_seq = rd_seq++;

    Blk* pre = L.preheader;
    Blk* tail_hdr = L.header;
    int s_cls = m.s_cls;
    int load_op = m.load_op;
    int load_cls = m.load_cls;
    int op = m.op;

    // Append size / size_main / end_main to the end of preheader
    Ref size_ref = newtmp(ctx, "rd.size", Kl, fn);
    Ref size_main_ref = newtmp(ctx, "rd.szm", Kl, fn);
    Ref end_main_ref = newtmp(ctx, "rd.endm", Kl, fn);

    Ins sub_i{};
    sub_i.op = Osub;
    sub_i.cls = Kl;
    sub_i.to = size_ref;
    sub_i.arg[0] = m.end;
    sub_i.arg[1] = m.iv_p.base;
    addins(ctx, &pre->ins, &pre->nins, &sub_i);

    Ins and_i{};
    and_i.op = Oand;
    and_i.cls = Kl;
    and_i.to = size_main_ref;
    and_i.arg[0] = size_ref;
    and_i.arg[1] = getcon(ctx, -static_cast<int64_t>(KS), fn);
    addins(ctx, &pre->ins, &pre->nins, &and_i);

    Ins add_i{};
    add_i.op = Oadd;
    add_i.cls = Kl;
    add_i.to = end_main_ref;
    add_i.arg[0] = m.iv_p.base;
    add_i.arg[1] = size_main_ref;
    addins(ctx, &pre->ins, &pre->nins, &add_i);

    // Create main_header / main_body / main_join
    Blk* mh = newblk(ctx);
    mh->name = strf(ctx, PFn, "rd%d.mh", my_seq);
    mh->id = fn->nblk++;

    Blk* mb = newblk(ctx);
    mb->name = strf(ctx, PFn, "rd%d.mb", my_seq);
    mb->id = fn->nblk++;

    Blk* mj = newblk(ctx);
    mj->name = strf(ctx, PFn, "rd%d.mj", my_seq);
    mj->id = fn->nblk++;

    // Pre-generate names (main_header phi.to, etc.)
    Ref pm_ref = newtmp(ctx, "rd.pm", Kl, fn);
    Ref pm_next_ref = newtmp(ctx, "rd.pm.n", Kl, fn);
    std::vector<Ref> s_refs(static_cast<size_t>(K)), s_next_refs(static_cast<size_t>(K));
    for (size_t i = 0; i < static_cast<size_t>(K); i++) {
        s_refs[i] = newtmp(ctx, "rd.s", s_cls, fn);
        s_next_refs[i] = newtmp(ctx, "rd.s.n", s_cls, fn);
    }

    // Identity ref
    Ref ident_ref = getcon(ctx, identity_for(op), fn);

    // main_header phi chain: pm + s0..K-1
    Phi* phi_pm = make_phi(ctx, Kl, pm_ref, m.iv_p.base, pre, pm_next_ref, mb);
    mh->phi = phi_pm;
    Phi* tail_phi = phi_pm;
    for (size_t i = 0; i < static_cast<size_t>(K); i++) {
        Ref init = (i == 0) ? m.s_init : ident_ref;
        Phi* ph = make_phi(ctx, s_cls, s_refs[i], init, pre, s_next_refs[i], mb);
        tail_phi->link = ph;
        tail_phi = ph;
    }

    // main_header cmp + jnz
    Ref cmpm_ref = newtmp(ctx, "rd.cmpm", Kw, fn);
    Ins cmpm_i{};
    cmpm_i.op = Ocultl;
    cmpm_i.cls = Kw;
    cmpm_i.to = cmpm_ref;
    cmpm_i.arg[0] = pm_ref;
    cmpm_i.arg[1] = end_main_ref;
    addins(ctx, &mh->ins, &mh->nins, &cmpm_i);
    mh->jmp.type = Jjnz;
    mh->jmp.arg = cmpm_ref;
    mh->jmp.arg2 = Ref::none();
    mh->s1 = mb;
    mh->s2 = mj;

    // main_body: K loads + K ops + 1 pointer step
    std::vector<Ref> v_refs(static_cast<size_t>(K));
    for (size_t i = 0; i < static_cast<size_t>(K); i++) {
        Ref addr;
        if (i == 0) {
            addr = pm_ref;
        } else {
            addr = newtmp(ctx, "rd.adr", Kl, fn);
            Ins ai{};
            ai.op = Oadd;
            ai.cls = Kl;
            ai.to = addr;
            ai.arg[0] = pm_ref;
            ai.arg[1] = getcon(ctx, static_cast<int64_t>(i) * S, fn);
            addins(ctx, &mb->ins, &mb->nins, &ai);
        }
        v_refs[i] = newtmp(ctx, "rd.v", load_cls, fn);
        Ins li{};
        li.op = static_cast<uint>(load_op);
        li.cls = static_cast<uint>(load_cls) & 3u;
        li.to = v_refs[i];
        li.arg[0] = addr;
        li.arg[1] = Ref::none();
        addins(ctx, &mb->ins, &mb->nins, &li);
    }
    for (size_t i = 0; i < static_cast<size_t>(K); i++) {
        Ins oi{};
        oi.op = static_cast<uint>(op);
        oi.cls = static_cast<uint>(s_cls) & 3u;
        oi.to = s_next_refs[i];
        oi.arg[0] = s_refs[i];
        oi.arg[1] = v_refs[i];
        addins(ctx, &mb->ins, &mb->nins, &oi);
    }
    Ins pn_i{};
    pn_i.op = Oadd;
    pn_i.cls = Kl;
    pn_i.to = pm_next_ref;
    pn_i.arg[0] = pm_ref;
    pn_i.arg[1] = getcon(ctx, static_cast<int64_t>(KS), fn);
    addins(ctx, &mb->ins, &mb->nins, &pn_i);
    mb->jmp.type = Jjmp;
    mb->jmp.arg = Ref::none();
    mb->jmp.arg2 = Ref::none();
    mb->s1 = mh;
    mb->s2 = nullptr;

    // main_join: log2(K) tree reduction
    Ref s01_ref = newtmp(ctx, "rd.s01", s_cls, fn);
    Ref s23_ref = newtmp(ctx, "rd.s23", s_cls, fn);
    Ref s_main_ref = newtmp(ctx, "rd.sm", s_cls, fn);

    Ins j1{};
    j1.op = static_cast<uint>(op);
    j1.cls = static_cast<uint>(s_cls) & 3u;
    j1.to = s01_ref;
    j1.arg[0] = s_refs[0];
    j1.arg[1] = s_refs[1];
    addins(ctx, &mj->ins, &mj->nins, &j1);

    Ins j2{};
    j2.op = static_cast<uint>(op);
    j2.cls = static_cast<uint>(s_cls) & 3u;
    j2.to = s23_ref;
    j2.arg[0] = s_refs[2];
    j2.arg[1] = s_refs[3];
    addins(ctx, &mj->ins, &mj->nins, &j2);

    Ins j3{};
    j3.op = static_cast<uint>(op);
    j3.cls = static_cast<uint>(s_cls) & 3u;
    j3.to = s_main_ref;
    j3.arg[0] = s01_ref;
    j3.arg[1] = s23_ref;
    addins(ctx, &mj->ins, &mj->nins, &j3);

    mj->jmp.type = Jjmp;
    mj->jmp.arg = Ref::none();
    mj->jmp.arg2 = Ref::none();
    mj->s1 = tail_hdr;
    mj->s2 = nullptr;

    // Change preheader.s1 -> main_header
    pre->s1 = mh;

    // Link new blocks.
    mj->link = pre->link;
    mb->link = mj;
    mh->link = mb;
    pre->link = mh;

    // Update tail_header phis.
    for (Phi* ph = tail_hdr->phi; ph; ph = ph->link) {
        for (uint k = 0; k < ph->narg; k++) {
            if (ph->blk[k] != pre)
                continue;
            ph->blk[k] = mj;
            if (req(ph->to, m.phi_p->to))
                ph->arg[k] = pm_ref;
            else if (req(ph->to, m.phi_s->to))
                ph->arg[k] = s_main_ref;
            break;
        }
    }
}

}  // namespace

void reduction_unroll(Ctx* ctx, Fn* fn) {
    std::vector<Loop> loops;
    find_loops(fn, loops);
    if (loops.empty())
        return;

    // Inner loops first (smaller body first)
    std::sort(loops.begin(), loops.end(),
              [](const Loop& a, const Loop& b) { return a.body.size() < b.body.size(); });

    std::unordered_set<uint> consumed;
    for (const Loop& L : loops) {
        bool clash = false;
        for (Blk* bb : L.body)
            if (consumed.count(bb->id)) {
                clash = true;
                break;
            }
        if (clash)
            continue;
        Match m{};
        if (!try_match_loop(fn, L, &m))
            continue;
        execute(ctx, fn, L, m);
        for (Blk* bb : L.body)
            consumed.insert(bb->id);
    }
}

}  // namespace rcc
