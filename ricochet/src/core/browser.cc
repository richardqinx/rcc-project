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
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <ranges>
#include <sys/stat.h>

namespace ricochet::core {

struct PageData
{
  std::vector<std::string> lines;
  std::vector<std::string> links;
  std::vector<render::InputField> inputs;
  std::string title;
  std::vector<std::string> original_lines {};
  std::string last_search {};
  std::size_t current_match_idx { 0 };
  std::vector<std::size_t> match_lines {};
};

struct HttpRequest
{
  std::string url {};
  std::string method = "GET";
  std::string body {};
};

namespace {

std::string get_config_path( const std::string& filename )
{
  const char* home = std::getenv( "HOME" ); // NOLINT(concurrency-mt-unsafe)
  const std::string dir = ( home != nullptr ) ? std::string( home ) + "/.ricochet" : ".ricochet";
  static_cast<void>( mkdir( dir.c_str(), 0755 ) );
  return dir + "/" + filename;
}

std::string extract_title( const parser::DomNode& root_node )
{
  std::vector<const parser::DomNode*> stack;
  stack.push_back( &root_node );

  while ( !stack.empty() ) {
    const parser::DomNode* current = stack.back();
    stack.pop_back();

    std::string base_tag = current->tag_name;
    const std::size_t space_pos = base_tag.find( ' ' );
    if ( space_pos != std::string::npos ) {
      base_tag = base_tag.substr( 0, space_pos );
    }

    for ( char& c : base_tag ) {
      if ( c >= 'A' && c <= 'Z' ) {
        c = static_cast<char>( c + 32 );
      }
    }

    if ( base_tag == "title" ) {
      std::string full_title;
      for ( const auto& child : current->children ) {
        if ( child.tag_name.empty() ) {
          full_title += child.text_content;
        }
      }
      if ( !full_title.empty() ) {
        return full_title;
      }
    }

    for ( const auto& child : std::ranges::reverse_view( current->children ) ) {
      stack.push_back( &child );
    }
  }
  return "";
}

void save_bookmark( const std::string& title, // NOLINT(bugprone-easily-swappable-parameters)
                    const std::string& url )
{
  if ( url == "ricochet://bookmarks" ) {
    return;
  }
  std::ofstream file( get_config_path( "bookmarks.txt" ), std::ios::app );
  if ( !file.is_open() ) {
    return;
  }

  std::string safe_title = title;
  if ( safe_title.empty() ) {
    safe_title = url;
  }
  for ( char& c : safe_title ) {
    if ( c == '\n' || c == '\r' || c == '\t' ) {
      c = ' ';
    }
  }
  file << url << '\t' << safe_title << '\n';
}

std::string generate_bookmarks_html()
{
  std::ifstream file( get_config_path( "bookmarks.txt" ) );
  std::string html = "<html><head><title>My Bookmarks</title></head><body><h1>Saved Bookmarks</h1><ul>";
  if ( !file.is_open() ) {
    html += "<li>No bookmarks found. Press 'b' on any page to add one!</li>";
  } else {
    std::string line;
    while ( std::getline( file, line ) ) {
      const std::size_t tab_pos = line.find( '\t' );
      if ( tab_pos != std::string::npos ) {
        const std::string url = line.substr( 0, tab_pos );
        const std::string title = line.substr( tab_pos + 1 );
        html += "<li><a href=\"";
        html += url;
        html += "\">[Link] ";
        html += title;
        html += "</a></li>";
      }
    }
  }
  html += "</ul></body></html>";
  return html;
}

void append_to_history( const std::string& url, // NOLINT(bugprone-easily-swappable-parameters)
                        const std::string& title )
{
  if ( url.starts_with( "ricochet://" ) ) {
    return;
  }
  std::ofstream file( get_config_path( "history.txt" ), std::ios::app );
  if ( !file.is_open() ) {
    return;
  }

  std::string safe_title = title;
  if ( safe_title.empty() ) {
    safe_title = url;
  }
  for ( char& c : safe_title ) {
    if ( c == '\n' || c == '\r' || c == '\t' ) {
      c = ' ';
    }
  }
  file << url << '\t' << safe_title << '\n';
}

std::string generate_history_html()
{
  std::ifstream file( get_config_path( "history.txt" ) );
  std::string html = "<html><head><title>History</title></head><body><h1>Browsing History</h1><ul>";
  if ( !file.is_open() ) {
    html += "<li>No history found. Start browsing!</li>";
  } else {
    std::vector<std::string> lines;
    std::string line;
    while ( std::getline( file, line ) ) {
      lines.push_back( std::move( line ) );
    }

    std::size_t count = 0;
    for ( auto it = lines.rbegin(); it != lines.rend() && count < 100; ++it, ++count ) {
      const std::size_t tab_pos = it->find( '\t' );
      if ( tab_pos != std::string::npos ) {
        const std::string url = it->substr( 0, tab_pos );
        const std::string title = it->substr( tab_pos + 1 );
        html += "<li><a href=\"";
        html += url;
        html += "\">[Link] ";
        html += title;
        html += "</a></li>";
      }
    }
  }
  html += "</ul></body></html>";
  return html;
}

std::string generate_help_html()
{
  return R"(<html>
  <head><title>Ricochet Help Manual</title></head>
  <body>
    <h1>Ricochet User Manual</h1>
    <h2>1. Navigation & Browsing</h2>
    <ul>
      <li><b>j / k</b> : Scroll down / up</li>
      <li><b>H / L</b> : History backward / forward</li>
      <li><b>/</b> : Open prompt to enter a new URL</li>
    </ul>
    <h2>2. Page Interaction</h2>
    <ul>
      <li><b>f</b> : Toggle Link Hints (type the two letters to follow a link instantly)</li>
      <li><b>n</b> : Find in page (search for text)</li>
      <li><b>N</b> : Jump to the next search match</li>
      <li><b>i</b> : Enter form input mode (fill textboxes, click buttons, toggle checkboxes)</li>
    </ul>
    <h2>3. Bookmarks & History</h2>
    <ul>
      <li><b>b</b> : Bookmark the current page</li>
      <li><b>B</b> : View all saved bookmarks</li>
      <li><b>h</b> : View browsing history</li>
    </ul>
    <h2>4. System</h2>
    <ul>
      <li><b>?</b> : Show this help manual</li>
      <li><b>q</b> : Quit Ricochet</li>
    </ul>
  </body>
  </html>)";
}

PageData process_binary_download( const HttpRequest& req, const net::HttpResponse& resp )
{
  std::string fname = "download.dat";
  const std::size_t slash = req.url.rfind( '/' );
  if ( slash != std::string::npos && slash + 1 < req.url.length() ) {
    fname = req.url.substr( slash + 1 );
    const std::size_t q_pos = fname.find( '?' );
    if ( q_pos != std::string::npos ) {
      fname = fname.substr( 0, q_pos );
    }
  }
  if ( fname.empty() ) {
    fname = "download.dat";
  }

  std::ofstream out( fname, std::ios::binary );
  out.write( resp.body.data(), static_cast<std::streamsize>( resp.body.size() ) );
  out.close();

  return { .lines = { "\033[1;32m[ Download Complete ]\033[0m",
                      "",
                      "Saved File : " + fname,
                      "File Type  : " + resp.content_type,
                      "File Size  : " + std::to_string( resp.body.size() ) + " bytes",
                      "",
                      "Press 'H' to return to the previous page." },
           .links = {},
           .inputs = {},
           .title = "Download Manager" };
}

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

PageData load_page( const HttpRequest& req )
{
  std::string html_body;
  if ( req.url == "ricochet://bookmarks" ) {
    html_body = generate_bookmarks_html();
  } else if ( req.url == "ricochet://history" ) {
    html_body = generate_history_html();
  } else if ( req.url == "ricochet://help" ) {
    html_body = generate_help_html();
  } else {
    const net::HttpClient client;
    auto response_result = client.fetch( req.url, req.method, req.body );
    if ( !response_result.has_value() ) {
      return { .lines
               = { "\033[1;31m[!] Failed to load:\033[0m " + req.url, "", "Reason: " + response_result.error() },
               .links = {},
               .inputs = {},
               .title = "Error" };
    }

    const auto& resp = response_result.value();
    std::string c_type = resp.content_type;
    for ( char& c : c_type ) {
      if ( c >= 'A' && c <= 'Z' ) {
        c = static_cast<char>( c + 32 );
      }
    }

    const bool is_text
      = c_type.starts_with( "text/" ) || c_type.contains( "xml" ) || c_type.contains( "json" ) || c_type.empty();

    if ( !is_text ) {
      return process_binary_download( req, resp );
    }
    html_body = response_result.value().body;
  }

  const parser::HtmlLexer lexer;
  const parser::TreeBuilder builder;
  const render::Renderer renderer;

  const auto dom_root = builder.build( lexer.tokenize( html_body ) );
  const render::RenderResult result = renderer.render( dom_root );
  const std::string title = extract_title( dom_root );

  return {
    .lines = split_into_lines( result.text ), .links = result.links, .inputs = result.inputs, .title = title };
}

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

// std::string get_link_input( const tui::Terminal& terminal )
// {
//   auto [width, height] = terminal.get_size();
//   terminal.move_cursor( height, 1 );
//   std::cout << "\x1b[2K\033[7m Follow Link [#]: \033[0m ";
//   std::cout.flush();

//   std::string num_input;
//   terminal.show_cursor( true );
//   while ( true ) {
//     const char ch = terminal.read_key();
//     if ( ch == '\r' || ch == '\n' ) {
//       break;
//     }
//     if ( ch == 27 ) {
//       num_input.clear();
//       break;
//     }
//     if ( ch == 127 || ch == 8 ) {
//       if ( !num_input.empty() ) {
//         num_input.pop_back();
//         std::cout << "\b \b";
//         std::cout.flush();
//       }
//     } else if ( std::isdigit( static_cast<unsigned char>( ch ) ) ) {
//       num_input += ch;
//       std::cout << ch;
//       std::cout.flush();
//     }
//   }
//   terminal.show_cursor( false );
//   return num_input;
// }

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
  if ( show ) {
    formatted_hints.reserve( page_data.links.size() );
    for ( std::size_t i = 0; i < page_data.links.size(); ++i ) {
      std::string h = hl_start;
      h += index_to_hint( i );
      h += hl_end;
      formatted_hints.push_back( std::move( h ) );
    }
  }

