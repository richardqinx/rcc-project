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

/// @file loopswap.cc
/// @brief Integer perfect 2-level nested loop interchange.
///
/// See docs/loopswap.md for architectural notes.

#include "rcc/all.h"
#include "rcc/loopinfo.h"

#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

// Helper: reaches
bool reaches(Fn* fn, Ref r, Ref target, std::unordered_set<uint>& visiting, int budget) {
    if (req(r, target))
        return true;
    if (budget <= 0)
        return false;
    if (rtype(r) != RTmp || r.val >= static_cast<uint>(fn->ntmp))
        return false;
    if (!visiting.insert(r.val).second)
        return true;

    Ins* def = fn->tmp[r.val].def;
    if (def && def->op == Ocopy) {
        bool ok = reaches(fn, def->arg[0], target, visiting, budget - 1);
        visiting.erase(r.val);
        return ok;
    }
    for (Blk* bb = fn->start; bb; bb = bb->link) {
        for (Phi* p = bb->phi; p; p = p->link) {
            if (!req(p->to, r))
                continue;
            bool ok = true;
            for (uint k = 0; k < p->narg; k++) {
                if (req(p->arg[k], r))
                    continue;
                if (!reaches(fn, p->arg[k], target, visiting, budget - 1)) {
                    ok = false;
                    break;
                }
            }
            visiting.erase(r.val);
            return ok;
        }
    }
    visiting.erase(r.val);
    return false;
}

bool carries(Fn* fn, Ref r, Ref target) {
    std::unordered_set<uint> v;
    return reaches(fn, r, target, v, 64);
}

// Affine decomposition
struct Aff {
    int64_t outer_c;
    int64_t inner_c;
};

Aff affine(Fn* fn, Ref r, Ref outer_to, Ref inner_to, int depth) {
    if (depth > 24)
        return {0, 0};
    if (rtype(r) == RCon)
        return {0, 0};
    if (rtype(r) != RTmp)
        return {0, 0};
    if (carries(fn, r, outer_to))
        return {1, 0};
    if (carries(fn, r, inner_to))
        return {0, 1};
    if (r.val >= static_cast<uint>(fn->ntmp))
        return {0, 0};
    Ins* def = fn->tmp[r.val].def;
    if (!def)
        return {0, 0};
    switch (def->op) {
        case Oadd: {
            Aff a = affine(fn, def->arg[0], outer_to, inner_to, depth + 1);
            Aff b = affine(fn, def->arg[1], outer_to, inner_to, depth + 1);
            return {a.outer_c + b.outer_c, a.inner_c + b.inner_c};
        }
        case Osub: {
            Aff a = affine(fn, def->arg[0], outer_to, inner_to, depth + 1);
            Aff b = affine(fn, def->arg[1], outer_to, inner_to, depth + 1);
            return {a.outer_c - b.outer_c, a.inner_c - b.inner_c};
        }
        case Omul: {
            int64_t c;
            if (ref_is_const(fn, def->arg[1], c)) {
                Aff a = affine(fn, def->arg[0], outer_to, inner_to, depth + 1);
                return {a.outer_c * c, a.inner_c * c};
            }
            if (ref_is_const(fn, def->arg[0], c)) {
                Aff a = affine(fn, def->arg[1], outer_to, inner_to, depth + 1);
                return {a.outer_c * c, a.inner_c * c};
            }
            return {0, 0};
        }
        case Oextsw:
        case Ocopy:
            return affine(fn, def->arg[0], outer_to, inner_to, depth + 1);
        default:
            return {0, 0};
    }
}

// Perfect nest: L1.body \ L2.body \ {L1.header, L1.latch} must only contain empty Jjmp blocks.
bool perfect_nest(const Loop& L1, const Loop& L2) {
    for (Blk* bb : L1.body) {
        if (bb == L1.header || bb == L1.latch)
            continue;
        if (L2.body_bid.count(bb->id))
            continue;
        if (bb->nins != 0)
            return false;
        if (bb->jmp.type != Jjmp)
            return false;
    }
    return true;
}

// Does the cmp ins for header.jmp.arg reference bv.to (or its carrier equiv)?
int find_header_cmp(Fn* fn, const Loop& L, const Biv& bv) {
    Ref a = L.header->jmp.arg;
    if (rtype(a) != RTmp)
        return -1;
    for (uint k = 0; k < L.header->nins; k++) {
        Ins* ii = &L.header->ins[k];
        if (!req(ii->to, a))
            continue;
        if (carries(fn, ii->arg[0], bv.to) || carries(fn, ii->arg[1], bv.to))
            return static_cast<int>(k);
        return -1;
    }
    return -1;
}

