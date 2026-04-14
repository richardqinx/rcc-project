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

#include "ricochet/render/table_renderer.hh"
#include "ricochet/render/internal.hh"
#include "ricochet/render/utils.hh"
#include <algorithm>

namespace ricochet::render {

namespace {

std::size_t get_node_text_len( const parser::DomNode& node ) // NOLINT(misc-no-recursion)
{
  std::size_t len = get_visible_length( decode_entities( node.text_content ) );
  for ( const auto& child : node.children ) {
    len += get_node_text_len( child );
  }
  return len;
}

void process_table_row_widths( const parser::DomNode& row, std::vector<std::size_t>& widths )
{
  std::size_t col_idx = 0;
  for ( const auto& cell : row.children ) {
    const std::size_t space_pos = cell.tag_name.find( ' ' );
    const std::string tag
      = ( space_pos == std::string::npos ) ? cell.tag_name : cell.tag_name.substr( 0, space_pos );
    if ( tag == "td" || tag == "th" ) {
      const std::size_t len = get_node_text_len( cell ) + 2;
      if ( col_idx >= widths.size() ) {
        widths.push_back( len );
      } else if ( len > widths[col_idx] ) {
        widths[col_idx] = len;
      }
      col_idx++;
    }
  }
}

std::vector<std::size_t> calc_table_widths( const parser::DomNode& table_node )
{
  std::vector<std::size_t> widths;
  for ( const auto& child : table_node.children ) {
    const std::size_t space_pos = child.tag_name.find( ' ' );
    const std::string tag
      = ( space_pos == std::string::npos ) ? child.tag_name : child.tag_name.substr( 0, space_pos );
    if ( tag == "tr" ) {
      process_table_row_widths( child, widths );
    } else if ( tag == "tbody" || tag == "thead" ) {
      for ( const auto& row : child.children ) {
        if ( row.tag_name.starts_with( "tr" ) ) {
          process_table_row_widths( row, widths );
        }
      }
    }
  }
  for ( auto& w : widths ) {
    w = std::min<std::size_t>( w, 30 );
  }
  return widths;
}

void render_table_row( const parser::DomNode& row,
                       RenderResult& result,
                       const FormContext& ctx,
                       const std::vector<std::size_t>& widths )
{
  std::string& output = result.text;
  output += "\033[90m|\033[0m ";
  std::size_t col_idx = 0;
  for ( const auto& cell : row.children ) {
    const std::size_t cell_space = cell.tag_name.find( ' ' );
    const std::string c_tag
      = ( cell_space == std::string::npos ) ? cell.tag_name : cell.tag_name.substr( 0, cell_space );
    if ( c_tag != "td" && c_tag != "th" ) {
      continue;
    }

    const std::size_t text_start = output.size();
    if ( c_tag == "th" ) {
      output += "\033[1;36m";
    }

    for ( const auto& cell_child : cell.children ) {
      render_node( cell_child, result, ctx );
    }

    if ( c_tag == "th" ) {
      output += "\033[0m";
    }

    const std::string added = output.substr( text_start );
    const std::size_t vis_len = get_visible_length( added );
    const std::size_t tw = ( col_idx < widths.size() ) ? widths[col_idx] : 10;

    if ( vis_len < tw ) {
      output.append( tw - vis_len, ' ' );
    }
    output += " \033[90m|\033[0m ";
    col_idx++;
  }
  output += "\n";
}

} // namespace

void render_table( const parser::DomNode& node, RenderResult& result, const FormContext& ctx )
{
  std::string& output = result.text;
  const std::vector<std::size_t> widths = calc_table_widths( node );

  output += "\n\033[90m+" + std::string( 78, '-' ) + "+\033[0m\n";

  for ( const auto& child : node.children ) {
    const std::size_t child_space = child.tag_name.find( ' ' );
    const std::string child_tag
      = ( child_space == std::string::npos ) ? child.tag_name : child.tag_name.substr( 0, child_space );
    if ( child_tag == "tr" ) {
      render_table_row( child, result, ctx, widths );
    } else if ( child_tag == "tbody" || child_tag == "thead" ) {
      for ( const auto& row : child.children ) {
        if ( row.tag_name.starts_with( "tr" ) ) {
          render_table_row( row, result, ctx, widths );
        }
      }
    }
  }
  output += "\033[90m+" + std::string( 78, '-' ) + "+\033[0m\n";
}

} // namespace ricochet::render
