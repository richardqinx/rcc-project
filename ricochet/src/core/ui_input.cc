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

#include "ricochet/core/ui_input.hh"
#include "ricochet/core/browser.hh"
#include <iostream>

namespace ricochet::core {

std::string get_url_input( const tui::Terminal& terminal )
{
  auto [width, height] = terminal.get_size();
  terminal.move_cursor( height, 1 );
  std::cout << "\x1b[2K\033[7m URL: \033[0m ";
  std::cout.flush();

  std::string input;
  terminal.show_cursor( true );
  while ( true ) {
    const char ch = terminal.read_key();
    if ( ch == '\r' || ch == '\n' ) {
      break;
    }
    if ( ch == 27 ) {
      input.clear();
      break;
    }
    if ( ch == 127 || ch == 8 ) {
      if ( !input.empty() ) {
        input.pop_back();
        std::cout << "\b \b";
        std::cout.flush();
      }
    } else if ( std::isprint( static_cast<unsigned char>( ch ) ) ) {
      input += ch;
      std::cout << ch;
      std::cout.flush();
    }
  }
  terminal.show_cursor( false );

  return Browser::format_url( std::move( input ) );
}

std::string get_terminal_input( const tui::Terminal& terminal, const std::string& prompt )
{
  auto [width, height] = terminal.get_size();
  terminal.move_cursor( height, 1 );
  std::cout << "\x1b[2K\033[7m " << prompt << " \033[0m ";
  std::cout.flush();

  std::string input;
  terminal.show_cursor( true );
  while ( true ) {
    const char ch = terminal.read_key();
    if ( ch == '\r' || ch == '\n' ) {
      break;
    }
    if ( ch == 27 ) {
      input.clear();
      break;
    }
    if ( ch == 127 || ch == 8 ) {
      if ( !input.empty() ) {
        input.pop_back();
        std::cout << "\b \b";
        std::cout.flush();
      }
    } else if ( std::isprint( static_cast<unsigned char>( ch ) ) ) {
      input += ch;
      std::cout << ch;
      std::cout.flush();
    }
  }
  terminal.show_cursor( false );
  return input;
}

} // namespace ricochet::core
