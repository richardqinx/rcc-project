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
  std::string match = std::string( attr_name ) + "=\"";
  std::size_t pos = tag_name.find( match );
  if ( pos != std::string::npos ) {
    pos += match.length();
    const std::size_t end = tag_name.find( '"', pos );
    if ( end != std::string::npos ) {
      return tag_name.substr( pos, end - pos );
    }
  }
  match = std::string( attr_name ) + "='";
  pos = tag_name.find( match );
  if ( pos != std::string::npos ) {
    pos += match.length();
    const std::size_t end = tag_name.find( '\'', pos );
    if ( end != std::string::npos ) {
      return tag_name.substr( pos, end - pos );
    }
  }
  return "";
}

void render_link_end( const parser::DomNode& node, RenderResult& result )
{
  result.text += "\033[0m";
  const std::string href = extract_attr( node.tag_name, "href" );
  if ( !href.empty() && !href.starts_with( "javascript:" ) && !href.starts_with( "#" ) ) {
    result.links.push_back( decode_entities( href ) );
    result.text += " \033[7m[" + std::to_string( result.links.size() ) + "]\033[0m";
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

std::string build_input_hint( const std::string& tag_name )
{
  std::string placeholder = extract_attr( tag_name, "placeholder" );
  std::string type = extract_attr( tag_name, "type" );
  std::string val = extract_attr( tag_name, "value" );

  if ( !placeholder.empty() ) {
    return placeholder;
  }
  if ( type == "submit" || type == "button" ) {
    return val.empty() ? "Submit" : val;
  }
  if ( type == "checkbox" || type == "radio" ) {
    return type;
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
  result.inputs.push_back( { extract_attr( node.tag_name, "name" ), ctx.action, ctx.method, default_val, type } );

  std::string hint = build_input_hint( node.tag_name );
  if ( base_tag == "button" ) {
    hint = "Submit";
  }
  if ( hint != "hidden" ) {
    output += " \033[7;33m[I" + std::to_string( result.inputs.size() ) + ":" + hint + "]\033[0m ";
  }
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