// Find index of BIV's latch increment ins (the Oadd) in latch->ins array.
int find_latch_inc(const Loop& L, const Biv& bv) {
    Phi* P = nullptr;
    for (Phi* p = L.header->phi; p; p = p->link)
        if (req(p->to, bv.to)) {
            P = p;
            break;
        }
    if (!P)
        return -1;
    Ref next = Ref::none();
    for (uint k = 0; k < P->narg; k++)
        if (P->blk[k] == L.latch) {
            next = P->arg[k];
            break;
        }
    if (rtype(next) != RTmp)
        return -1;
    for (uint k = 0; k < L.latch->nins; k++)
        if (req(L.latch->ins[k].to, next) && L.latch->ins[k].op == Oadd)
            return static_cast<int>(k);
    return -1;
}

Phi* find_phi(Blk* b, Ref to) {
    for (Phi* p = b->phi; p; p = p->link)
        if (req(p->to, to))
            return p;
    return nullptr;
}

void unlink_phi(Blk* b, Phi* t) {
    for (Phi** cur = &b->phi; *cur; cur = &(*cur)->link)
        if (*cur == t) {
            *cur = t->link;
            t->link = nullptr;
            return;
        }
}

void prepend_phi(Blk* b, Phi* p) {
    p->link = b->phi;
    b->phi = p;
}

void rewrite_phi_blk(Phi* p, Blk* from, Blk* to) {
    for (uint k = 0; k < p->narg; k++)
        if (p->blk[k] == from)
            p->blk[k] = to;
}

void rewrite_uses(Fn* fn, Ref from, Ref to) {
    for (Blk* b = fn->start; b; b = b->link) {
        for (Phi* p = b->phi; p; p = p->link)
            for (uint k = 0; k < p->narg; k++)
                if (req(p->arg[k], from))
                    p->arg[k] = to;
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            if (req(i->arg[0], from))
                i->arg[0] = to;
            if (req(i->arg[1], from))
                i->arg[1] = to;
        }
        if (req(b->jmp.arg, from))
            b->jmp.arg = to;
        if (req(b->jmp.arg2, from))
            b->jmp.arg2 = to;
    }
}

struct AccessRec {
    Ref addr;
    int64_t inner_s;
    bool is_store;
};

