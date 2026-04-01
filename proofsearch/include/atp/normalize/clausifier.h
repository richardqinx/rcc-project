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

/// @file clausifier.h
/// @brief Converts FOL formulas to Clause Normal Form (CNF).
///
/// Pipeline: eliminate ↔/→  →  NNF  →  Skolemize  →  distribute ∨ over ∧  →  flatten.

#include "atp/core/clause.h"
#include "atp/core/term_bank.h"
#include "atp/normalize/formula.h"

#include <vector>

namespace atp {

/// Convert a first-order formula into a set of clauses (CNF).
std::vector<Clause> clausify(const Formula& formula, TermBank& bank);

}  // namespace atp
