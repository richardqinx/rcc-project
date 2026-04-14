/*
 * Ricochet - A minimalist CLI web browser for fast, distraction-free navigation.
 * Copyright (C) 2026  Richard Qin
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

#include <cstddef>
#include <string>
#include <vector>

namespace ricochet::core {

std::vector<std::string> split_into_lines( const std::string& text );
std::string normalize_url( const std::string& target, const std::string& current_url );
std::string strip_ddg_tracker( std::string target );
std::vector<std::string> wrap_lines( const std::vector<std::string>& original_lines, std::size_t wrap_width );

} // namespace ricochet::core
