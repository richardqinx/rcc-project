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

#include "ricochet/core/browser.hh"
#include "ricochet/net/http_client.hh"
#include <iostream>

namespace ricochet::core {

int Browser::run( std::string_view initial_url )
{
  std::cout << "Browser: Navigating to " << initial_url << "...\n";

  const net::HttpClient client;
  auto response_result = client.fetch( initial_url );
  if ( !response_result.has_value() ) {
    std::cerr << "[!] Ricochet failed to load page: " << response_result.error() << "\n";
    return 1;
  }

  const auto& response = response_result.value();
  std::cout << "\n[=== HTTP Status: " << response.status_code << " ===]\n";
  std::cout << "[=== HTML Content Below ===]\n\n";
  std::cout << response.body << "\n";
  return 0;
}

} // namespace ricochet::core
