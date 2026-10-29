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

#include "ricochet/render/utils.hh"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace ricochet::render {

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
        (void)0;
      } catch ( const std::out_of_range& ) {
        (void)0;
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
      } else if ( ( c & 0xF0 ) == 0xE0 || ( c & 0xF8 ) == 0xF0 ) {
        len += 2;
      }
    }
  }
  return len;
}

bool is_ignored_tag( std::string_view tag )
{
  return tag == "head" || tag == "style" || tag == "script" || tag == "meta" || tag == "title" || tag == "link";
}

bool is_heading_tag( std::string_view tag )
{
  return tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4";
}

} // namespace ricochet::render
