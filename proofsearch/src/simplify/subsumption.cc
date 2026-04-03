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

#include "atp/simplify/subsumption.h"

namespace atp {

bool subsumes(const Clause& general, const Clause& specific) {
    if (general.size() > specific.size()) {
        return false;
    }
    for (const auto& lit_g : general.literals) {
        bool found = false;
        for (const auto& lit_s : specific.literals) {
            if (lit_g.atom == lit_s.atom && lit_g.is_positive == lit_s.is_positive) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}
}  // namespace atp
