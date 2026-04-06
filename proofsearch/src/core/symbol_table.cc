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

#include "atp/core/symbol_table.h"

#include "atp/core/types.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace atp {

SymbolId SymbolTable::intern(std::string_view name, SymbolKind kind, uint16_t arity, SortId sort) {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) {
        SymbolId const id = it->second;
        SymbolInfo const& info = infos_[id];
        if (info.kind_ != kind || info.arity_ != arity) {
            throw std::runtime_error("Symbol kind or arity mismatch for: " + std::string(name));
        }
        return id;
    }
    auto new_id = static_cast<SymbolId>(infos_.size());
    infos_.push_back(SymbolInfo{
        .name_ = std::string(name), .kind_ = kind, .arity_ = arity, .sort_ = sort});
    std::string_view const stable_name_view = infos_.back().name_;
    name_to_id_.emplace(stable_name_view, new_id);
    return new_id;
}

std::string_view SymbolTable::getName(SymbolId id) const {
    return infos_[id].name_;
}

const SymbolInfo& SymbolTable::getInfo(SymbolId id) const {
    return infos_[id];
}

bool SymbolTable::isVariable(SymbolId id) const {
    return infos_[id].kind_ == SymbolKind::kVariable;
}

size_t SymbolTable::size() const {
    return infos_.size();
}

}  // namespace atp
