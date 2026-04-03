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

/// @file clause.h
/// @brief A clause is a disjunction of literals, with provenance tracking.
///
/// NOTE: V1 uses std::vector<Literal> for simplicity.
/// Future optimization: migrate to a Clause Arena with contiguous storage.

#include "atp/core/literal.h"
#include "atp/core/types.h"

#include <vector>

namespace atp {

/// A clause: a disjunction of literals plus provenance for proof reconstruction.
struct Clause {
    ClauseId id = kInvalidId;
    std::vector<Literal> literals;

    // Provenance (for proof trace)
    InferenceRule rule = InferenceRule::kInput;
    ClauseId parent1 = kInvalidId;
    ClauseId parent2 = kInvalidId;
    uint16_t depth = 0;  ///< Derivation depth (for BFS scheduling)

    /// True if the clause has no literals (contradiction).
    [[nodiscard]] bool isEmpty() const { return literals.empty(); }

    /// Number of literals.
    [[nodiscard]] size_t size() const { return literals.size(); }
};

}  // namespace atp
