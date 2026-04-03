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

/// @file discrim_tree.h
/// @brief Perfect Discrimination Tree for fast term indexing.
///
/// Used by subsumption and resolution to quickly find candidate clauses
/// whose literals unify with a query term. Supports three query modes:
///   - Unifiable:       find terms that can be unified with the query
///   - Instances:       find terms that are instances of the query (more specific)
///   - Generalizations: find terms that are generalizations of the query (more general)

#include "atp/core/clause.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"

#include <vector>

namespace atp {

/// A match result from the discrimination tree.
struct IndexMatch {
    ClauseId clause_id;
    size_t lit_index;
};

/// A discrimination tree index over terms.
/// Supports insertion and three types of retrieval queries.
class DiscrimTree {
  public:
    explicit DiscrimTree(const TermBank& bank);

    /// Insert a term (identified by its TermId) with an associated clause/literal ref.
    void insert(TermId term, ClauseId clause_id, size_t lit_index);

    /// Retrieve all entries whose indexed term is unifiable with `query`.
    [[nodiscard]] std::vector<IndexMatch> queryUnifiable(TermId query) const;

    /// Retrieve all entries whose indexed term is an instance of `query`.
    /// (query is more general, indexed term is more specific)
    [[nodiscard]] std::vector<IndexMatch> queryInstances(TermId query) const;

    /// Retrieve all entries whose indexed term is a generalization of `query`.
    /// (indexed term is more general, query is more specific)
    /// This is the key operation for forward subsumption.
    [[nodiscard]] std::vector<IndexMatch> queryGeneralizations(TermId query) const;

    /// Remove all entries for a given clause.
    void removeClause(ClauseId clause_id);

    /// Number of indexed entries.
    [[nodiscard]] size_t size() const;

  private:
    const TermBank& bank_;
    // TODO: Phase 3 implementation — trie node arena
};

}  // namespace atp
