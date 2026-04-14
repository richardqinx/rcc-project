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

#include "ricochet/core/search_handler.hh"

namespace ricochet::core {

void find_search_matches( PageData& page_data, const std::string& query )
{
  page_data.last_search = query;
  page_data.match_lines.clear();
  page_data.current_match_idx = 0;
  for ( std::size_t i = 0; i < page_data.original_lines.size(); ++i ) {
    if ( page_data.original_lines[i].find( query ) != std::string::npos ) {
      page_data.match_lines.push_back( i );
    }
  }
}

void apply_search_highlight( PageData& page_data, const std::string& query )
{
  page_data.lines = page_data.original_lines;
  if ( page_data.match_lines.empty() ) {
    return;
  }

  const std::string hl_start = "\033[43;30m";
  const std::string hl_end = "\033[0m";

  for ( const auto match_idx : page_data.match_lines ) {
    auto& line = page_data.lines[match_idx];
    std::size_t pos = 0;
    std::string replacement;
    replacement.reserve( hl_start.length() + query.length() + hl_end.length() );
    replacement += hl_start;
    replacement += query;
    replacement += hl_end;
    while ( ( pos = line.find( query, pos ) ) != std::string::npos ) {
      line.replace( pos, query.length(), replacement );
      pos += hl_start.length() + query.length() + hl_end.length();
    }
  }
}

void execute_search( PageData& page_data, const std::string& query, std::size_t& scroll_y, bool next_match )
{
  if ( query.empty() ) {
    return;
  }

  if ( !next_match || query != page_data.last_search ) {
    find_search_matches( page_data, query );
    apply_search_highlight( page_data, query );
  } else if ( !page_data.match_lines.empty() ) {
    page_data.current_match_idx = ( page_data.current_match_idx + 1 ) % page_data.match_lines.size();
  }

  if ( page_data.match_lines.empty() ) {
    return;
  }

  const std::size_t target_line = page_data.match_lines[page_data.current_match_idx];
  scroll_y = ( target_line > 5 ) ? ( target_line - 5 ) : 0;
}

} // namespace ricochet::core
