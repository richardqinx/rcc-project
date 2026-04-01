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

/// @file unification.h
/// @brief Most General Unifier (MGU) computation.

#include "atp/core/term_bank.h"
#include "atp/infer/substitution.h"

namespace atp {

/// Configuration for the unification algorithm.
struct UnificationConfig {
    bool enable_occurs_check = false;  ///< Set true for sound but slower unification
};

/// Attempt to unify two terms, producing a most general unifier.
/// @return true if unification succeeds, with the MGU in `subst`.
bool unify(const TermBank& bank, TermId t1, TermId t2, Substitution& subst,
           const UnificationConfig& config = {});

}  // namespace atp
