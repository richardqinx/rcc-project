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
///
/// This module depends only on ClauseStore (not Prover), so it can be
/// reused with any search strategy.

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
    std::string description;  ///< Human-readable description
};

/// Reconstruct the proof trace from the empty clause.
/// Walks parent pointers via ClauseStore, independent of search strategy.
std::vector<ProofStep> extractProof(const ClauseStore& store, ClauseId empty_clause_id,
                                    const TermBank& bank);

/// Format a proof as a human-readable string.
std::string formatProof(const std::vector<ProofStep>& proof, const TermBank& bank);

}  // namespace atp
