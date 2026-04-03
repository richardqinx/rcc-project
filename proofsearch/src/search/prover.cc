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
#include "atp/core/types.h"
#include "atp/infer/resolution.h"
#include "atp/infer/unification.h"
#include "atp/simplify/subsumption.h"
#include "atp/simplify/tautology.h"

#include <algorithm>

namespace atp {

Prover::Prover(TermBank& bank, ClauseStore& store, ProverConfig config)
    : bank_(bank),
      store_(store),
      config_(std::move(config)),
      unprocessed_(config_.comparator) {}

void Prover::addClauses(std::vector<Clause> clauses) {
    for (auto& clause : clauses) {
        clause.depth = 0;
        ClauseId id = store_.addClause(clause);
        unprocessed_.push(store_.getClause(id));
    }
}

ProverResult Prover::prove() {
    size_t iterations = 0;

    while (!unprocessed_.empty()) {
        // ─── Resource limits ──────────────────────────────────────────
        if (iterations >= config_.max_iterations ||
            store_.size() >= config_.max_clauses) {
            return ProverResult::kTimeout;
        }
        ++iterations;

        // ─── 1. Select the best clause from unprocessed ──────────────
        Clause given = unprocessed_.top();
        unprocessed_.pop();

        // ─── 2. Check if given is itself the empty clause ────────────
        if (given.isEmpty()) {
            empty_clause_id_ = given.id;
            return ProverResult::kTheorem;
        }

        // ─── 3. Forward redundancy checks ────────────────────────────
        if (isTautology(given)) {
            continue;
        }

        bool forward_subsumed = false;
        for (ClauseId pid : processed_) {
            if (subsumes(store_.getClause(pid), given)) {
                forward_subsumed = true;
                break;
            }
        }
        if (forward_subsumed) {
            continue;
        }

        // ─── 4. Backward subsumption ─────────────────────────────────
        // Remove from processed all clauses that are subsumed by given.
        // Use erase-remove idiom for O(n) scan instead of repeated erase.
        std::erase_if(processed_, [&](ClauseId pid) {
            return subsumes(given, store_.getClause(pid));
        });

        // ─── 5. Generate new clauses ─────────────────────────────────
        // Inference BEFORE adding given to processed — matches doc §4.6.
        std::vector<Clause> new_clauses;
        const UnificationConfig uconfig{.enable_occurs_check = config_.enable_occurs_check};

        // 5a. Resolve given with every processed clause (both directions)
        for (ClauseId pid : processed_) {
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

        // 5b. Factor given (only once)
        auto factors = factor(bank_, given, uconfig);
        for (auto& f : factors) {
            new_clauses.push_back(std::move(f));
        }

        // ─── 6. Add given to processed AFTER inference ───────────────
        processed_.push_back(given.id);

        // ─── 7. Process new clauses ──────────────────────────────────
        for (auto& nc : new_clauses) {
            // 7a. Check for empty clause → proof found!
            if (nc.isEmpty()) {
                ClauseId eid = store_.addClause(std::move(nc));
                empty_clause_id_ = eid;
                return ProverResult::kTheorem;
            }

            // 7b. Skip tautologies
            if (isTautology(nc)) {
                continue;
            }

            // 7c. Set depth
            uint32_t d1 = (nc.parent1 != kInvalidId)
                              ? store_.getClause(nc.parent1).depth
                              : 0;
            uint32_t d2 = (nc.parent2 != kInvalidId)
                              ? store_.getClause(nc.parent2).depth
                              : 0;
            nc.depth = static_cast<uint16_t>(std::max(d1, d2) + 1);

            // 7d. Store the clause (needed for forward subsumption check)
            ClauseId nid = store_.addClause(nc);
            const Clause& stored_nc = store_.getClause(nid);

            // 7e. Forward subsumption on new clause
            bool new_subsumed = false;
            for (ClauseId pid : processed_) {
                if (subsumes(store_.getClause(pid), stored_nc)) {
                    new_subsumed = true;
                    break;
                }
            }
            if (new_subsumed) {
                continue;
            }

            // 7f. Enqueue
            unprocessed_.push(stored_nc);
        }
    }

    return ProverResult::kSaturation;
}

std::optional<ClauseId> Prover::getEmptyClauseId() const {
    return empty_clause_id_;
}

}  // namespace atp