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
#include "ricochet/net/http_client.hh"
#include "ricochet/parser/html_lexer.hh"
#include "ricochet/parser/tree_builder.hh"
#include "ricochet/render/renderer.hh"
#include "ricochet/tui/terminal.hh"
#include <cstddef>
#include <iostream>
#include <sstream>

namespace ricochet::core {

int Browser::run( std::string_view initial_url )
{
  std::cout << "-> Fetching: " << initial_url << "...\n";

  const net::HttpClient client;
  auto response_result = client.fetch( initial_url );

  if ( !response_result.has_value() ) {
    std::cerr << "[!] Ricochet failed to load page: " << response_result.error() << "\n";
    return 1;
  }

  const auto& response = response_result.value();

  const parser::HtmlLexer lexer;
  const auto tokens = lexer.tokenize( response.body );

  const parser::TreeBuilder builder;
  const auto dom_root = builder.build( tokens );

  // std::cout << "\033[2J\033[1;1H";

  const render::Renderer renderer;
  const std::string rendered_text = renderer.render( dom_root );

  std::vector<std::string> lines;
  std::stringstream ss(rendered_text);
  std::string line;
  while (std::getline(ss, line, '\n')) {
    lines.push_back(line);
  }

  // TUI Event Loop
  const tui::Terminal terminal;

  std::size_t scroll_y = 0;
  const std::size_t screen_height = 24;

  while (true) {
    terminal.clear_screen();

    for (std::size_t i = 0; i < screen_height - 1; ++i) {
      const std::size_t line_idx = scroll_y + i;
      if (line_idx < lines.size()) {
        std::cout << lines[line_idx] << "\r\n";
      } else {
        std::cout << "~\r\n";
      }
    }

    std::cout << "\033[7m" << " URL: " << initial_url 
                  << " | [j] Down  [k] Up  [q] Quit " << "\033[0m";
    std::cout.flush();

    const char c = terminal.read_key();
    if (c == 'q') {
      break; // Break loop, terminal destructor will restore terminal
    }
    if (c == 'j') {
      if (scroll_y + screen_height - 1 < lines.size()) {
        scroll_y++; // Scroll down
      }
    } else if (c == 'k') {
      if (scroll_y > 0) {
        scroll_y--; // Scroll up
      }
    }
  }

  terminal.clear_screen();
  return 0;
}

} // namespace ricochet::core
