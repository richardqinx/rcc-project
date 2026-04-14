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

#include "ricochet/tui/terminal.hh"

#include <string>
#include <string_view>
#include <vector>

namespace ricochet::core {

std::string build_footer( std::string_view url, std::size_t width );

void draw_view( const tui::Terminal& terminal,
                const std::vector<std::string>& lines,
                std::size_t scroll_y,
                std::string_view url,
                std::string_view title );

} // namespace ricochet::core
