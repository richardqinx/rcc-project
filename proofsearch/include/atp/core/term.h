/*
 * richard-atp-engine: A saturation-based automated theorem prover for first-order logic.
 * Copyright (C) 2026 Richard Qin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

/// @file term.h
/// @brief Core Term data structure — hash-consed, immutable.
///
/// V1 uses vector<TermId> for args.
/// Future: flat arena with contiguous arg storage for cache locality.

#include <vector>

#include "atp/core/types.h"

namespace atp {

/// A term is a function/predicate application: f(t1, t2, ..., tn).
/// Variables are 0-arity terms whose symbol has SymbolKind::kVariable.
struct Term {
    SymbolId symbol_id;          ///< Index into the SymbolTable
    std::vector<TermId> args;    ///< Argument sub-term IDs

    /// Number of arguments.
    [[nodiscard]] uint16_t arity() const {
        return static_cast<uint16_t>(args.size());
    }
};

}  // namespace atp
