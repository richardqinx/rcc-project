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

/// @file abi.cc
/// @brief elimsb - Eliminate ABI-level sub-word op variants (sb/ub/sh/uh for arg/par/ret).
///
/// See docs/abi.md for architectural notes.

#include "rcc/all.h"

namespace rcc {

void elimsb(Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        Ins* ie = b->ins + b->nins;
        for (Ins* i = b->ins; i < ie; ++i) {
            if (isargbh(i->op)) {
                i->op = Oarg;
            } else if (isparbh(i->op)) {
                i->op = Opar;
            }
        }
        if (isretbh(b->jmp.type)) {
            b->jmp.type = Jretw;
        }
    }
}

}  // namespace rcc
