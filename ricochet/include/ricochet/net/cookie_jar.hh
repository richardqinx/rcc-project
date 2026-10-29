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

#include <string>
#include <unordered_map>

namespace ricochet::net {

using CookieJar = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

void load_cookies_from_file( CookieJar& jar );
CookieJar& get_cookie_jar();
void save_cookies_to_file( const CookieJar& jar );

void update_cookies( const std::string& raw_response, const std::string& host );
std::string build_cookie_header( const std::string& host );

} // namespace ricochet::net
