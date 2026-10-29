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

#include "ricochet/net/http_utils.hh"

#include <string>
#include <string_view>

namespace ricochet::net {

std::string dechunk( std::string_view body )
{
  std::string result;
  std::size_t pos = 0;
  while ( pos < body.size() ) {
    auto line_end = body.find( "\r\n", pos );
    if ( line_end == std::string_view::npos ) {
      break;
    }

    const std::string len_str( body.substr( pos, line_end - pos ) );
    unsigned int chunk_size = 0;
    try {
      chunk_size = std::stoul( len_str, nullptr, 16 );
    } catch ( ... ) {
      break;
    }

    if ( chunk_size == 0 ) {
      break;
    }

    pos = line_end + 2;
    if ( pos + chunk_size > body.size() ) {
      break;
    }
    result.append( body.substr( pos, chunk_size ) );

    pos += chunk_size + 2;
  }
  return result;
}

std::string remove_html_comments( std::string html )
{
  std::size_t start_pos = 0;
  while ( ( start_pos = html.find( "<!--", start_pos ) ) != std::string::npos ) {
    auto end_pos = html.find( "-->", start_pos );
    if ( end_pos != std::string::npos ) {
      html.erase( start_pos, end_pos - start_pos + 3 );
    } else {
      html.erase( start_pos );
      break;
    }
  }
  return html;
}

std::string remove_tag_blocks( std::string html, // NOLINT(bugprone-easily-swappable-parameters)
                               const std::string& tag_name )
{
  const std::string open_tag = "<" + tag_name;
  const std::string close_tag = "</" + tag_name + ">";
  std::size_t start_pos = 0;

  while ( ( start_pos = html.find( open_tag, start_pos ) ) != std::string::npos ) {
    const std::size_t end_pos = html.find( close_tag, start_pos );
    if ( end_pos != std::string::npos ) {
      html.erase( start_pos, end_pos - start_pos + close_tag.length() );
    } else {
      html.erase( start_pos );
      break;
    }
  }
  return html;
}

} // namespace ricochet::net
