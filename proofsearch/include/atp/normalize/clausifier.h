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
/// Clausifier is a stateful object because:
///   1. Skolem function counter must be shared across all formulas in a problem
///      (otherwise different formulas generate conflicting Skolem names).
///   2. Variable renaming counter must be shared for the same reason.
///
/// Pipeline: eliminate ↔/→  →  NNF  →  Skolemize  →  distribute ∨ over ∧  →  flatten.

#include "atp/core/clause.h"
#include "atp/core/symbol_table.h"
#include "atp/core/term_bank.h"
#include "atp/normalize/formula.h"

#include <vector>

namespace atp {

/// Stateful clausifier. Create one per proof problem.
///
/// Usage:
///   Clausifier clausifier(bank, symbols);
///   auto clauses1 = clausifier.clausify(formula1);
///   auto clauses2 = clausifier.clausify(formula2);
///   // Skolem IDs are unique across both calls.
class Clausifier {
  public:
    Clausifier(TermBank& bank, SymbolTable& symbols);

    /// Convert a single formula to CNF clauses.
    /// Skolem functions generated here are globally unique (counter persists).
    [[nodiscard]] std::vector<Clause> clausify(const Formula& formula);

    /// Negate a formula: wraps it in ¬(...)
    /// Used to negate the conjecture before clausification.
    [[nodiscard]] static std::unique_ptr<Formula> negate(std::unique_ptr<Formula> formula);

    /// Number of Skolem functions generated so far.
    [[nodiscard]] uint32_t skolemCount() const { return next_skolem_id_; }

  private:
    TermBank& bank_;
    SymbolTable& symbols_;
    uint32_t next_skolem_id_ = 0;
    uint32_t next_var_id_ = 0;  ///< For variable standardization apart
};

}  // namespace atp
