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

#include <expected>
#include <string>
#include <string_view>

namespace ricochet::net {

struct ParsedUrl
{
  std::string host {};
  std::string path {};
};

std::expected<ParsedUrl, std::string> parse_url( std::string_view url );

std::string extract_redirect_url( const std::string& raw_response, bool is_https, const std::string& host );

} // namespace ricochet::net