bool try_swap(Ctx* ctx, Fn* fn, const Loop& L1, const Loop& L2) {
    if (!perfect_nest(L1, L2))
        return false;

    std::vector<Biv> bs1, bs2;
    detect_bivs(fn, L1, bs1);
    detect_bivs(fn, L2, bs2);
    if (bs1.empty() || bs2.empty())
        return false;

    // Pick the BIV referenced by header.cmp (SysY loops typically compare one BIV).
    Biv b1{}, b2{};
    int cmp1 = -1, cmp2 = -1;
    for (auto& bv : bs1) {
        int idx = find_header_cmp(fn, L1, bv);
        if (idx >= 0) {
            b1 = bv;
            cmp1 = idx;
            break;
        }
    }
    for (auto& bv : bs2) {
        int idx = find_header_cmp(fn, L2, bv);
        if (idx >= 0) {
            b2 = bv;
            cmp2 = idx;
            break;
        }
    }
    if (cmp1 < 0 || cmp2 < 0)
        return false;
    if ((b1.cls != Kw && b1.cls != Kl) || (b2.cls != Kw && b2.cls != Kl))
        return false;

    int inc1 = find_latch_inc(L1, b1);
    int inc2 = find_latch_inc(L2, b2);
    if (inc1 < 0 || inc2 < 0)
        return false;

    // Scan L2.body: integer only + decompose addr.
    int64_t sum_outer = 0, sum_inner = 0;
    std::vector<AccessRec> mems;
    for (Blk* bb : L2.body) {
        for (uint k = 0; k < bb->nins; k++) {
            Ins* ii = &bb->ins[k];
            int op = ii->op;
            if (op == Ocall)
                return false;
            if (ii->cls == Ks || ii->cls == Kd)
                return false;
            bool is_ld = isload(op);
            bool is_st = isstore(op);
            if (!is_ld && !is_st)
                continue;
            if (op == Ostores || op == Ostored)
                return false;
            Ref addr = is_st ? ii->arg[1] : ii->arg[0];
            Aff a = affine(fn, addr, b1.to, b2.to, 0);
            sum_outer += std::abs(a.outer_c);
            sum_inner += std::abs(a.inner_c);
            mems.push_back({addr, a.inner_c, is_st});
        }
    }

    // accumulator pattern: if there are stores => every store has inner_s==0 and matches a load
    // with same addr and inner_s==0.
    bool has_store = false;
    for (auto& m : mems)
        if (m.is_store) {
            has_store = true;
            break;
        }
    if (has_store) {
        for (auto& s : mems) {
            if (!s.is_store)
                continue;
            if (s.inner_s != 0)
                return false;
            bool found = false;
            for (auto& l : mems) {
                if (!l.is_store && req(l.addr, s.addr) && l.inner_s == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }

    if (sum_inner == 0)
        return false;
    if (sum_outer * 4 >= sum_inner)
        return false;

    // Execute
    Blk* h1 = L1.header;
    Blk* h2 = L2.header;

    Phi* P1 = find_phi(h1, b1.to);
    Phi* P2 = find_phi(h2, b2.to);
    if (!P1 || !P2)
        return false;

    // Collect self-loop carrier of b1.to on L2.header (Braun artifact; invalid after swap, needs
    // removal).
    std::vector<Phi*> stale;
    for (Phi* p = h2->phi; p; p = p->link) {
        if (p->narg != 2)
            continue;
        Ref pre = Ref::none();
        bool is_self = false;
        for (uint k = 0; k < p->narg; k++) {
            if (p->blk[k] == L2.preheader)
                pre = p->arg[k];
            else if (p->blk[k] == L2.latch && req(p->arg[k], p->to))
                is_self = true;
        }
        if (is_self && carries(fn, pre, b1.to))
            stale.push_back(p);
    }

    // Snapshot: copy 4 target ins before swap to avoid distortion.
    Ins C1 = h1->ins[cmp1];
    Ins C2 = h2->ins[cmp2];
    Ins I1 = L1.latch->ins[inc1];
    Ins I2 = L2.latch->ins[inc2];

    // Swap phi: unlink + rewrite edge + prepend to the other.
    unlink_phi(h1, P1);
    unlink_phi(h2, P2);
    rewrite_phi_blk(P1, L1.preheader, L2.preheader);
    rewrite_phi_blk(P1, L1.latch, L2.latch);
    rewrite_phi_blk(P2, L2.preheader, L1.preheader);
    rewrite_phi_blk(P2, L2.latch, L1.latch);
    prepend_phi(h1, P2);
    prepend_phi(h2, P1);

    // Swap cmp ins + jmp descriptor.
    h1->ins[cmp1].op = Onop;
    h2->ins[cmp2].op = Onop;
    addins(ctx, &h1->ins, &h1->nins, &C2);
    addins(ctx, &h2->ins, &h2->nins, &C1);
    std::swap(h1->jmp.arg, h2->jmp.arg);
    std::swap(h1->jmp.cls, h2->jmp.cls);
    std::swap(h1->jmp.arg2, h2->jmp.arg2);

    // Swap increment.
    L1.latch->ins[inc1].op = Onop;
    L2.latch->ins[inc2].op = Onop;
    addins(ctx, &L1.latch->ins, &L1.latch->nins, &I2);
    addins(ctx, &L2.latch->ins, &L2.latch->nins, &I1);

    // Invalidate carriers: rewrite_uses to substitute with b1.to, then unlink phi.
    for (Phi* s : stale) {
        rewrite_uses(fn, s->to, b1.to);
        unlink_phi(h2, s);
    }

    return true;
}

// L2 is immediate-child of L1: strict subset of body, with no L3 strictly in between.
bool is_immediate_child(const std::vector<Loop>& loops, size_t i_par, size_t j_chl) {
    const Loop& par = loops[i_par];
    const Loop& chl = loops[j_chl];
    if (chl.body_bid.size() >= par.body_bid.size())
        return false;
    for (uint id : chl.body_bid)
        if (!par.body_bid.count(id))
            return false;
    for (size_t m = 0; m < loops.size(); m++) {
        if (m == i_par || m == j_chl)
            continue;
        const Loop& mid = loops[m];
        if (mid.body_bid.size() <= chl.body_bid.size())
            continue;
        if (mid.body_bid.size() >= par.body_bid.size())
            continue;
        bool chl_in_mid = true;
        for (uint id : chl.body_bid)
            if (!mid.body_bid.count(id)) {
                chl_in_mid = false;
                break;
            }
        if (!chl_in_mid)
            continue;
        bool mid_in_par = true;
        for (uint id : mid.body_bid)
            if (!par.body_bid.count(id)) {
                mid_in_par = false;
                break;
            }
        if (mid_in_par)
            return false;
    }
    return true;
}

}  // namespace

void loopinterchange(Ctx* ctx, Fn* fn) {
    std::vector<Loop> loops;
    find_loops(fn, loops);
    if (loops.size() < 2)
        return;

    for (size_t i = 0; i < loops.size(); i++) {
        Loop& L1 = loops[i];
        Loop* L2 = nullptr;
        bool amb = false;
        for (size_t j = 0; j < loops.size(); j++) {
            if (i == j)
                continue;
            if (!is_immediate_child(loops, i, j))
                continue;
            if (L2) {
                amb = true;
                break;
            }
            L2 = &loops[j];
        }
        if (!L2 || amb)
            continue;
        if (try_swap(ctx, fn, L1, *L2))
            return;  // One swap per pass, CFG/loops are invalidated
    }
}

}  // namespace rcc
