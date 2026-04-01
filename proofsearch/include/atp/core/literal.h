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

/// @file literal.h
/// @brief A literal is a (possibly negated) atomic formula.

#include "atp/core/types.h"

namespace atp {

/// A literal: a polarity flag plus the TermId of the atom.
struct Literal {
    TermId atom;         ///< TermId of the predicate application
    bool is_positive;    ///< true = positive, false = negated

    /// Complement: flip polarity, same atom.
    [[nodiscard]] Literal complement() const { return {atom, !is_positive}; }

    bool operator==(const Literal& other) const = default;
};

}  // namespace atp
