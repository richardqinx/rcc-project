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

#pragma once

#include "ricochet/tui/position.hh"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ricochet::tui::detail {

enum class TextUnitKind : std::uint8_t
{
  Glyph,
  LineBreak,
  Tab
};
enum class WordKind : std::uint8_t
{
  Space,
  Word,
  Ideograph,
  Punctuation
};

struct Grapheme
{
  ByteRange bytes {};
  std::wstring display {};
  int columns { 1 };
  TextUnitKind kind { TextUnitKind::Glyph };
  WordKind word { WordKind::Punctuation };
  bool break_after { false };
};

struct TextAnalysis
{
  std::vector<Grapheme> graphemes {};
  // Vim-style reading units: letters/numbers/underscore, punctuation runs,
  // individual ideographs. Whitespace is excluded; offsets remain UTF-8 bytes.
  std::vector<ByteRange> words {};
};

// Called on the UI thread in the same UTF-8 locale as Screen. ICU owns Unicode
// segmentation/properties; POSIX wcwidth owns terminal widths. No curses types.
class TextService
{
public:
  explicit TextService( std::size_t glyph_capacity );
  TextAnalysis analyze( std::string_view utf8, bool line_breaks, bool tabs ) const;

private:
  std::size_t glyph_capacity_;
};

} // namespace ricochet::tui::detail
