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

#include "atp/core/types.h"

#include <vector>

namespace atp {

/// A substitution is a mapping from variable TermIds to their bound TermIds.
/// Designed as a short-lived, stack-allocated object during unification.
class Substitution {
  public:
    Substitution() = default;

    /// Bind a variable to a term. Returns false if already bound to something different.
    bool bind(TermId var, TermId term);

    /// Look up what a variable is bound to. Returns kInvalidId if unbound.
    [[nodiscard]] TermId lookup(TermId var) const;

    /// Walk the binding chain to the final bound value.
    [[nodiscard]] TermId resolve(TermId id) const;

    /// Clear all bindings.
    void clear();

  private:
    // TODO: Phase 2 implementation
    std::vector<std::pair<TermId, TermId>> bindings_;
};

}  // namespace atp
