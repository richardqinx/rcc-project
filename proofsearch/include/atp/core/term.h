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

#include <span>

#include "atp/core/types.h"

namespace atp {

/// A term is a function/predicate application: f(t1, t2, ..., tn).
/// Stored in a flat arena; args are contiguous TermIds following the header.
/// Variables are represented as terms with a special symbol range.
struct Term {
    SymbolId symbol_id;  ///< Index into the SymbolTable
    uint16_t arity;      ///< Number of arguments

    /// Access the argument TermIds (stored immediately after this struct in the arena).
    [[nodiscard]] std::span<const TermId> args() const;
    [[nodiscard]] std::span<TermId> mutableArgs();
};

}  // namespace atp
