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

/// @file peephole.cc
/// @brief Backend sliding-window peephole pass.

#include "rcc/all.h"

namespace rcc {

namespace {

// Store op to byte width.
int store_size(int op) {
    switch (op) {
        case Ostoreb:
            return 1;
        case Ostoreh:
            return 2;
        case Ostorew:
        case Ostores:
            return 4;
        case Ostorel:
        case Ostored:
            return 8;
        default:
            return 0;
    }
}

// Check if store and load are compatible for forwarding.
bool store_load_compatible(int sop, int lcls) {
    switch (sop) {
        case Ostorew:
            return lcls == Kw;
        case Ostorel:
            return lcls == Kl;
        case Ostores:
            return lcls == Ks;
        case Ostored:
            return lcls == Kd;
        default:
            return false;
    }
}

void peep_blk(Blk* b) {
    uint n = b->nins;
    if (n == 0)
        return;

    // Self Ocopy elimination.
    for (uint k = 0; k < n; k++) {
        Ins* i = &b->ins[k];
        if (i->op == Ocopy && req(i->to, i->arg[0]) && rtype(i->to) != -1)
            i->op = Onop;
    }

    // Window 2 mode.
    auto next_live = [&](uint from) -> int {
        for (uint k = from; k < n; k++)
            if (b->ins[k].op != Onop)
                return static_cast<int>(k);
        return -1;
    };

    int p = next_live(0);
    while (p >= 0) {
        int q = next_live(static_cast<uint>(p) + 1);
        if (q < 0)
            break;
        Ins* pi = &b->ins[p];
        Ins* qi = &b->ins[q];

        // Store + Load to same slot: keep store, change load to copy.
        if (isstore(pi->op) && qi->op == Oload && rtype(pi->arg[1]) == RSlot &&
            rtype(qi->arg[0]) == RSlot && req(pi->arg[1], qi->arg[0]) &&
            store_load_compatible(pi->op, qi->cls) && rtype(pi->arg[0]) != RCon) {
            // Skip forwarding if value is a constant.
            qi->op = Ocopy;
            qi->arg[0] = pi->arg[0];
            qi->arg[1] = Ref::none();
            // Keep cls and to.
            p = q;
            continue;
        }

        // Consecutive stores to same slot: drop first.
        if (isstore(pi->op) && pi->op == qi->op && rtype(pi->arg[1]) == RSlot &&
            rtype(qi->arg[1]) == RSlot && req(pi->arg[1], qi->arg[1]) && store_size(pi->op) > 0) {
            pi->op = Onop;
            p = q;
            continue;
        }

        p = q;
    }

    // Redundant Oextsw elimination.
    for (uint pidx = 0; pidx < n; pidx++) {
        Ins* pi = &b->ins[pidx];
        if (pi->op != Oextsw)
            continue;
        if (!isreg(pi->to) || !isreg(pi->arg[0]))
            continue;
        for (uint qidx = pidx + 1; qidx < n; qidx++) {
            Ins* qi = &b->ins[qidx];
            if (qi->op == Onop)
                continue;
            // Identical Oextsw.
            if (qi->op == pi->op && req(qi->to, pi->to) && req(qi->arg[0], pi->arg[0]) &&
                req(qi->arg[1], pi->arg[1])) {
                qi->op = Onop;
                continue;
            }
            // Anchor invalidated.
            if (isreg(qi->to) && (req(qi->to, pi->to) || req(qi->to, pi->arg[0])))
                break;
        }
    }
}

}  // namespace

void peephole(Ctx* /*ctx*/, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link)
        peep_blk(b);
}

}  // namespace rcc
