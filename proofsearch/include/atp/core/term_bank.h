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

/// @file term_bank.h
/// @brief Hash-consing term pool — guarantees structural sharing.

#include "atp/core/symbol_table.h"
#include "atp/core/term.h"
#include "atp/core/types.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace atp {

/// Global term pool with hash-consing.
/// Every structurally identical term maps to the same TermId.
///
/// Holds a reference to SymbolTable for variable detection.
class TermBank {
  public:
    /// TermBank needs the SymbolTable to distinguish variables from constants
    /// (both have 0 args, but SymbolKind differs).
    explicit TermBank(const SymbolTable& symbols) : symbols_(symbols) {}

    /// Create or retrieve a term. Returns the canonical TermId.
    TermId makeTerm(SymbolId symbol, std::span<const TermId> args);

    /// Create a variable term. Convenience for makeTerm(var, {}).
    TermId makeVar(SymbolId var_symbol);

    /// Look up a term by its ID.
    [[nodiscard]] const Term& getTerm(TermId id) const;

    /// Check if a TermId represents a variable (delegates to SymbolTable).
    [[nodiscard]] bool isVariable(TermId id) const;

    /// Total number of unique terms stored.
    [[nodiscard]] size_t size() const;

    /// Access the underlying SymbolTable.
    [[nodiscard]] const SymbolTable& symbols() const { return symbols_; }

  private:
    struct TermKey {
        SymbolId symbol;
        std::vector<TermId> args;

        bool operator==(const TermKey& other) const = default;
    };

    struct TermKeyHash {
        size_t operator()(const TermKey& key) const {
            size_t seed = std::hash<uint32_t>{}(key.symbol);
            for (TermId arg : key.args) {
                seed ^= std::hash<uint32_t>{}(arg) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };

    const SymbolTable& symbols_;
    std::vector<Term> terms_;
    std::unordered_map<TermKey, TermId, TermKeyHash> term_to_id_;
};

}  // namespace atp
