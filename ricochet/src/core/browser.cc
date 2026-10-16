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

#include <fstream>

#include "ricochet/core/local_data.hh"

namespace ricochet::core {

std::string Browser::format_url( std::string input )
{
  if ( input.empty() ) {
    return input;
  }

  if ( input.starts_with( "http://" ) || input.starts_with( "https://" ) ) {
    return input;
  }

  const bool is_domain = input.contains( '.' ) && !input.contains( ' ' );
  if ( is_domain ) {
    input.insert( 0, "https://" );
    return input;
  }

  for ( char& ch : input ) {
    if ( ch == ' ' ) {
      ch = '+';
    }
  }
  return "https://lite.duckduckgo.com/lite/?q=" + input;
}

std::string Browser::get_configured_homepage()
{
  std::ifstream file( get_config_path( "config.txt" ) );
  if ( file.is_open() ) {
    std::string line;
    while ( std::getline( file, line ) ) {
      const std::size_t eq = line.find( '=' );
      if ( eq != std::string::npos && line.substr( 0, eq ) == "homepage" ) {
        return line.substr( eq + 1 );
      }
    }
  }
  return "https://wikipedia.org";
}

} // namespace ricochet::core
