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

/// @file proof_trace.h
/// @brief Proof reconstruction from the empty clause back to axioms.

#include "atp/core/clause.h"
#include "atp/core/clause_store.h"
#include "atp/core/term_bank.h"

#include <string>
#include <vector>

namespace atp {

/// A single step in a reconstructed proof.
struct ProofStep {
    ClauseId clause_id;
    InferenceRule rule;
    ClauseId parent1;
    ClauseId parent2;
};

/// Reconstruct the proof trace from the empty clause.
/// Returns steps in topological order: axioms first, empty clause last.
std::vector<ProofStep> extractProof(const ClauseStore& store, ClauseId empty_clause_id);

/// Convert a TermId back to a human-readable string.
std::string termToString(TermId id, const TermBank& bank);

/// Convert a Clause to a human-readable string.
std::string clauseToString(const Clause& clause, const TermBank& bank);

/// Format a proof as a human-readable string.
std::string formatProof(const std::vector<ProofStep>& proof, const ClauseStore& store,
                        const TermBank& bank);

}  // namespace atp
