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

#include "atp/infer/unification.h"

namespace atp {

/// Attempt binary resolution between two clauses on the specified literal indices.
/// @return The resolvent clause if resolution succeeds, std::nullopt otherwise.
std::optional<Clause> resolve(TermBank& bank, const Clause& c1, size_t lit_idx1,
                              const Clause& c2, size_t lit_idx2,
                              const UnificationConfig& uconfig = {});

/// Generate all possible resolvents between two clauses.
std::vector<Clause> allResolvents(TermBank& bank, const Clause& c1, const Clause& c2,
                                  const UnificationConfig& uconfig = {});

/// Attempt factoring on a single clause (merge two unifiable literals).
std::vector<Clause> factor(TermBank& bank, const Clause& c,
                           const UnificationConfig& uconfig = {});

/// Rename all variables in a clause to fresh ones, avoiding conflicts
/// with variables already in the bank.
Clause renameVariables(const Clause& c, TermBank& bank, SymbolTable& symbols,
                       uint32_t& next_var_id);


}  // namespace atp
