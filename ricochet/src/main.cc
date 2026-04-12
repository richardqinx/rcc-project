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

  if ( args.size() < 2 ) {
    std::cerr << "Error: No URL provided.\n";
    print_usage( args[0] );
    return 1;
  }

  const std::string_view input = args[1];

  if ( input == "show" ) {
    if ( args[2] == "w" ) {
      ricochet::core::show_warranty();
    } else if ( args[2] == "c" ) {
      ricochet::core::show_copying();
    } else {
      std::cerr << "Error: Invalid argument for show command.\n";
      print_usage( args[0] );
      return 1;
    }
    return 0;
  }

  std::string start_url( input );
  if ( !start_url.starts_with( "http://" ) && !start_url.starts_with( "https://" ) ) {
    start_url.insert( 0, "https://" );
  }

  try {
    return ricochet::core::Browser::run( start_url );
  } catch ( const std::exception& e ) {
    std::cerr << "Critical Error: " << e.what() << "\n";
    return 1;
  }
}