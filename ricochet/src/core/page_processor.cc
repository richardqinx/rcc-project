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

#include "ricochet/core/page_processor.hh"
#include <fstream>
#include <ranges>
#include <vector>

namespace ricochet::core {

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

} // namespace ricochet::core
