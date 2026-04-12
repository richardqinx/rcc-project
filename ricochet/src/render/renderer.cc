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
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace ricochet::render {

namespace {

std::string decode_entities( std::string text )
{
  constexpr std::array<std::pair<std::string_view, std::string_view>, 25> entities
    = { { { "&nbsp;", " " },   { "&lt;", "<" },     { "&gt;", ">" },     { "&amp;", "&" },   { "&quot;", "\"" },
          { "&apos;", "'" },   { "&copy;", "©" },   { "&reg;", "®" },    { "&trade;", "™" }, { "&euro;", "€" },
          { "&pound;", "£" },  { "&yen;", "¥" },    { "&cent;", "¢" },   { "&sect;", "§" },  { "&para;", "¶" },
          { "&hellip;", "…" }, { "&dagger;", "†" }, { "&Dagger;", "‡" }, { "&bull;", "•" },  { "&prime;", "′" },
          { "&Prime;", "″" },  { "&lsaquo;", "‹" }, { "&rsaquo;", "›" }, { "&oline;", "‾" }, { "&frasl;", "⁄" } } };

  for ( const auto& [entity, replacement] : entities ) {
    std::size_t pos = 0;
    while ( ( pos = text.find( entity, pos ) ) != std::string::npos ) {
      text.replace( pos, entity.length(), replacement );
      pos += replacement.length();
    }
  }

  std::size_t pos = 0;
  while ( ( pos = text.find( "&#", pos ) ) != std::string::npos ) {
    const std::size_t end = text.find( ';', pos );
    if ( end != std::string::npos && end - pos < 10 ) {
      const std::string code = text.substr( pos + 2, end - pos - 2 );
      try {
        std::uint32_t cp = 0;
        if ( !code.empty() && ( code[0] == 'x' || code[0] == 'X' ) ) {
          cp = std::stoul( code.substr( 1 ), nullptr, 16 );
        } else {
          cp = std::stoul( code );
        }
        if ( cp < 128 ) {
          text.replace( pos, end - pos + 1, 1, static_cast<char>( cp ) );
          continue;
        }
      } catch ( const std::invalid_argument& ) {
        (void)0; // Not a valid number, leave as-is
      } catch ( const std::out_of_range& ) {
        (void)0; // Value too large, leave as-is
      }
    }
    pos++;
  }
  return text;
}

std::string extract_attr( const std::string& tag_name, std::string_view attr_name )
{
  std::string match = " " + std::string( attr_name ) + "=";
  std::size_t pos = tag_name.find( match );

  if ( pos == std::string::npos ) {
    match = std::string( attr_name ) + "=";
    if ( !tag_name.starts_with( match ) ) {
      return "";
    }
    pos = 0;
  } else {
    pos += 1;
  }

  pos += attr_name.length() + 1;
  if ( pos >= tag_name.length() ) {
    return "";
  }

  const char quote = tag_name[pos];
  if ( quote == '"' || quote == '\'' ) {
    const std::size_t end = tag_name.find( quote, pos + 1 );
    if ( end != std::string::npos ) {
      return tag_name.substr( pos + 1, end - pos - 1 );
    }
    return "";
  }

  const std::size_t end = tag_name.find_first_of( " >/", pos );
  return tag_name.substr( pos, ( end == std::string::npos ) ? std::string::npos : end - pos );
}

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

std::string build_input_hint( const std::string& tag_name, bool checked )
{
  std::string placeholder = extract_attr( tag_name, "placeholder" );
  const std::string type = extract_attr( tag_name, "type" );
  std::string val = extract_attr( tag_name, "value" );

  if ( !placeholder.empty() ) {
    return placeholder;
  }
  if ( type == "submit" || type == "button" ) {
    return val.empty() ? "Submit" : val;
  }
  if ( type == "checkbox" ) {
    return checked ? "[X] " + val : "[ ] " + val;
  }
  if ( type == "radio" ) {
    return checked ? "(X) " + val : "( ) " + val;
  }
  if ( type == "hidden" ) {
    return "hidden";
  }
  if ( !val.empty() ) {
    return val;
  }
  return "text";
}

struct FormContext
{
  std::string action;
  std::string method;
};

bool is_ignored_tag( std::string_view tag )
{
  return tag == "head" || tag == "style" || tag == "script" || tag == "meta" || tag == "title" || tag == "link";
}

bool is_heading_tag( std::string_view tag )
{
  return tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4";
}

void render_input_node( const parser::DomNode& node,
                        RenderResult& result,
                        const FormContext& ctx,
                        std::string& output )
{
  std::string type = extract_attr( node.tag_name, "type" );
  const std::size_t space_pos = node.tag_name.find( ' ' );
  const std::string base_tag
    = ( space_pos == std::string::npos ) ? node.tag_name : node.tag_name.substr( 0, space_pos );
  if ( base_tag == "button" && type.empty() ) {
    type = "submit";
  }
  const std::string default_val = extract_attr( node.tag_name, "value" );
  const bool is_checked = node.tag_name.contains( "checked" );
  result.inputs.push_back(
    { extract_attr( node.tag_name, "name" ), ctx.action, ctx.method, default_val, type, is_checked } );

  std::string hint = build_input_hint( node.tag_name, is_checked );
  if ( base_tag == "button" ) {
    hint = "Submit";
  }
  if ( hint != "hidden" ) {
    output += " \033[7;33m[I" + std::to_string( result.inputs.size() ) + ":" + hint + "]\033[0m ";
  }
}

void render_node( const parser::DomNode& node, RenderResult& result, FormContext ctx );

std::size_t get_visible_length( std::string_view str )
{
  std::size_t len = 0;
  bool in_ansi = false;
  for ( std::size_t i = 0; i < str.length(); ++i ) {
    const auto c = static_cast<unsigned char>( str[i] );
    if ( c == 0x1B ) {
      in_ansi = true;
    } else if ( in_ansi && c == 'm' ) {
      in_ansi = false;
    } else if ( !in_ansi ) {
      if ( ( c & 0x80 ) == 0 || ( c & 0xE0 ) == 0xC0 ) {
        len += 1;
      } // ASCII (width 1) Latin/Greek (width 1)
      else if ( ( c & 0xF0 ) == 0xE0 || ( c & 0xF8 ) == 0xF0 ) {
        len += 2;
      } // CJK (width 2) Emoji/Rare (width 2)
    }
  }
  return len;
}

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

void render_table_row( const parser::DomNode& row, // NOLINT(misc-no-recursion)
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

void render_table( const parser::DomNode& node, // NOLINT(misc-no-recursion)
                   RenderResult& result,
                   const FormContext& ctx )
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

void render_node( const parser::DomNode& node, // NOLINT(misc-no-recursion)
                  RenderResult& result,
                  FormContext ctx )
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

} // namespace

RenderResult Renderer::render( // NOLINT(readability-convert-member-functions-to-static)
  const parser::DomNode& node ) const
{
  RenderResult result;
  render_node( node, result, FormContext {} );
  return result;
}

} // namespace ricochet::render