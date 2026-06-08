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

/// @file loopinfo.h
/// @brief Natural loop and constant BIV identification.

#pragma once

#include "rcc/all.h"

#include <unordered_set>
#include <vector>

namespace rcc {

struct Loop {
    Blk* header;
    Blk* preheader;
    Blk* latch;
    std::vector<Blk*> body;             // Includes header / latch
    std::unordered_set<uint> body_bid;  // Set of b->id in body, used for tmp.bid lookup
};

/// Constant Basic Induction Variable.
struct Biv {
    Ref to;        // header phi.to
    int cls;       // phi cls
    int64_t init;  // Preheader constant
    int64_t step;  // Constant step of Oadd at latch, != 0
};

void find_loops(Fn*, std::vector<Loop>&);
void detect_bivs(Fn*, const Loop&, std::vector<Biv>&);

/// Decode CBits constant Ref (integer). Returns false for CAddr / float / non-RCon.
bool ref_is_const(Fn*, Ref, int64_t& out);

/// Get defining Ins via Tmp::def. Returns nullptr for non-RTmp or out of bounds.
Ins* def_ins(Fn*, Ref);

/// Canonical pointer IV after LSR.
struct PointerIV {
    Ref phi_to;
    Ref base;
    Ref next;
    int64_t step;
};

/// Identify pointer IV on header phi. Returns false on failure.
bool pick_pointer_iv(Fn*, const Loop&, Phi*, PointerIV*);

/// Match pointer termination condition against pointer IV.
bool check_exit_cmp(Fn*, const Loop&, Ref p, Ref* end_out, Blk** after_out);

/// Check if any def inside the loop is used by outside users.
bool has_outside_use(Fn*, const Loop&);

}  // namespace rcc
