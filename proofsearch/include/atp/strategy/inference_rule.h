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

/// @file inference_rule.h
/// @brief Abstract interface for pluggable inference rules.
///
/// Each inference rule (resolution, factoring, paramodulation, etc.)
/// is a self-contained object that the Prover can selectively load.
/// This prevents search space explosion by only enabling rules relevant
/// to the problem at hand.

#include "atp/core/clause.h"
#include "atp/core/clause_store.h"
#include "atp/core/term_bank.h"

#include <string_view>
#include <vector>

namespace atp {

/// Abstract base class for all inference rules.
/// The Prover invokes only the rules present in its active calculus.
class InferenceRuleBase {
  public:
    virtual ~InferenceRuleBase() = default;

    /// Human-readable name for logging and proof output.
    [[nodiscard]] virtual std::string_view name() const = 0;

    /// Is this a generating rule (produces new clauses) or simplifying rule
    /// (rewrites/deletes existing clauses)?
    enum class Kind : uint8_t { kGenerating, kSimplifying };
    [[nodiscard]] virtual Kind kind() const = 0;
};

/// Generating inference rule: given a "given clause" and the set of processed
/// clauses, produce new clauses.
class GeneratingRule : public InferenceRuleBase {
  public:
    [[nodiscard]] Kind kind() const final { return Kind::kGenerating; }

    /// Generate new clauses from the given clause against all processed clauses.
    /// @param given     The selected clause from the Unprocessed queue.
    /// @param processed All previously processed clauses (accessed via store).
    /// @param bank      The global term bank.
    /// @return Newly generated clauses (not yet added to the store).
    [[nodiscard]] virtual std::vector<Clause> generate(const Clause& given,
                                                       const std::vector<ClauseId>& processed,
                                                       const ClauseStore& store,
                                                       TermBank& bank) const = 0;
};

/// Simplifying inference rule: rewrite or delete a clause in-place.
class SimplifyingRule : public InferenceRuleBase {
  public:
    [[nodiscard]] Kind kind() const final { return Kind::kSimplifying; }

    /// Attempt to simplify a clause. Returns true if the clause was modified.
    /// If the clause should be deleted entirely, clear its literals.
    virtual bool simplify(Clause& clause, const ClauseStore& store, TermBank& bank) const = 0;
};

}  // namespace atp
