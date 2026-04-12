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

#include "ricochet/tui/terminal.hh"
#include <cstdlib>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ricochet::tui {

Terminal::Terminal()
{
  // get terminal attributes
  tcgetattr( STDIN_FILENO, &original_termios_ );

  struct termios raw = original_termios_;

  // close echo, close canonical mode
  // close terminal signals (ISIG disable Ctrl+C and Ctrl+Z, let the program handle it)
  raw.c_lflag &= ~( ECHO | ICANON | ISIG );

  // close software flow control (Ctrl+S, Ctrl+Q)
  raw.c_iflag &= ~( IXON | ICRNL );

  // apply new terminal attributes
  tcsetattr( STDIN_FILENO, TCSAFLUSH, &raw );
}

Terminal::~Terminal()
{
  show_cursor( true );
  // RAII: restore terminal attributes when program exits
  tcsetattr( STDIN_FILENO, TCSAFLUSH, &original_termios_ );
}

char Terminal::read_key() const // NOLINT(readability-convert-member-functions-to-static)
{
  char c = '\0';
  // read 1 byte
  if ( read( STDIN_FILENO, &c, 1 ) == -1 ) {
    return '\0';
  }
  return c;
}

void Terminal::clear_screen() const // NOLINT(readability-convert-member-functions-to-static)
{
  // ANSI escape codes: \x1b[2J clear screen, \x1b[H move cursor to (1,1)
  std::cout << "\x1b[2J\x1b[H";
}

std::pair<std::size_t, std::size_t> Terminal::get_size() // NOLINT(readability-convert-member-functions-to-static)
  const
{
  struct winsize w {};
  if ( ioctl( STDOUT_FILENO, TIOCGWINSZ, &w ) == -1 ) {
    return { 80, 24 };
  }
  return { static_cast<std::size_t>( w.ws_col ), static_cast<std::size_t>( w.ws_row ) };
}

void Terminal::move_cursor( std::size_t row, std::size_t col ) const // NOLINT
{
  // ANSI: \x1b[H move to 1,1; \x1b[row;colH move to specified row and column
  std::cout << "\x1b[" << row << ";" << col << "H";
}

void Terminal::show_cursor( bool visible ) const // NOLINT
{
  // \x1b[?25h show cursor, \x1b[?25l hide cursor
  std::cout << ( visible ? "\x1b[?25h" : "\x1b[?25l" );
}

} // namespace ricochet::tui