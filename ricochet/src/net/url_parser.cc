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

#include "ricochet/net/url_parser.hh"

#include <cctype>

namespace ricochet::net {

std::expected<ParsedUrl, std::string> parse_url( std::string_view url )
{
  std::string_view prefix = "http://";
  if ( url.starts_with( "https://" ) ) {
    prefix = "https://";
  } else if ( url.starts_with( "http://" ) ) {
    prefix = "http://";
  } else {
    return std::unexpected( "Only 'http://' or 'https://' URLs are supported." );
  }

  url.remove_prefix( prefix.length() );

  const auto sep_pos = url.find_first_of( "/?#" );

  ParsedUrl result;
  if ( sep_pos == std::string_view::npos ) {
    result.host = std::string( url );
    result.path = "/";
  } else {
    result.host = std::string( url.substr( 0, sep_pos ) );
    std::string_view path_view = url.substr( sep_pos );
    const auto hash_pos = path_view.find( '#' );
    if ( hash_pos != std::string_view::npos ) {
      path_view = path_view.substr( 0, hash_pos );
    }
    result.path = std::string( path_view );
    if ( result.path.empty() || result.path[0] != '/' ) {
      result.path.insert( 0, "/" );
    }
  }
  return result;
}

std::string extract_redirect_url( const std::string& raw_response, bool is_https, const std::string& host )
{
  auto loc_pos = raw_response.find( "Location: " );
  if ( loc_pos == std::string::npos ) {
    loc_pos = raw_response.find( "location: " );
  }
  if ( loc_pos != std::string::npos ) {
    loc_pos += 10;
    auto end_pos = raw_response.find( '\r', loc_pos );
    if ( end_pos == std::string::npos ) {
      end_pos = raw_response.find( '\n', loc_pos );
    }
    if ( end_pos != std::string::npos ) {
      std::string next_url = raw_response.substr( loc_pos, end_pos - loc_pos );
      while ( !next_url.empty() && std::isspace( static_cast<unsigned char>( next_url.front() ) ) ) {
        next_url.erase( 0, 1 );
      }
      if ( next_url.starts_with( "//" ) ) {
        next_url = ( is_https ? "https:" : "http:" ) + next_url;
      } else if ( next_url.starts_with( "/" ) ) {
        next_url = ( is_https ? "https://" : "http://" ) + host + next_url;
      }
      return next_url;
    }
  }
  return "";
}

} // namespace ricochet::net
