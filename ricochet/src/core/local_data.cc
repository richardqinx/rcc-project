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

#include "ricochet/core/local_data.hh"
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <vector>

namespace ricochet::core {

std::string get_config_path( const std::string& filename )
{
  const char* home = std::getenv( "HOME" ); // NOLINT(concurrency-mt-unsafe)
  const std::string dir = ( home != nullptr ) ? std::string( home ) + "/.ricochet" : ".ricochet";
  static_cast<void>( mkdir( dir.c_str(), 0755 ) );
  return dir + "/" + filename;
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

} // namespace ricochet::core
