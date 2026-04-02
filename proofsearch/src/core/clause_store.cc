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

#include "atp/core/clause_store.h"
#include <cassert>
#include <utility>

namespace atp {

ClauseId ClauseStore::addClause(Clause clause) {
    auto new_id = static_cast<ClauseId>(clauses_.size());
    clause.id = new_id;
    clauses_.push_back(std::move(clause));
    return new_id;
}

const Clause& ClauseStore::getClause(ClauseId id) const {
    assert(id < clauses_.size() && "ClauseId out of bounds!");
    return clauses_[id];
}

Clause& ClauseStore::getMutableClause(ClauseId id) {
    assert(id < clauses_.size() && "ClauseId out of bounds!");
    return clauses_[id];
}

size_t ClauseStore::size() const {
    return clauses_.size();
}

void ClauseStore::forEach(const std::function<void(const Clause&)>& visitor) const {
    for (const auto& clause : clauses_) {
        // TODO: Tombstone mechanism
        visitor(clause);
    }
}

}  // namespace atp
