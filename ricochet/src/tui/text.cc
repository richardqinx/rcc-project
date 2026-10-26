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

#include "text.hh"

#include <climits>
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <unicode/ubrk.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/utext.h>
#include <utility>

namespace ricochet::tui::detail {
namespace {

using UnicodeText = std::unique_ptr<UText, decltype( &utext_close )>;
using Breaks = std::unique_ptr<UBreakIterator, decltype( &ubrk_close )>;

Breaks breaks_for( UBreakIteratorType type, UText& text )
{
  UErrorCode status = U_ZERO_ERROR;
  Breaks breaks( ubrk_open( type, "root", nullptr, 0, &status ), &ubrk_close );
  if ( U_FAILURE( status ) ) {
    throw std::runtime_error( u_errorName( status ) );
  }
  ubrk_setUText( breaks.get(), &text, &status );
  if ( U_FAILURE( status ) ) {
    throw std::runtime_error( u_errorName( status ) );
  }
  return breaks;
}

WordKind word_kind( UChar32 codepoint )
{
  if ( u_isUWhiteSpace( codepoint ) ) {
    return WordKind::Space;
  }
  if ( u_hasBinaryProperty( codepoint, UCHAR_IDEOGRAPHIC ) ) {
    return WordKind::Ideograph;
  }
  if ( codepoint == '_' || u_isalnum( codepoint ) ) {
    return WordKind::Word;
  }
  return WordKind::Punctuation;
}

} // namespace

TextService::TextService( std::size_t glyph_capacity ) : glyph_capacity_( glyph_capacity ) {}

std::string TextService::encode( std::u32string_view input ) const
{
  // ICU's UTF-32 conversion takes a signed 32-bit length, like its break iterator.
  if ( input.size() > INT32_MAX ) {
    throw std::length_error( "Input exceeds ICU conversion length capacity" );
  }
  const std::vector<UChar32> codepoints( input.begin(), input.end() );
  // fromUTF32 requires a non-null pointer even for a zero-length input.
  const UChar32 empty = 0;
  const auto converted = icu::UnicodeString::fromUTF32( codepoints.empty() ? &empty : codepoints.data(),
                                                        static_cast<int32_t>( codepoints.size() ) );
  if ( converted.isBogus() ) {
    throw std::runtime_error( "ICU could not convert editor input" );
  }
  std::string result;
  converted.toUTF8String( result );
  return result;
}

TextAnalysis TextService::analyze( std::string_view utf8, bool line_breaks, bool tabs ) const
{
  // ICU BreakIterator exposes int32_t native offsets even with a UTF-8 UText.
  if ( utf8.size() > INT32_MAX ) {
    throw std::length_error( "Text exceeds ICU break-iterator offset capacity" );
  }
  UErrorCode status = U_ZERO_ERROR;
  UnicodeText text(
    utext_openUTF8( nullptr, utf8.empty() ? "" : utf8.data(), static_cast<int64_t>( utf8.size() ), &status ),
    &utext_close );
  if ( U_FAILURE( status ) ) {
    throw std::runtime_error( u_errorName( status ) );
  }
  auto characters = breaks_for( UBRK_CHARACTER, *text );
  auto lines = breaks_for( UBRK_LINE, *text );
  TextAnalysis result;
  int32_t begin = ubrk_first( characters.get() );
  int32_t line_end = ubrk_next( lines.get() );
  WordKind previous = WordKind::Space;
  for ( int32_t end = ubrk_next( characters.get() ); end != UBRK_DONE;
        begin = end, end = ubrk_next( characters.get() ) ) {
    while ( line_end != UBRK_DONE && line_end < end ) {
      line_end = ubrk_next( lines.get() );
    }
    Grapheme glyph { .bytes = { static_cast<std::size_t>( begin ), static_cast<std::size_t>( end ) },
                     .break_after = line_end == end };
    utext_setNativeIndex( text.get(), begin );
    const auto first = utext_next32( text.get() );
    glyph.word = word_kind( first );
    const auto source = utf8.substr( glyph.bytes.start, glyph.bytes.end - glyph.bytes.start );
    if ( line_breaks && ( source == "\n" || source == "\r\n" ) ) {
      glyph.kind = TextUnitKind::LineBreak;
      glyph.columns = 0;
    } else if ( tabs && source == "\t" ) {
      glyph.kind = TextUnitKind::Tab;
      glyph.display = L" ";
    } else {
      const int width = ::wcwidth( static_cast<wchar_t>( first ) );
      glyph.display.push_back( static_cast<wchar_t>( first ) );
      bool representable = width > 0 && !u_hasBinaryProperty( first, UCHAR_DEFAULT_IGNORABLE_CODE_POINT )
                           && !u_hasBinaryProperty( first, UCHAR_EMOJI_PRESENTATION );
      while ( utext_getNativeIndex( text.get() ) < end ) {
        const auto codepoint = utext_next32( text.get() );
        glyph.display.push_back( static_cast<wchar_t>( codepoint ) );
        // ncurses accepts one spacing character followed by nonspacing marks.
        // Joiners, selectors and extra spacing characters use one visible fallback.
        representable = representable && ::wcwidth( static_cast<wchar_t>( codepoint ) ) == 0
                        && u_charType( codepoint ) == U_NON_SPACING_MARK
                        && !u_hasBinaryProperty( codepoint, UCHAR_DEFAULT_IGNORABLE_CODE_POINT );
      }
      if ( !representable || glyph.display.size() > glyph_capacity_ ) {
        glyph.display = L"?";
        glyph.columns = 1;
      } else {
        glyph.columns = width;
      }
    }
    if ( glyph.word != WordKind::Space ) {
      if ( glyph.word == previous && glyph.word != WordKind::Ideograph ) {
        result.words.back().end = glyph.bytes.end;
        result.graphemes.back().word_end = false;
      } else {
        result.words.push_back( glyph.bytes );
        glyph.word_start = true;
      }
    }
    glyph.word_end = glyph.word != WordKind::Space;
    previous = glyph.word;
    result.graphemes.push_back( std::move( glyph ) );
  }
  return result;
}

} // namespace ricochet::tui::detail
