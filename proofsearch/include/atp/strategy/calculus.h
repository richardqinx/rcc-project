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

/// @file calculus.h
/// @brief A Calculus bundles the exact set of inference rules + heuristics
///        to use for a specific problem class.
///
/// Think of it as the "algorithm profile" that the Prover loads.
/// Different problems get different calculi:
///   - Pure propositional:  resolution + factoring only
///   - FOL without equality: + full unification
///   - FOL with equality:   + paramodulation + KBO ordering
///   - Arithmetic:          + theory solver integration
///
/// This is the single point where search space control happens.

#include "atp/rewrite/ordering.h"
#include "atp/search/clause_queue.h"
#include "atp/strategy/inference_rule.h"

#include <memory>
#include <string_view>
#include <vector>

namespace atp {

/// A Calculus = a complete specification of how to search for a proof.
/// The Prover is parameterized by this — it never hardcodes which rules to use.
struct Calculus {
    /// Name for logging (e.g., "superposition", "pure_resolution").
    std::string name;

    /// Generating rules (executed in order during the main loop).
    std::vector<std::unique_ptr<GeneratingRule>> generating_rules;

    /// Simplifying rules (applied to each new clause before enqueuing).
    std::vector<std::unique_ptr<SimplifyingRule>> simplifying_rules;

    /// Clause selection heuristic.
    ClauseComparator comparator = bfsCompare;

    /// Term ordering (nullptr if equality reasoning is disabled).
    std::unique_ptr<TermOrdering> ordering = nullptr;

    /// Resource limits.
    size_t max_clauses = 100000;
    size_t max_iterations = 500000;
};

}  // namespace atp
