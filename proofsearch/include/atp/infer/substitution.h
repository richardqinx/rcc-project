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

/// @file substitution.h
/// @brief Variable binding map: Var -> Term.
///
/// Short-lived object created during unification, applied to build resolvents,
/// then discarded. Never stored long-term.

#include "atp/core/term_bank.h"
#include "atp/core/types.h"

#include <unordered_map>

namespace atp {

/// A substitution σ maps variable TermIds to their bound TermIds.
///
/// Example: σ = { X → f(a), Y → Z }
///   lookup(X) = f(a)     (direct binding)
///   resolve(X) = f(a)    (same — f(a) is not a variable)
///   resolve(Y) = Z       (if Z is unbound, stops at Z)
///   resolve(Y) = f(b)    (if Z → f(b) also in σ, walks the chain)
class Substitution {
  public:
    Substitution() = default;

    /// Bind a variable to a term. Returns false if already bound to a different term.
    bool bind(TermId var, TermId term);

    /// Look up what a variable is directly bound to. Returns kInvalidId if unbound.
    [[nodiscard]] TermId lookup(TermId var) const;

    /// Walk the binding chain to the final value.
    /// If var → Y → f(a), returns f(a).
    /// Requires TermBank to check whether intermediate values are variables.
    [[nodiscard]] TermId resolve(TermId id, const TermBank& bank) const;

    /// Apply this substitution to a term recursively, creating new terms in the bank.
    /// Free (unbound) variables are left as-is.
    [[nodiscard]] TermId apply(TermId term, TermBank& bank) const;

    /// Check if a variable occurs anywhere inside a term (for occurs check).
    [[nodiscard]] bool occursIn(TermId var, TermId term, const TermBank& bank) const;

    /// Clear all bindings.
    void clear();

    /// Number of bindings.
    [[nodiscard]] size_t size() const { return bindings_.size(); }

    /// Check if empty.
    [[nodiscard]] bool empty() const { return bindings_.empty(); }

  private:
    std::unordered_map<TermId, TermId> bindings_;
};

}  // namespace atp
