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

#include "ricochet/render/renderer.hh"
#include <iostream>

namespace ricochet::render {

namespace {

void render_node( const parser::DomNode& node ) // NOLINT(misc-no-recursion)
{
  if ( node.tag_name == "head" || node.tag_name == "style" || node.tag_name == "script" || node.tag_name == "meta"
       || node.tag_name == "title" || node.tag_name == "link" ) {
    return;
  }

  if ( node.tag_name.empty() ) {
    std::cout << node.text_content;
    return;
  }
  if ( node.tag_name == "h1" ) {
    std::cout << "\n\n\033[1;31m";
  } else if ( node.tag_name == "a" ) {
    std::cout << "\033[4;34m";
  } else if ( node.tag_name == "p" || node.tag_name == "div" ) {
    std::cout << "\n";
  }

  for ( const auto& child : node.children ) {
    render_node( child );
  }
  if ( node.tag_name == "h1" || node.tag_name == "a" ) {
    std::cout << "\033[0m";
  }

  if ( node.tag_name == "h1" || node.tag_name == "p" || node.tag_name == "div" ) {
    std::cout << "\n";
  }
}

} // namespace

void Renderer::render( const parser::DomNode& node ) const // NOLINT(readability-convert-member-functions-to-static)
{
  render_node( node );
  std::cout << "\n";
}

} // namespace ricochet::render