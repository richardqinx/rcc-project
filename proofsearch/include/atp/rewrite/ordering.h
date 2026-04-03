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

/// @file ordering.h
/// @brief Term orderings for equality reasoning (KBO, LPO).
///
/// Term orderings are the foundation of equality-based theorem proving.
/// They ensure that rewriting always terminates by enforcing a well-founded
/// ordering on terms. Required by paramodulation, demodulation, and
/// ordered resolution.

#include "atp/core/term_bank.h"
#include "atp/core/types.h"

namespace atp {

/// Ordering comparison result.
enum class OrderResult : uint8_t {
    kLessThan,      ///< t1 < t2
    kEqual,         ///< t1 = t2
    kGreaterThan,   ///< t1 > t2
    kIncomparable,  ///< t1 and t2 are incomparable
};

/// Abstract base for term orderings.
/// Concrete implementations: KBO (Knuth-Bendix), LPO (Lexicographic Path).
class TermOrdering {
  public:
    virtual ~TermOrdering() = default;

    /// Compare two terms under the ordering.
    [[nodiscard]] virtual OrderResult compare(const TermBank& bank, TermId t1, TermId t2) const = 0;
};

}  // namespace atp
