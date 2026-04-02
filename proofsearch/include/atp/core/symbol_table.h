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

/// @file symbol_table.h
/// @brief Bidirectional mapping between symbol names and SymbolIds,
///        with kind and sort metadata.

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include "atp/core/types.h"

namespace atp {

/// Metadata associated with an interned symbol.
struct SymbolInfo {
    std::string name;
    SymbolKind kind = SymbolKind::kFunction;
    uint16_t arity = 0;
    SortId sort = kUnsorted;  ///< Result sort (for sorted logic)
};

/// Interns symbol names so that each unique string is stored exactly once.
/// Also tracks symbol metadata (kind, arity, sort).
/// Thread-safety: NOT thread-safe (single-threaded prover design).
class SymbolTable {
  public:
    SymbolTable() = default;

    /// Intern a symbol name with metadata. Returns existing ID if already present.
    SymbolId intern(std::string_view name, SymbolKind kind, uint16_t arity = 0,
                    SortId sort = kUnsorted);

    /// Look up the name for a given SymbolId. UB if id is out of range.
    [[nodiscard]] std::string_view getName(SymbolId id) const;

    /// Look up full metadata for a SymbolId.
    [[nodiscard]] const SymbolInfo& getInfo(SymbolId id) const;

    /// Check if a symbol is a variable.
    [[nodiscard]] bool isVariable(SymbolId id) const;

    /// Number of interned symbols.
    [[nodiscard]] size_t size() const;

  private:
    // TODO: Phase 1 implementation
    std::deque<SymbolInfo> infos_;
    std::unordered_map<std::string_view, SymbolId> name_to_id_;
};

}  // namespace atp
