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

/// @file clause_store.h
/// @brief Central clause storage — owns all clauses, assigns ClauseIds.
///
/// Extracted from Prover to separate storage responsibility from search logic.
/// This enables swapping search strategies (OTTER loop / DISCOUNT loop)
/// without touching clause lifecycle management.

#include "atp/core/clause.h"
#include "atp/core/types.h"

#include <functional>
#include <vector>

namespace atp {

/// Central clause store. Owns all clauses and assigns unique ClauseIds.
/// All modules access clauses through this store via ClauseId references.
class ClauseStore {
  public:
    ClauseStore() = default;

    /// Add a clause, assign it a ClauseId, and return the ID.
    ClauseId addClause(Clause clause);

    /// Access a clause by its ID. UB if id is out of range.
    [[nodiscard]] const Clause& getClause(ClauseId id) const;

    /// Mutable access (for marking clauses as deleted, etc.)
    [[nodiscard]] Clause& getMutableClause(ClauseId id);

    /// Total number of stored clauses.
    [[nodiscard]] size_t size() const;

    /// Iterate over all clauses with a visitor.
    void forEach(const std::function<void(const Clause&)>& visitor) const;

  private:
    std::vector<Clause> clauses_;
};

}  // namespace atp
