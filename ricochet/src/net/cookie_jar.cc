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

#include "ricochet/net/cookie_jar.hh"
#include "ricochet/net/config.hh"

#include <fstream>

namespace ricochet::net {

void load_cookies_from_file( CookieJar& jar )
{
  std::ifstream file( get_config_path( "cookies.txt" ) );
  if ( !file.is_open() ) {
    return;
  }

  std::string line;
  while ( std::getline( file, line ) ) {
    const std::size_t t1 = line.find( '\t' );
    if ( t1 == std::string::npos ) {
      continue;
    }
    const std::size_t t2 = line.find( '\t', t1 + 1 );
    if ( t2 != std::string::npos ) {
      jar[line.substr( 0, t1 )][line.substr( t1 + 1, t2 - t1 - 1 )] = line.substr( t2 + 1 );
    }
  }
}

CookieJar& get_cookie_jar()
{
  static CookieJar jar;
  static bool initialized = false;
  if ( !initialized ) {
    load_cookies_from_file( jar );
    initialized = true;
  }
  return jar;
}

void save_cookies_to_file( const CookieJar& jar )
{
  std::ofstream file( get_config_path( "cookies.txt" ), std::ios::trunc );
  if ( !file.is_open() ) {
    return;
  }

  for ( const auto& [domain, cookies] : jar ) {
    for ( const auto& [k, v] : cookies ) {
      file << domain << '\t' << k << '\t' << v << '\n';
    }
  }
}

void update_cookies( const std::string& raw_response, // NOLINT(bugprone-easily-swappable-parameters)
                     const std::string& host )
{
  std::size_t pos = 0;
  bool updated = false;
  while ( ( pos = raw_response.find( "Set-Cookie: ", pos ) ) != std::string::npos ) {
    pos += 12;
    const std::size_t end_pos = raw_response.find( '\r', pos );
    if ( end_pos == std::string::npos ) {
      break;
    }

    const std::string cookie_line = raw_response.substr( pos, end_pos - pos );
    const std::size_t semi_pos = cookie_line.find( ';' );
    const std::string cookie_pair
      = ( semi_pos == std::string::npos ) ? cookie_line : cookie_line.substr( 0, semi_pos );

    const std::size_t eq_pos = cookie_pair.find( '=' );
    if ( eq_pos != std::string::npos ) {
      const std::string key = cookie_pair.substr( 0, eq_pos );
      const std::string val = cookie_pair.substr( eq_pos + 1 );
      get_cookie_jar()[host][key] = val;
      updated = true;
    }
  }
  if ( updated ) {
    save_cookies_to_file( get_cookie_jar() );
  }
}

std::string build_cookie_header( const std::string& host )
{
  const auto& jar = get_cookie_jar();
  auto it = jar.find( host );
  if ( it == jar.end() || it->second.empty() ) {
    return "";
  }

  std::string header = "Cookie: ";
  bool first = true;
  for ( const auto& [k, v] : it->second ) {
    if ( !first ) {
      header += "; ";
    }
    header += k;
    header += "=";
    header += v;
    first = false;
  }
  header += "\r\n";
  return header;
}

} // namespace ricochet::net
