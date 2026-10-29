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

/// @file paramodulation.h
/// @brief Paramodulation inference rule for equality reasoning.
///
/// Paramodulation replaces a subterm in one clause with an equal term
/// from an equation in another clause. Combined with term orderings,
/// this gives a complete calculus for first-order logic with equality
/// (the Superposition calculus).

#include "atp/core/clause.h"
#include "atp/core/term_bank.h"
#include "atp/rewrite/ordering.h"

#include <vector>

namespace atp {

/// Generate all paramodulants between two clauses.
/// Uses the term ordering to restrict to ordered paramodulation (superposition).
std::vector<Clause> paramodulate(const TermBank& bank, const Clause& c1, const Clause& c2,
                                 const TermOrdering& ordering);

/// Simplify a clause by demodulation (rewriting with unit equalities).
/// Returns true if the clause was modified.
bool demodulate(TermBank& bank, Clause& clause, const Clause& unit_eq,
                const TermOrdering& ordering);

}  // namespace atp
