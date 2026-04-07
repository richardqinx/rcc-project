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

#include "atp/search/prover.h"

#include "atp/core/clause.h"
#include "atp/core/clause_store.h"
#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include "atp/infer/resolution.h"
#include "atp/infer/unification.h"
#include "atp/simplify/subsumption.h"
#include "atp/simplify/tautology.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace atp {

Prover::Prover(TermBank& bank, ClauseStore& store, ProverConfig config)
    : bank_(bank), store_(store), config_(std::move(config)), unprocessed_(config_.comparator_) {}

void Prover::addClauses(std::vector<Clause> clauses) {
    for (auto& clause : clauses) {
        clause.depth_ = 0;
        ClauseId const id = store_.addClause(clause);
        unprocessed_.push(store_.getClause(id));
    }
}

bool Prover::hitResourceLimit(size_t iterations) const {
    return iterations >= config_.max_iterations_ || store_.size() >= config_.max_clauses_;
}

Clause Prover::popGivenClause() {
    Clause given = unprocessed_.top();
    unprocessed_.pop();
    return given;
}

bool Prover::isForwardSubsumedByProcessed(const Clause& clause) const {
    return std::ranges::any_of(processed_,
                               [&](ClauseId pid) { return subsumes(store_.getClause(pid), clause); });
}

bool Prover::shouldSkipGiven(const Clause& given) const {
    return isTautology(given) || isForwardSubsumedByProcessed(given);
}

void Prover::applyBackwardSubsumption(const Clause& given) {
    std::erase_if(processed_, [&](ClauseId pid) { return subsumes(given, store_.getClause(pid)); });
}

std::vector<Clause> Prover::generateNewClauses(const Clause& given) const {
    std::vector<Clause> new_clauses;
    const UnificationConfig uconfig{.enable_occurs_check_ = config_.enable_occurs_check_};

    for (ClauseId const pid : processed_) {
        const Clause& pc = store_.getClause(pid);

        auto resolvents1 = allResolvents(bank_, given, pc, uconfig);
        auto resolvents2 = allResolvents(bank_, pc, given, uconfig);

        for (auto& r : resolvents1) {
            new_clauses.push_back(std::move(r));
        }
        for (auto& r : resolvents2) {
            new_clauses.push_back(std::move(r));
        }
    }

    auto factors = factor(bank_, given, uconfig);
    for (auto& f : factors) {
        new_clauses.push_back(std::move(f));
    }

    return new_clauses;
}

uint16_t Prover::computeDepth(const Clause& clause) const {
    uint32_t const d1 = (clause.parent1_ != kInvalidId) ? store_.getClause(clause.parent1_).depth_ : 0;
    uint32_t const d2 = (clause.parent2_ != kInvalidId) ? store_.getClause(clause.parent2_).depth_ : 0;
    return static_cast<uint16_t>(std::max(d1, d2) + 1);
}

std::optional<ClauseId> Prover::processNewClauses(std::vector<Clause> new_clauses) {
    for (auto& nc : new_clauses) {
        if (nc.isEmpty()) {
            return store_.addClause(std::move(nc));
        }

        if (isTautology(nc)) {
            continue;
        }

        nc.depth_ = computeDepth(nc);

        ClauseId const nid = store_.addClause(nc);
        const Clause& stored_nc = store_.getClause(nid);
        if (isForwardSubsumedByProcessed(stored_nc)) {
            continue;
        }

        unprocessed_.push(stored_nc);
    }

    return std::nullopt;
}

ProverResult Prover::prove() {
    size_t iterations = 0;

    while (!unprocessed_.empty()) {
        if (hitResourceLimit(iterations)) {
            return ProverResult::kTimeout;
        }
        ++iterations;

        Clause given = popGivenClause();

        if (given.isEmpty()) {
            empty_clause_id_ = given.id_;
            return ProverResult::kTheorem;
        }

        if (shouldSkipGiven(given)) {
            continue;
        }

        applyBackwardSubsumption(given);
        std::vector<Clause> new_clauses = generateNewClauses(given);
        processed_.push_back(given.id_);

        const std::optional<ClauseId> derived_empty = processNewClauses(std::move(new_clauses));
        if (derived_empty.has_value()) {
            empty_clause_id_ = derived_empty;
            return ProverResult::kTheorem;
        }
    }

    return ProverResult::kSaturation;
}

std::optional<ClauseId> Prover::getEmptyClauseId() const {
    return empty_clause_id_;
}

}  // namespace atp