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
#include "ricochet/render/input_renderer.hh"
#include "ricochet/render/internal.hh"
#include "ricochet/render/table_renderer.hh"
#include "ricochet/render/utils.hh"

namespace ricochet::render {

namespace {

void render_link_end( const parser::DomNode& node, RenderResult& result )
{
  result.text += "\033[0m";
  const std::string href = extract_attr( node.tag_name, "href" );
  if ( !href.empty() && !href.starts_with( "javascript:" ) && !href.starts_with( "#" ) ) {
    result.links.push_back( decode_entities( href ) );
    result.text += "\033[8m{L:" + std::to_string( result.links.size() ) + "}\033[0m";
  }
}

void render_img( const parser::DomNode& node, RenderResult& result )
{
  std::size_t alt_pos = node.tag_name.find( "alt=\"" );
  if ( alt_pos != std::string::npos ) {
    alt_pos += 5;
    const std::size_t alt_end = node.tag_name.find( '"', alt_pos );
    result.text += " \033[90m[Image: " + node.tag_name.substr( alt_pos, alt_end - alt_pos ) + "]\033[0m ";
  } else {
    result.text += " \033[90m[IMAGE]\033[0m ";
  }
}

} // namespace

void render_node( const parser::DomNode& node, RenderResult& result, FormContext ctx ) // NOLINT(misc-no-recursion)
{
  std::string& output = result.text;
  const std::size_t space_pos = node.tag_name.find( ' ' );
  const std::string base_tag
    = ( space_pos == std::string::npos ) ? node.tag_name : node.tag_name.substr( 0, space_pos );

  if ( is_ignored_tag( base_tag ) ) {
    return;
  }
  if ( base_tag.empty() ) {
    output += decode_entities( node.text_content );
    return;
  }

  if ( base_tag == "table" ) {
    render_table( node, result, ctx );
    return;
  }

  if ( base_tag == "form" ) {
    ctx.action = extract_attr( node.tag_name, "action" );
    ctx.method = extract_attr( node.tag_name, "method" );
  }

  const bool is_header = is_heading_tag( base_tag );
  const bool is_link = ( base_tag == "a" );
  const bool is_list_item = ( base_tag == "li" );
  const bool is_input = ( base_tag == "input" || base_tag == "textarea" || base_tag == "button" );

  if ( is_header ) {
    output += "\n\n\033[1;31m";
  } else if ( is_link ) {
    output += "\033[4;34m";
  } else if ( is_list_item ) {
    output += "\n  • ";
  } else if ( is_input ) {
    render_input_node( node, result, ctx, output );
  } else if ( base_tag == "p" || base_tag == "div" || is_header ) {
    if ( !output.empty() && output.back() != '\n' ) {
      output += "\n";
    }
  }

  for ( const auto& child : node.children ) {
    render_node( child, result, ctx );
  }

  if ( is_header ) {
    output += "\033[0m";
  } else if ( is_link ) {
    render_link_end( node, result );
  }

  if ( base_tag == "h1" || base_tag == "p" || base_tag == "div" ) {
    output += "\n";
  }
  if ( base_tag == "img" ) {
    render_img( node, result );
  }
}

RenderResult Renderer::render( // NOLINT(readability-convert-member-functions-to-static)
  const parser::DomNode& node ) const
{
  RenderResult result;
  render_node( node, result, FormContext {} );
  return result;
}

} // namespace ricochet::render