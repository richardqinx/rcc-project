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

#include "ricochet/core/utils.hh"

namespace ricochet::core {

std::vector<std::string> split_into_lines( const std::string& text )
{
  std::vector<std::string> lines;
  std::size_t start = 0;
  while ( start < text.length() ) {
    const std::size_t end = text.find( '\n', start );
    if ( end == std::string::npos ) {
      lines.push_back( text.substr( start ) );
      break;
    }
    lines.push_back( text.substr( start, end - start ) );
    start = end + 1;
  }
  return lines;
}

std::string normalize_url( const std::string& target, const std::string& current_url )
{
  if ( target.starts_with( "/" ) ) {
    if ( target.starts_with( "//" ) ) {
      return "https:" + target;
    }
    const std::size_t slash_pos = current_url.find( '/', 8 );
    const std::string domain
      = ( slash_pos == std::string::npos ) ? current_url : current_url.substr( 0, slash_pos );
    return domain + target;
  }
  if ( !target.starts_with( "http" ) ) {
    const std::size_t last_slash = current_url.rfind( '/' );
    if ( last_slash > 7 ) {
      return current_url.substr( 0, last_slash + 1 ) + target;
    }
    return current_url + "/" + target;
  }
  return target;
}

std::string strip_ddg_tracker( std::string target )
{
  const std::size_t uddg_pos = target.find( "uddg=" );
  if ( uddg_pos != std::string::npos ) {
    const std::size_t end_pos = target.find( '&', uddg_pos );
    const std::string encoded = target.substr( uddg_pos + 5, end_pos - ( uddg_pos + 5 ) );
    std::string decoded;
    for ( std::size_t i = 0; i < encoded.length(); ++i ) {
      if ( encoded[i] == '%' && i + 2 < encoded.length() ) {
        const std::string hex = encoded.substr( i + 1, 2 );
        try {
          decoded += static_cast<char>( std::stoi( hex, nullptr, 16 ) );
        } catch ( const std::exception& ) {
          (void)0;
        }
        i += 2;
      } else {
        decoded += encoded[i];
      }
    }
    return decoded;
  }
  return target;
}

std::vector<std::string> wrap_lines( const std::vector<std::string>& original_lines, std::size_t wrap_width )
{
  std::vector<std::string> wrapped_lines;
  for ( const auto& line : original_lines ) {
    std::string current_line;
    std::size_t vis_count = 0;
    bool in_ansi = false;
    for ( const char c : line ) {
      if ( c == '\x1b' ) {
        in_ansi = true;
      }
      if ( in_ansi ) {
        current_line += c;
        if ( c == 'm' ) {
          in_ansi = false;
        }
      } else {
        if ( vis_count >= wrap_width ) {
          wrapped_lines.push_back( current_line + "\033[0m" );
          current_line.clear();
          vis_count = 0;
        }
        current_line += c;
        vis_count++;
      }
    }
    if ( !current_line.empty() || line.empty() ) {
      wrapped_lines.push_back( current_line + "\033[0m" );
    }
  }
  return wrapped_lines;
}

} // namespace ricochet::core
