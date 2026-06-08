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

/// @file dce.cc
/// @brief Dead Code Elimination: mark-and-sweep starting from side-effect / control flow roots.
///
/// See docs/dce.md for architectural notes.

#include "rcc/all.h"

#include <vector>

namespace rcc {

namespace {

bool is_root(uint op) {
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

}  // namespace

void dce(Ctx*, Fn* fn) {
    int ntmp = fn->ntmp;
    std::vector<Phi*> phi_def(static_cast<size_t>(ntmp), nullptr);
    std::vector<Ins*> ins_def(static_cast<size_t>(ntmp), nullptr);

    for (Blk* b = fn->start; b; b = b->link) {
        for (Phi* p = b->phi; p; p = p->link)
            if (rtype(p->to) == RTmp && p->to.val < static_cast<uint>(ntmp))
                phi_def[p->to.val] = p;
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            if (rtype(i->to) == RTmp && i->to.val < static_cast<uint>(ntmp))
                ins_def[i->to.val] = i;
        }
    }

    std::vector<char> alive(static_cast<size_t>(ntmp), 0);
    std::vector<uint> wl;
    auto mark = [&](Ref r) {
        if (rtype(r) == RTmp && r.val < static_cast<uint>(ntmp) && !alive[r.val]) {
            alive[r.val] = 1;
            wl.push_back(r.val);
        }
    };

    for (Blk* b = fn->start; b; b = b->link) {
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            if (is_root(i->op)) {
                mark(i->arg[0]);
                mark(i->arg[1]);
            }
        }
        mark(b->jmp.arg);
    }

    while (!wl.empty()) {
        uint t = wl.back();
        wl.pop_back();
        if (Ins* d = ins_def[t]) {
            mark(d->arg[0]);
            mark(d->arg[1]);
            continue;
        }
        if (Phi* p = phi_def[t]) {
            for (uint a = 0; a < p->narg; a++)
                mark(p->arg[a]);
        }
    }

    int killed_ins = 0, killed_phi = 0;
    for (Blk* b = fn->start; b; b = b->link) {
        Phi** pp = &b->phi;
        while (*pp) {
            Phi* p = *pp;
            bool dead =
                (rtype(p->to) == RTmp && p->to.val < static_cast<uint>(ntmp) && !alive[p->to.val]);
            if (dead) {
                *pp = p->link;
                killed_phi++;
            } else {
                pp = &p->link;
            }
        }

        uint w = 0;
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            if (i->op == Onop) {
                killed_ins++;
                continue;
            }
            bool root = is_root(i->op);
            bool has_to = (rtype(i->to) == RTmp && i->to.val < static_cast<uint>(ntmp));
            if (!root && has_to && !alive[i->to.val]) {
                killed_ins++;
                continue;
            }
            if (w != k)
                b->ins[w] = *i;
            w++;
        }
        b->nins = w;
    }

    if (debug['D']) {
        fprintf(stderr, "\n> DCE for %s: killed %d ins, %d phi\n", fn->name, killed_ins,
                killed_phi);
    }
}

}  // namespace rcc