  for ( auto& line : page_data.lines ) {
    for ( std::size_t i = 0; i < page_data.links.size(); ++i ) {
      const std::string marker = "{L:" + std::to_string( i + 1 ) + "}";
      const std::size_t pos = line.find( marker );
      if ( pos != std::string::npos ) {
        if ( show ) {
          line.replace( pos, marker.length(), formatted_hints[i] );
        } else {
          const std::size_t end_pos = line.find( hl_end, pos );
          if ( end_pos != std::string::npos ) {
            line.replace( pos, end_pos + hl_end.length() - pos, marker );
          }
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

  bool found = false;
  for ( std::size_t i = 0; i < page_data.links.size(); ++i ) {
    if ( index_to_hint( i ) == input ) {
      std::string target = page_data.links[i];
      target = normalize_url( target, current_url );
      target = strip_ddg_tracker( target );
      current_url = target;
      navigate = true;
      found = true;
      break;
    }
  }

  if ( !found ) {
    toggle_link_hints( page_data, false );
  }
}

enum class InputAction : std::uint8_t
{
  None,
  Quit,
  Navigate
};

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

InputAction submit_form( const render::InputField& target_input,
                         const std::vector<render::InputField>& inputs,
                         const std::string& current_url,
                         std::vector<HttpRequest>& history,
                         std::size_t& history_idx )
{
  std::string body;
  for ( const auto& in : inputs ) {
    if ( in.action == target_input.action && !in.name.empty() && in.type != "submit" && in.type != "button" ) {
      if ( ( in.type == "checkbox" || in.type == "radio" ) && !in.checked ) {
        continue;
      }
      std::string encoded = in.value;
      for ( char& ch : encoded ) {
        if ( ch == ' ' ) {
          ch = '+';
        }
      }
      if ( !body.empty() ) {
        body += "&";
      }
      body += in.name + "=" + encoded;
    }
  }
  const std::string action
    = target_input.action.empty() ? current_url : normalize_url( target_input.action, current_url );
  HttpRequest next_req { .url = action, .method = "GET", .body = "" };
  if ( target_input.method == "post" || target_input.method == "POST" ) {
    next_req.method = "POST";
    next_req.body = body;
  } else {
    const char sep = action.contains( '?' ) ? '&' : '?';
    next_req.url += sep + body;
  }
  history.resize( history_idx + 1 );
  history.push_back( next_req );
  history_idx++;
  return InputAction::Navigate;
}

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

void update_input_value( PageData& page_data, std::size_t index, const std::string& query )
{
  page_data.inputs[index - 1].value = query;
  const std::string marker = "[I" + std::to_string( index ) + ":";
  const std::string replacement = "[I" + std::to_string( index ) + ":" + query + "]";

  auto update_lines = [&]( std::vector<std::string>& lines_arr ) {
    for ( auto& line : lines_arr ) {
      const std::size_t pos = line.find( marker );
      if ( pos != std::string::npos ) {
        const std::size_t end = line.find( ']', pos );
        if ( end != std::string::npos ) {
          line.replace( pos, end - pos + 1, replacement );
        }
      }
    }
  };
  update_lines( page_data.lines );
  update_lines( page_data.original_lines );
}

void update_single_line_ui( std::string& line, const std::string& marker, const render::InputField& in )
{
  const std::size_t pos = line.find( marker );
  if ( pos == std::string::npos ) {
    return;
  }
  const std::size_t end = line.find( ']', pos );
  if ( end == std::string::npos ) {
    return;
  }

  std::string prefix;
  if ( in.type == "checkbox" ) {
    prefix = in.checked ? "[X] " : "[ ] ";
  } else {
    prefix = in.checked ? "(X) " : "( ) ";
  }
  line.replace( pos, end - pos + 1, marker + prefix + in.value + "]" );
}

void update_toggle_ui( PageData& page_data )
{
  for ( std::size_t i = 0; i < page_data.inputs.size(); ++i ) {
    const auto& in = page_data.inputs[i];
    if ( in.type != "checkbox" && in.type != "radio" ) {
      continue;
    }
    const std::string marker = "[I" + std::to_string( i + 1 ) + ":";
    for ( auto& line : page_data.lines ) {
      update_single_line_ui( line, marker, in );
    }
    for ( auto& line : page_data.original_lines ) {
      update_single_line_ui( line, marker, in );
    }
  }
}

InputAction handle_form_input( const tui::Terminal& terminal,
                               PageData& page_data,
                               const std::string& current_url,
                               std::vector<HttpRequest>& history,
                               std::size_t& history_idx )
{
  const std::string num_input = get_terminal_input( terminal, "Select Input [#]:" );
  if ( num_input.empty() ) {
    return InputAction::None;
  }
  std::size_t index = 0;
  try {
    index = std::stoul( num_input );
  } catch ( ... ) {
    return InputAction::None;
  }
  if ( index == 0 || index > page_data.inputs.size() ) {
    return InputAction::None;
  }

  auto& target_input = page_data.inputs[index - 1];
  if ( target_input.type == "submit" || target_input.type == "button" ) {
    return submit_form( target_input, page_data.inputs, current_url, history, history_idx );
  }

  if ( target_input.type == "checkbox" || target_input.type == "radio" ) {
    if ( target_input.type == "radio" ) {
      for ( auto& in : page_data.inputs ) {
        if ( in.name == target_input.name && in.action == target_input.action ) {
          in.checked = false;
        }
      }
      target_input.checked = true;
    } else {
      target_input.checked = !target_input.checked;
    }
    update_toggle_ui( page_data );
    return InputAction::None;
  }

  const std::string query = get_terminal_input( terminal, "Enter Text [I" + std::to_string( index ) + "]:" );
  if ( query.empty() ) {
    return InputAction::None;
  }
  update_input_value( page_data, index, query );
  return InputAction::None;
}

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
