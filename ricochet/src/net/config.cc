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

#include "ricochet/net/config.hh"

#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

namespace ricochet::net {

std::string get_config_path( const std::string& filename )
{
  const char* home = std::getenv( "HOME" ); // NOLINT(concurrency-mt-unsafe)
  const std::string dir = ( home != nullptr ) ? std::string( home ) + "/.ricochet" : ".ricochet";
  static_cast<void>( mkdir( dir.c_str(), 0755 ) );
  return dir + "/" + filename;
}

BrowserConfig& get_browser_config()
{
  static BrowserConfig config { .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                              "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
                                .homepage = "https://wikipedia.org" };
  static bool loaded = false;
  if ( !loaded ) {
    std::ifstream file( get_config_path( "config.txt" ) );
    if ( file.is_open() ) {
      std::string line;
      while ( std::getline( file, line ) ) {
        const std::size_t eq = line.find( '=' );
        if ( eq == std::string::npos ) {
          continue;
        }
        const std::string key = line.substr( 0, eq );
        const std::string val = line.substr( eq + 1 );
        if ( key == "user_agent" ) {
          config.user_agent = val;
        } else if ( key == "homepage" ) {
          config.homepage = val;
        }
      }
    }
    loaded = true;
  }
  return config;
}

} // namespace ricochet::net
