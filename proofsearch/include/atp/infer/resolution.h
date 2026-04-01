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

/// @file resolution.h
/// @brief Binary resolution and factoring inference rules.

#include "atp/core/clause.h"
#include "atp/core/term_bank.h"

#include <optional>
#include <vector>

namespace atp {

/// Attempt binary resolution between two clauses on the specified literal indices.
/// @return The resolvent clause if resolution succeeds, std::nullopt otherwise.
std::optional<Clause> resolve(const TermBank& bank, const Clause& c1, size_t lit_idx1,
                              const Clause& c2, size_t lit_idx2);

/// Generate all possible resolvents between two clauses.
std::vector<Clause> allResolvents(const TermBank& bank, const Clause& c1, const Clause& c2);

/// Attempt factoring on a single clause (merge two unifiable literals).
std::vector<Clause> factor(const TermBank& bank, const Clause& c);

}  // namespace atp
