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

/// @file hash_utils.h
/// @brief Hash combination utilities.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace atp {

/// Combine a hash with a new value (boost::hash_combine style).
inline void hashCombine(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/// Hash a span of values.
template <typename T>
size_t hashRange(const T* data, size_t count) {
    size_t seed = 0;
    std::hash<T> hasher;
    for (size_t i = 0; i < count; ++i) {
        hashCombine(seed, hasher(data[i]));
    }
    return seed;
}

}  // namespace atp
