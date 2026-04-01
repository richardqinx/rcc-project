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

/// @file clause_queue.h
/// @brief Priority queue for the Given Clause Loop.

#include "atp/core/clause.h"

#include <functional>

namespace atp {

/// Comparison function type for clause ordering.
using ClauseComparator = std::function<bool(const Clause& a, const Clause& b)>;

/// BFS comparator: prioritize shallower derivation depth.
inline bool bfsCompare(const Clause& a, const Clause& b) {
    return a.depth > b.depth;  // min-heap by depth
}

/// Weight comparator: prioritize shorter (fewer literals) clauses.
inline bool weightCompare(const Clause& a, const Clause& b) {
    return a.literals.size() > b.literals.size();  // min-heap by literal count
}

}  // namespace atp
