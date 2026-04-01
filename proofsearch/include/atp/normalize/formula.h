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

/// @file formula.h
/// @brief First-order logic formula AST (pre-clausification).

#include "atp/core/types.h"

#include <memory>
#include <string>
#include <vector>

namespace atp {

/// AST node types for first-order logic formulas.
enum class FormulaKind : uint8_t {
    kAtom,       ///< Predicate application: P(t1, ..., tn)
    kNot,        ///< Negation: ¬F
    kAnd,        ///< Conjunction: F ∧ G
    kOr,         ///< Disjunction: F ∨ G
    kImplies,    ///< Implication: F → G
    kIff,        ///< Biconditional: F ↔ G
    kForall,     ///< Universal: ∀x. F
    kExists,     ///< Existential: ∃x. F
};

/// A first-order logic formula node.
struct Formula {
    FormulaKind kind;

    // For kAtom: the predicate term
    TermId atom = kInvalidId;

    // For quantifiers: the bound variable name
    std::string var_name;

    // Sub-formulas (1 for Not/Quantifiers, 2 for binary connectives)
    std::vector<std::unique_ptr<Formula>> children;

    static std::unique_ptr<Formula> makeAtom(TermId atom);
    static std::unique_ptr<Formula> makeNot(std::unique_ptr<Formula> child);
    static std::unique_ptr<Formula> makeAnd(std::unique_ptr<Formula> lhs,
                                            std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeOr(std::unique_ptr<Formula> lhs,
                                           std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeImplies(std::unique_ptr<Formula> lhs,
                                                std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeIff(std::unique_ptr<Formula> lhs,
                                            std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeForall(std::string var,
                                               std::unique_ptr<Formula> body);
    static std::unique_ptr<Formula> makeExists(std::string var,
                                               std::unique_ptr<Formula> body);
};

}  // namespace atp
