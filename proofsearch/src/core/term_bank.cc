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

#include "atp/core/term_bank.h"
#include "atp/core/types.h"
#include <cassert>

namespace atp {

TermId TermBank::makeTerm(SymbolId symbol, std::span<const TermId> args) {
    TermKey key{.symbol = symbol, .args = {args.begin(), args.end()}};
    auto it = term_to_id_.find(key);
    if (it != term_to_id_.end()) {
        return it->second;
    }
    auto new_id = static_cast<TermId>(terms_.size());
    Term new_term{.symbol_id = symbol, .args = {args.begin(), args.end()}};
    terms_.push_back(std::move(new_term));
    term_to_id_.emplace(std::move(key), new_id);
    return new_id;
}

TermId TermBank::makeVar(SymbolId var_symbol) {
    return makeTerm(var_symbol, {});
}

const Term& TermBank::getTerm(TermId id) const {
    assert(id < terms_.size() && "TermId out of bounds!");
    return terms_[id];
}

bool TermBank::isVariable(TermId id) const {
    assert(id < terms_.size() && "TermId out of bounds!");
    return symbols_.isVariable(terms_[id].symbol_id);
}

size_t TermBank::size() const {
    return terms_.size();
}

}  // namespace atp
