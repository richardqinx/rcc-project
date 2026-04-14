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

#include "ricochet/core/link_hints.hh"
#include "ricochet/core/ui_view.hh"
#include "ricochet/core/utils.hh"
#include <iostream>

namespace ricochet::core {

std::string index_to_hint( std::size_t index )
{
  const std::string alphabet = "asdfghjklqwertyuiopzxcvbnm";
  std::string hint;
  hint += alphabet[( index / alphabet.length() ) % alphabet.length()];
  hint += alphabet[index % alphabet.length()];
  return hint;
}

void toggle_link_hints( PageData& page_data, bool show )
{
  const std::string hl_start = "\033[0;1;35;7m ";
  const std::string hl_end = " \033[0m";

  std::vector<std::string> formatted_hints;
  formatted_hints.reserve( page_data.links.size() );
  for ( std::size_t i = 0; i < page_data.links.size(); ++i ) {
    std::string h = hl_start;
    h += index_to_hint( i );
    h += hl_end;
    formatted_hints.push_back( std::move( h ) );
  }

  for ( auto& line : page_data.lines ) {
    for ( std::size_t i = 0; i < page_data.links.size(); ++i ) {
      const std::string marker = "{L:" + std::to_string( i + 1 ) + "}";
      if ( show ) {
        const std::size_t pos = line.find( marker );
        if ( pos != std::string::npos ) {
          line.replace( pos, marker.length(), formatted_hints[i] );
        }
      } else {
        const std::size_t pos = line.find( formatted_hints[i] );
        if ( pos != std::string::npos ) {
          line.replace( pos, formatted_hints[i].length(), marker );
        }
      }
    }
  }
}

void handle_follow_link( const tui::Terminal& terminal,
                         PageData& page_data,
                         std::string& current_url,
                         std::size_t& scroll_y, // NOLINT(bugprone-easily-swappable-parameters)
                         bool& navigate )
{
  toggle_link_hints( page_data, true );

  draw_view( terminal, page_data.lines, scroll_y, current_url, page_data.title );

  auto [w, h] = terminal.get_size();
  terminal.move_cursor( h, 1 );
  std::cout << "\x1b[2K\033[1;35;7m Follow Hint (2 keys): \033[0m ";
  std::cout.flush();

  std::string input;
  input += terminal.read_key();
  std::cout << input[0];
  std::cout.flush();
  input += terminal.read_key();

  // bool found = false;
  for ( std::size_t i = 0; i < page_data.links.size(); ++i ) {
    if ( index_to_hint( i ) == input ) {
      std::string target = page_data.links[i];
      target = normalize_url( target, current_url );
      target = strip_ddg_tracker( target );
      current_url = target;
      navigate = true;
      // found = true;
      break;
    }
  }

  toggle_link_hints( page_data, false );
}

} // namespace ricochet::core
