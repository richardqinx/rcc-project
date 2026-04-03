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

/// @file prover.h
/// @brief The Given Clause Loop — main saturation-based proof search.
///
/// V1: Uses a simple ProverConfig with hardcoded resolution + factoring.
/// V2: Will be refactored to accept a Calculus object from strategy/ module
///     for dynamic algorithm loading per problem type.

#include "atp/core/clause.h"
#include "atp/core/clause_store.h"
#include "atp/core/term_bank.h"
#include "atp/search/clause_queue.h"

#include <optional>
#include <queue>
#include <vector>

namespace atp {

/// Result of a proof attempt.
enum class ProverResult : uint8_t {
    kTheorem,       ///< Empty clause derived — conjecture is a theorem
    kSaturation,    ///< Search space saturated — cannot prove
    kTimeout,       ///< Resource limit exceeded
};

/// V1 configuration — simple and direct.
/// Future: replaced by strategy::Calculus for dynamic rule loading.
struct ProverConfig {
    ClauseComparator comparator = bfsCompare;
    size_t max_clauses = 100000;     ///< Safety limit
    size_t max_iterations = 500000;  ///< Safety limit
    bool enable_occurs_check = false;
};

/// Main prover class implementing the Given Clause Loop.
/// V1: hardcodes resolution + factoring + subsumption.
/// V2: will accept a Calculus object for pluggable inference rules.
class Prover {
  public:
    Prover(TermBank& bank, ClauseStore& store, ProverConfig config = {});

    /// Add initial clauses (axioms + negated conjecture).
    void addClauses(std::vector<Clause> clauses);

    /// Run the proof search.
    ProverResult prove();

    /// After a successful proof, retrieve the empty clause ID.
    [[nodiscard]] std::optional<ClauseId> getEmptyClauseId() const;

    /// Access the underlying clause store.
    [[nodiscard]] const ClauseStore& clauseStore() const { return store_; }

  private:
    TermBank& bank_;
    ClauseStore& store_;
    ProverConfig config_;
    std::priority_queue<Clause, std::vector<Clause>, ClauseComparator> unprocessed_;
    std::vector<ClauseId> processed_;
    std::optional<ClauseId> empty_clause_id_;
};

}  // namespace atp
