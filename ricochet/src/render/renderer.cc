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

namespace ricochet::render {

namespace {

std::string decode_entities( std::string text ) // NOLINT(readability-convert-member-functions-to-static)
{
  constexpr std::array<std::pair<std::string_view, std::string_view>, 24> entities
    = { { { "&nbsp;", " " },   { "&lt;", "<" },     { "&gt;", ">" },    { "&amp;", "&" },   { "&quot;", "\"" },
          { "&copy;", "©" },   { "&reg;", "®" },    { "&trade;", "™" }, { "&euro;", "€" },  { "&pound;", "£" },
          { "&yen;", "¥" },    { "&cent;", "¢" },   { "&sect;", "§" },  { "&para;", "¶" },  { "&hellip;", "…" },
          { "&dagger;", "†" }, { "&Dagger;", "‡" }, { "&bull;", "•" },  { "&prime;", "′" }, { "&Prime;", "″" },
          { "&lsaquo;", "‹" }, { "&rsaquo;", "›" }, { "&oline;", "‾" }, { "&frasl;", "⁄" } } };

  for ( const auto& [entity, replacement] : entities ) {
    std::size_t pos = 0;
    while ( ( pos = text.find( entity, pos ) ) != std::string::npos ) {
      text.replace( pos, entity.length(), replacement );
      pos += replacement.length();
    }
  }
  return text;
}

std::string extract_href( const std::string& tag_name )
{
  std::size_t pos = tag_name.find( "href=\"" );
  if ( pos != std::string::npos ) {
    pos += 6;
    const std::size_t end = tag_name.find( '\"', pos );
    if ( end != std::string::npos ) {
      return tag_name.substr( pos, end - pos );
    }
  }
  pos = tag_name.find( "href='" );
  if ( pos != std::string::npos ) {
    pos += 6;
    const std::size_t end = tag_name.find( '\'', pos );
    if ( end != std::string::npos ) {
      return tag_name.substr( pos, end - pos );
    }
  }
  return "";
}

void render_node( const parser::DomNode& node, // NOLINT(misc-no-recursion)
                  std::string& output,
                  std::vector<std::string>& links )
{
  const std::size_t space_pos = node.tag_name.find( ' ' );
  const std::string base_tag
    = ( space_pos == std::string::npos ) ? node.tag_name : node.tag_name.substr( 0, space_pos );

  if ( base_tag == "head" || base_tag == "style" || base_tag == "script" || base_tag == "meta"
       || base_tag == "title" || base_tag == "link" ) {
    return;
  }

  if ( base_tag.empty() ) {
    output += decode_entities( node.text_content );
    return;
  }

  const bool is_header = ( base_tag == "h1" || base_tag == "h2" || base_tag == "h3" || base_tag == "h4" );
  const bool is_link = ( base_tag == "a" );

  if ( is_header ) {
    output += "\n\n\033[1;31m";
  } else if ( is_link ) {
    output += "\033[4;34m"; // 【核心修复】：必须是 output += ，绝对不能用 std::cout
  } else if ( base_tag == "p" || base_tag == "div" || is_header ) {
    if ( !output.empty() && output.back() != '\n' ) {
      output += "\n";
    }
  }

  for ( const auto& child : node.children ) {
    render_node( child, output, links );
  }

  if ( is_header ) {
    output += "\033[0m";
  } else if ( is_link ) {
    output += "\033[0m";
    const std::string href = extract_href( node.tag_name );
    if ( !href.empty() && !href.starts_with( "javascript:" ) && !href.starts_with( "#" ) ) {
      links.push_back( href );
      output += " \033[7m[" + std::to_string( links.size() ) + "]\033[0m"; // 打印数字标签！
    }
  }

  if ( base_tag == "h1" || base_tag == "p" || base_tag == "div" ) {
    output += "\n";
  }
}

} // namespace

RenderResult Renderer::render( // NOLINT(readability-convert-member-functions-to-static)
  const parser::DomNode& node ) const
{
  RenderResult result;
  render_node( node, result.text, result.links );
  return result;
}

} // namespace ricochet::render