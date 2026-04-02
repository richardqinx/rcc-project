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
///
/// Design decision: atoms use TermId (interned), quantifier variables use
/// SymbolId (interned). The parser interns all symbols during parsing.
/// This avoids string→ID lookups during clausification and ensures
/// consistent variable identity across quantifier scopes.

#include "atp/core/types.h"

#include <memory>
#include <vector>

namespace atp {

/// AST node types for first-order logic formulas.
enum class FormulaKind : uint8_t {
    kAtom,       ///< Predicate application: P(t1, ..., tn)
    kEquality,   ///< Equality atom: t1 = t2 (distinguished for equality reasoning)
    kNot,        ///< Negation: ¬F
    kAnd,        ///< Conjunction: F ∧ G
    kOr,         ///< Disjunction: F ∨ G
    kImplies,    ///< Implication: F → G
    kIff,        ///< Biconditional: F ↔ G
    kForall,     ///< Universal: ∀x. F
    kExists,     ///< Existential: ∃x. F
};

/// A first-order logic formula node.
///
/// All identifiers are interned: atoms as TermIds, quantifier variables as
/// SymbolIds. No raw strings survive past the parser.
struct Formula {
    FormulaKind kind;

    /// For kAtom / kEquality: the predicate/equality term, already interned.
    TermId atom = kInvalidId;

    /// For kForall / kExists: the bound variable, as an interned SymbolId.
    /// Using SymbolId (not string) so the clausifier can map bound variables
    /// back to term-bank variables without string lookups.
    SymbolId bound_var = kInvalidId;

    /// Sub-formulas (1 for Not/Quantifiers, 2 for binary connectives).
    std::vector<std::unique_ptr<Formula>> children;

    // ── Factory methods ──

    static std::unique_ptr<Formula> makeAtom(TermId atom);
    static std::unique_ptr<Formula> makeEquality(TermId lhs, TermId rhs, TermId eq_atom);
    static std::unique_ptr<Formula> makeNot(std::unique_ptr<Formula> child);
    static std::unique_ptr<Formula> makeAnd(std::unique_ptr<Formula> lhs,
                                            std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeOr(std::unique_ptr<Formula> lhs,
                                           std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeImplies(std::unique_ptr<Formula> lhs,
                                                std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeIff(std::unique_ptr<Formula> lhs,
                                            std::unique_ptr<Formula> rhs);
    static std::unique_ptr<Formula> makeForall(SymbolId var,
                                               std::unique_ptr<Formula> body);
    static std::unique_ptr<Formula> makeExists(SymbolId var,
                                               std::unique_ptr<Formula> body);
};

}  // namespace atp
