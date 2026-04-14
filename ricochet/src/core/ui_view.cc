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

#include "ricochet/core/ui_view.hh"
#include <iostream>

namespace ricochet::core {

std::string build_footer( std::string_view url, std::size_t width )
{
  const std::string_view keys
    = " | [j/k]Scroll [n/N]Find [/]Nav [f]Hint [i]In [r]Ref [H/L]Back [h]Hist [b/B]Mark [?]Help [q]Quit ";
  const std::string_view prefix = " URL: ";

  std::string footer;
  if ( width <= keys.length() + prefix.length() + 3 ) {
    footer.reserve( prefix.length() + url.length() + keys.length() );
    footer += prefix;
    footer += url;
    footer += keys;
    if ( footer.length() > width && width > 3 ) {
      footer.erase( width - 3 );
      footer += "...";
    }
  } else {
    const std::size_t max_url_len = width - keys.length() - prefix.length();
    footer.reserve( width );
    footer += prefix;
    if ( url.length() > max_url_len ) {
      footer += url.substr( 0, max_url_len - 3 );
      footer += "...";
    } else {
      footer += url;
    }
    footer += keys;
  }

  if ( footer.length() < width ) {
    footer.append( width - footer.length(), ' ' );
  }
  return footer;
}

void draw_view( const tui::Terminal& terminal,
                const std::vector<std::string>& lines,
                std::size_t scroll_y,
                std::string_view url,    // NOLINT(bugprone-easily-swappable-parameters)
                std::string_view title ) // NOLINT(bugprone-easily-swappable-parameters)
{
  terminal.clear_screen();
  auto [width, height] = terminal.get_size();
  const std::size_t usable_height = ( height > 2 ) ? ( height - 2 ) : 1;

  std::string clean_title( title );
  if ( clean_title.empty() ) {
    const std::size_t start = url.find( "://" );
    const std::size_t host_start = ( start == std::string::npos ) ? 0 : start + 3;
    const std::size_t host_end = url.find( '/', host_start );
    clean_title = std::string( url.substr( host_start, host_end - host_start ) );
  }
  for ( char& c : clean_title ) {
    if ( c == '\n' || c == '\r' ) {
      c = ' ';
    }
  }

  std::string header = " PAGE: " + clean_title;
  if ( header.size() < width ) {
    header.append( width - header.size(), ' ' );
  } else if ( header.size() > width && width > 3 ) {
    header = header.substr( 0, width - 3 ) + "...";
  }
  std::cout << "\033[7m" << header << "\033[0m\r\n";

  for ( std::size_t i = 0; i < usable_height; ++i ) {
    if ( scroll_y + i < lines.size() ) {
      std::cout << lines[scroll_y + i] << "\r\n";
    } else {
      std::cout << "~\r\n";
    }
  }

  const std::string footer = build_footer( url, width );
  std::cout << "\033[7m" << footer << "\033[0m";
  std::cout.flush();
}

} // namespace ricochet::core
