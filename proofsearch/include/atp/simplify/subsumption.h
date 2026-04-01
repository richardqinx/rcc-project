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

/// @file subsumption.h
/// @brief Forward and backward subsumption checks (backed by discrimination tree).

#include "atp/core/clause.h"
#include "atp/index/discrim_tree.h"

#include <vector>

namespace atp {

/// Subsumption engine: checks if a new clause is subsumed by existing ones
/// (forward) or subsumes existing ones (backward).
class SubsumptionEngine {
  public:
    explicit SubsumptionEngine(const TermBank& bank);

    /// Returns true if `candidate` is subsumed by any existing clause.
    [[nodiscard]] bool isForwardSubsumed(const Clause& candidate) const;

    /// Returns IDs of existing clauses that are subsumed by `new_clause`.
    [[nodiscard]] std::vector<ClauseId> backwardSubsumed(const Clause& new_clause) const;

    /// Add a clause to the subsumption index.
    void addClause(const Clause& clause);

    /// Remove a clause from the subsumption index.
    void removeClause(ClauseId id);

  private:
    DiscrimTree index_;
    // TODO: Phase 5 implementation
};

}  // namespace atp
