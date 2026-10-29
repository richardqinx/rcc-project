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

/// @file problem_analyzer.h
/// @brief Analyzes input clauses and selects the optimal Calculus.
///
/// Before the Prover starts its main loop, the ProblemAnalyzer inspects
/// the input clause set to determine problem characteristics:
///   - Contains equality?             → enable paramodulation
///   - Pure Horn clauses?             → use SLD resolution
///   - Contains arithmetic literals?  → enable theory solver
///   - Problem size / clause density  → tune resource limits
///
/// This is the "auto mode" that mature ATPs use to avoid loading
/// unnecessary inference rules that would explode the search space.

#include "atp/core/clause.h"
#include "atp/strategy/calculus.h"

#include <vector>

namespace atp {

/// Features detected in the input problem.
struct ProblemFeatures {
    bool has_equality = false;             ///< Contains '=' predicate
    bool is_purely_propositional = false;  ///< No function symbols, no variables
    bool is_horn = false;                  ///< All clauses are Horn clauses
    bool has_existential = false;          ///< Had existential quantifiers (Skolemized)
    size_t num_clauses = 0;
    size_t num_predicates = 0;
    size_t num_functions = 0;
    size_t max_clause_size = 0;
    size_t max_term_depth = 0;
};

/// Analyze the input clauses and extract problem features.
ProblemFeatures analyzeFeatures(const std::vector<Clause>& clauses, const TermBank& bank);

/// Select the optimal calculus based on detected features.
/// This is the core strategy dispatch logic.
std::unique_ptr<Calculus> selectCalculus(const ProblemFeatures& features);

}  // namespace atp
