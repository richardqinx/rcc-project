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

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

#include "ricochet/core/form_handler.hh"
#include "ricochet/core/link_hints.hh"
#include "ricochet/core/local_data.hh"
#include "ricochet/core/page_loader.hh"
#include "ricochet/core/search_handler.hh"
#include "ricochet/core/ui_input.hh"
#include "ricochet/core/ui_view.hh"
#include "ricochet/core/utils.hh"

namespace ricochet::core {

namespace {

InputAction push_history( const std::string& url, std::vector<HttpRequest>& history, std::size_t& history_idx )
{
  history.resize( history_idx + 1 );
  history.push_back( HttpRequest { .url = url, .method = "GET", .body = "" } );
  history_idx++;
  return InputAction::Navigate;
}

InputAction process_key( char c,
                         const tui::Terminal& terminal,
                         PageData& page_data,
                         std::string& current_url,
                         std::vector<HttpRequest>& history,
                         std::size_t& history_idx, // NOLINT(bugprone-easily-swappable-parameters)
                         std::size_t& scroll_y )   // NOLINT(bugprone-easily-swappable-parameters)
{
  auto [width, height] = terminal.get_size();
  const std::size_t view_h = ( height > 1 ) ? ( height - 1 ) : 1;

  switch ( c ) {
    case 'q':
      return InputAction::Quit;
    case 'j':
      if ( scroll_y + view_h < page_data.lines.size() ) {
        scroll_y++;
      }
      break;
    case 'k':
      if ( scroll_y > 0 ) {
        scroll_y--;
      }
      break;
    case 'H':
      if ( history_idx > 0 ) {
        history_idx--;
        return InputAction::Navigate;
      }
      break;
    case 'L':
      if ( history_idx + 1 < history.size() ) {
        history_idx++;
        return InputAction::Navigate;
      }
      break;
    case 'r':
      return InputAction::Navigate;
    case 'b':
      save_bookmark( page_data.title, current_url );
      break;
    case 'B':
      return push_history( "ricochet://bookmarks", history, history_idx );
    case '?':
      return push_history( "ricochet://help", history, history_idx );
    case 'h':
      return push_history( "ricochet://history", history, history_idx );
    case 'n': {
      const std::string query = get_terminal_input( terminal, "Find in Page:" );
      execute_search( page_data, query, scroll_y, false );
      break;
    }
    case 'N':
      execute_search( page_data, page_data.last_search, scroll_y, true );
      break;
    case '/': {
      const std::string next_url = get_url_input( terminal );
      if ( !next_url.empty() ) {
        return push_history( next_url, history, history_idx );
      }
      break;
    }
    case 'f': {
      const std::string old_url = current_url;
      bool nav = false;
      handle_follow_link( terminal, page_data, current_url, scroll_y, nav );
      if ( nav && current_url != old_url ) {
        return push_history( current_url, history, history_idx );
      }
      break;
    }
    case 'i':
      if ( !page_data.inputs.empty() ) {
        return handle_form_input( terminal, page_data, current_url, history, history_idx );
      }
      break;
    default:
      break;
  }
  return InputAction::None;
}

} // namespace

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

int Browser::run( std::string_view initial_url )
{
  std::vector<HttpRequest> history;
  history.emplace_back( HttpRequest { .url = std::string( initial_url ), .method = "GET", .body = "" } );
  std::size_t history_idx = 0;
  const tui::Terminal terminal;
  terminal.show_cursor( false );

  while ( true ) {
    const HttpRequest current_req = history[history_idx];
    std::string current_url = current_req.url;
    terminal.clear_screen();
    std::cout << "-> Fetching: " << current_req.url << "...\r\n";
    std::cout.flush();

    auto page_data = load_page( current_req );

    static std::string last_recorded_url;
    if ( current_url != last_recorded_url && !current_url.starts_with( "ricochet://" ) ) {
      append_to_history( current_url, page_data.title );
      last_recorded_url = current_url;
    }

    auto [term_width, term_height] = terminal.get_size();
    const std::size_t wrap_width = ( term_width > 1 ) ? ( term_width - 1 ) : 80;
    page_data.lines = wrap_lines( page_data.lines, wrap_width );
    page_data.original_lines = page_data.lines;

    std::size_t scroll_y = 0;
    bool navigate = false;

    while ( !navigate ) {
      draw_view( terminal, page_data.lines, scroll_y, current_url, page_data.title );
      const char c = terminal.read_key();

      const InputAction action = process_key( c, terminal, page_data, current_url, history, history_idx, scroll_y );
      if ( action == InputAction::Quit ) {
        terminal.clear_screen();
        return 0;
      }
      if ( action == InputAction::Navigate ) {
        navigate = true;
      }
    }
  }
  terminal.clear_screen();
  return 0;
}

} // namespace ricochet::core
