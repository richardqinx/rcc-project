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
/// @file loopinfo.cc
/// @brief find_loops + detect_bivs + ref_is_const (shared).

#include "rcc/loopinfo.h"

#include "rcc/all.h"

namespace rcc {

bool ref_is_const(Fn* fn, Ref r, int64_t& out) {
    if (rtype(r) != RCon)
        return false;
    if (r.val >= static_cast<uint>(fn->ncon))
        return false;
    Con* c = &fn->con[r.val];
    if (c->type != Con::CBits || c->flt)
        return false;
    out = c->bits.i;
    return true;
}

Ins* def_ins(Fn* fn, Ref r) {
    if (rtype(r) != RTmp || r.val >= static_cast<uint>(fn->ntmp))
        return nullptr;
    return fn->tmp[r.val].def;
}

void find_loops(Fn* fn, std::vector<Loop>& out) {
    out.clear();
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = ~0u;

    for (uint i = 0; i < fn->nblk; i++) {
        Blk* hd = fn->rpo[i];
        if (hd->npred != 2)
            continue;

        Blk* pre = nullptr;
        Blk* latch = nullptr;
        for (uint k = 0; k < hd->npred; k++) {
            Blk* p = hd->pred[k];
            if (p->id != ~0u && p->id >= hd->id)
                latch = p;
            else
                pre = p;
        }
        if (!pre || !latch)
            continue;

        Loop L;
        L.header = hd;
        L.preheader = pre;
        L.latch = latch;
        L.body.push_back(hd);
        L.body_bid.insert(hd->id);
        hd->visit = hd->id;
        std::vector<Blk*> stk = {latch};
        while (!stk.empty()) {
            Blk* n = stk.back();
            stk.pop_back();
            if (n->visit == hd->id)
                continue;
            n->visit = hd->id;
            L.body.push_back(n);
            L.body_bid.insert(n->id);
            for (uint k = 0; k < n->npred; k++)
                stk.push_back(n->pred[k]);
        }
        out.push_back(std::move(L));
    }
}

void detect_bivs(Fn* fn, const Loop& L, std::vector<Biv>& out) {
    for (Phi* p = L.header->phi; p; p = p->link) {
        if (p->narg != 2)
            continue;
        Ref init_ref = Ref::none(), next_ref = Ref::none();
        for (uint k = 0; k < p->narg; k++) {
            if (p->blk[k] == L.preheader)
                init_ref = p->arg[k];
            else if (p->blk[k] == L.latch)
                next_ref = p->arg[k];
        }
        int64_t init_v = 0;
        if (!ref_is_const(fn, init_ref, init_v))
            continue;
        Ins* nx = def_ins(fn, next_ref);
        if (!nx || nx->op != Oadd)
            continue;
        int64_t step_v = 0;
        if (req(nx->arg[0], p->to) && ref_is_const(fn, nx->arg[1], step_v)) {
        } else if (req(nx->arg[1], p->to) && ref_is_const(fn, nx->arg[0], step_v)) {
        } else {
            continue;
        }
        if (step_v == 0)
            continue;
        out.push_back({p->to, static_cast<int>(p->cls), init_v, step_v});
    }
}

bool pick_pointer_iv(Fn* fn, const Loop& L, Phi* p, PointerIV* out) {
    if (p->cls != Kl)
        return false;
    if (p->narg != 2)
        return false;
    Ref base = Ref::none(), next = Ref::none();
    for (uint k = 0; k < p->narg; k++) {
        if (p->blk[k] == L.preheader)
            base = p->arg[k];
        else if (p->blk[k] == L.latch)
            next = p->arg[k];
        else
            return false;
    }
    if (rtype(base) == -1 || rtype(next) == -1)
        return false;
    Ins* nx = def_ins(fn, next);
    if (!nx || nx->op != Oadd)
        return false;
    int64_t step = 0;
    if (req(nx->arg[0], p->to) && ref_is_const(fn, nx->arg[1], step)) {
    } else if (req(nx->arg[1], p->to) && ref_is_const(fn, nx->arg[0], step)) {
    } else {
        return false;
    }
    if (step <= 0)
        return false;
    out->phi_to = p->to;
    out->base = base;
    out->next = next;
    out->step = step;
    return true;
}

bool check_exit_cmp(Fn* fn, const Loop& L, Ref p, Ref* end_out, Blk** after_out) {
    Blk* hd = L.header;
    if (hd->jmp.type != Jjnz)
        return false;
    Ref cmp_r = hd->jmp.arg;
    if (rtype(cmp_r) != RTmp)
        return false;
    Ins* cmp_def = nullptr;
    for (uint k = 0; k < hd->nins; k++) {
        Ins* ii = &hd->ins[k];
        if (req(ii->to, cmp_r)) {
            cmp_def = ii;
            break;
        }
    }
    if (!cmp_def)
        return false;
    if (cmp_def->op != Ocultl && cmp_def->op != Ocsltl)
        return false;
    if (!req(cmp_def->arg[0], p))
        return false;
    Ref end = cmp_def->arg[1];
    if (rtype(end) == RTmp) {
        Ins* d = def_ins(fn, end);
        if (d) {
            for (Blk* bb : L.body)
                if (bb->ins <= d && d < bb->ins + bb->nins)
                    return false;
        }
    }
    Blk* s1 = hd->s1;
    Blk* s2 = hd->s2;
    bool s1_in = L.body_bid.count(s1->id) != 0;
    bool s2_in = L.body_bid.count(s2->id) != 0;
    if (s1_in == s2_in)
        return false;
    *end_out = end;
    *after_out = s1_in ? s2 : s1;
    return true;
}

bool has_outside_use(Fn* fn, const Loop& L) {
    std::unordered_set<uint> loop_def;
    for (Blk* bb : L.body) {
        for (Phi* ph = bb->phi; ph; ph = ph->link)
            if (rtype(ph->to) == RTmp)
                loop_def.insert(ph->to.val);
        for (uint k = 0; k < bb->nins; k++)
            if (rtype(bb->ins[k].to) == RTmp)
                loop_def.insert(bb->ins[k].to.val);
    }
    for (Blk* b = fn->start; b; b = b->link) {
        if (L.body_bid.count(b->id))
            continue;
        for (Phi* ph = b->phi; ph; ph = ph->link)
            for (uint k = 0; k < ph->narg; k++)
                if (rtype(ph->arg[k]) == RTmp && loop_def.count(ph->arg[k].val))
                    return true;
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            for (int a = 0; a < 2; a++)
                if (rtype(i->arg[a]) == RTmp && loop_def.count(i->arg[a].val))
                    return true;
        }
        if (rtype(b->jmp.arg) == RTmp && loop_def.count(b->jmp.arg.val))
            return true;
        if (rtype(b->jmp.arg2) == RTmp && loop_def.count(b->jmp.arg2.val))
            return true;
    }
    return false;
}

}  // namespace rcc
