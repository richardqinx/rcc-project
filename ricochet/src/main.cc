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

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "ricochet/core/browser.hh"
#include "ricochet/core/license.hh"

namespace {

void print_usage( std::string_view program_name )
{
  std::cout << "Usage: " << program_name << " [URL]\n"
            << "Example: " << program_name << " https://wikipedia.org\n";
}

} // namespace

int main( int argc, char* argv[] )
{
  const std::span<char*> args_view( argv, static_cast<size_t>( argc ) );
  const std::vector<std::string_view> args( args_view.begin(), args_view.end() );

  ricochet::core::print_license_notice();

  if ( args.size() >= 2 && args[1] == "show" ) {
    if ( args.size() >= 3 && args[2] == "w" ) {
      ricochet::core::show_warranty();
      return 0;
    }
    if ( args.size() >= 3 && args[2] == "c" ) {
      ricochet::core::show_copying();
      return 0;
    }
    std::cerr << "Error: Invalid argument for show command.\n";
    print_usage( args[0] );
    return 1;
  }

  std::string start_url;
  if ( args.size() < 2 ) {
    start_url = ricochet::core::Browser::get_configured_homepage();
  } else {
    start_url = std::string( args[1] );
  }

  start_url = ricochet::core::Browser::format_url( std::move( start_url ) );

  try {
    return ricochet::core::Browser::run( start_url );
  } catch ( const std::exception& e ) {
    std::cerr << "Critical Error: " << e.what() << "\n";
    return 1;
  }
}