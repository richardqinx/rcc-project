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

#include "atp/simplify/tautology.h"
#include <cstddef>

 namespace atp {
    bool isTautology(const Clause& clause) {
        for (size_t i = 0; i < clause.size(); ++i) {
            for (size_t j = i + 1; j < clause.size(); ++j) {
                if (clause.literals[i].atom == clause.literals[j].atom && 
                    clause.literals[i].is_positive != clause.literals[j].is_positive) {
                    return true;
                }
            }
        }
        return false;
    }
}