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
#include <exception>
#include <iostream>

namespace ricochet::core {

struct PageData
{
  std::vector<std::string> lines;
  std::vector<std::string> links;
};

namespace {
PageData load_page( std::string_view url )
{
  const net::HttpClient client;
  auto response_result = client.fetch( url );
  if ( !response_result.has_value() ) {
    return { .lines = { "[!] Failed to load: " + std::string( url ) }, .links = {} };
  }

  const parser::HtmlLexer lexer;
  const parser::TreeBuilder builder;
  const render::Renderer renderer;

  const auto dom_root = builder.build( lexer.tokenize( response_result.value().body ) );
  const render::RenderResult result = renderer.render( dom_root );

  std::vector<std::string> lines;
  std::size_t start = 0;
  while ( start < result.text.length() ) {
    const std::size_t end = result.text.find( '\n', start );
    if ( end == std::string::npos ) {
      lines.push_back( result.text.substr( start ) );
      break;
    }
    lines.push_back( result.text.substr( start, end - start ) );
    start = end + 1;
  }
  return { .lines = lines, .links = result.links };
}

void draw_view( const tui::Terminal& terminal,
                const std::vector<std::string>& lines,
                std::size_t scroll_y,
                std::string_view url )
{
  terminal.clear_screen();
  auto [width, height] = terminal.get_size();
  const std::size_t usable_height = ( height > 1 ) ? ( height - 1 ) : 1;

  for ( std::size_t i = 0; i < usable_height; ++i ) {
    if ( scroll_y + i < lines.size() ) {
      std::cout << lines[scroll_y + i] << "\r\n";
    } else {
      std::cout << "~\r\n";
    }
  }
  std::cout << "\033[7m URL: " << url << " | [j/k] Scroll [/] Jump [f] Follow [q] Quit \033[0m";
  std::cout.flush();
}

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

  if ( !input.empty() && !input.starts_with( "http://" ) && !input.starts_with( "https://" ) ) {
    input.insert( 0, "https://" );
  }
  return input;
}

std::string get_link_input( const tui::Terminal& terminal )
{
  auto [width, height] = terminal.get_size();
  terminal.move_cursor( height, 1 );
  std::cout << "\x1b[2K\033[7m Follow Link [#]: \033[0m ";
  std::cout.flush();

  std::string num_input;
  terminal.show_cursor( true );
  while ( true ) {
    const char ch = terminal.read_key();
    if ( ch == '\r' || ch == '\n' ) {
      break;
    }
    if ( ch == 27 ) {
      num_input.clear();
      break;
    }
    if ( ch == 127 || ch == 8 ) {
      if ( !num_input.empty() ) {
        num_input.pop_back();
        std::cout << "\b \b";
        std::cout.flush();
      }
    } else if ( std::isdigit( static_cast<unsigned char>( ch ) ) ) {
      num_input += ch;
      std::cout << ch;
      std::cout.flush();
    }
  }
  terminal.show_cursor( false );
  return num_input;
}

void handle_follow_link( const tui::Terminal& terminal,
                         const std::vector<std::string>& links,
                         std::string& current_url,
                         bool& navigate )
{
  const std::string num_input = get_link_input( terminal );
  if ( !num_input.empty() ) {
    try {
      const std::size_t index = std::stoul( num_input );
      if ( index > 0 && index <= links.size() ) {
        std::string target = links[index - 1];

        if ( target.starts_with( "/" ) ) {
          if ( target.starts_with( "//" ) ) {
            target = "https:" + target; // "//example.com"
          } else {
            const std::size_t slash_pos = current_url.find( '/', 8 ); // 跳过 https://
            const std::string domain
              = ( slash_pos == std::string::npos ) ? current_url : current_url.substr( 0, slash_pos );
            target = domain + target; // "/about" -> "https://gnu.org/about"
          }
        } else if ( !target.starts_with( "http" ) ) {
          const std::size_t last_slash = current_url.rfind( '/' );
          if ( last_slash > 7 ) {
            target = current_url.substr( 0, last_slash + 1 ) + target;
          } else {
            target = current_url + "/" + target;
          }
        }

        current_url = target;
        navigate = true;
      }
    } catch ( const std::exception& e ) {
      (void)e; // NOLINT
    }
  }
}

} // namespace

int Browser::run( std::string_view initial_url )
{
  std::cout << "-> Fetching: " << initial_url << "...\n";

  std::string current_url { initial_url };
  const tui::Terminal terminal;
  terminal.clear_screen();

  while ( !current_url.empty() ) {
    auto page_data = load_page( current_url );
    const auto& lines = page_data.lines;
    const auto& links = page_data.links;
    std::size_t scroll_y = 0;
    bool navigate = false;

    while ( !navigate ) {
      draw_view( terminal, lines, scroll_y, current_url );

      const char c = terminal.read_key();
      auto [width, height] = terminal.get_size();
      const std::size_t view_h = ( height > 1 ) ? ( height - 1 ) : 1;

      if ( c == 'q' ) {
        terminal.clear_screen();
        return 0;
      }
      if ( c == 'j' && scroll_y + view_h < lines.size() ) {
        scroll_y++;
      } else if ( c == 'k' && scroll_y > 0 ) {
        scroll_y--;
      } else if ( c == '/' ) {
        std::string next_url = get_url_input( terminal );
        if ( !next_url.empty() ) {
          current_url = std::move( next_url );
          navigate = true;
        }
      } else if ( c == 'f' ) {
        handle_follow_link( terminal, links, current_url, navigate );
      }
    }
  }

  terminal.clear_screen();
  return 0;
}

} // namespace ricochet::core
