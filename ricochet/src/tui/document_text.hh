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

#include "ricochet/tui/document.hh"
#include "ricochet/tui/view.hh"
#include "text.hh"

#include <map>
#include <span>
#include <string>

namespace ricochet::tui::detail {

// Shared by layout and extraction: password positions address only this mask.
std::string control_display_text( const Control& control, const TextService& text );

// Logical reading-order text, independent of wrapping, clipping and decorations.
// Owned together with one immutable document and its revision-zero control values.
class DocumentText
{
public:
  DocumentText( const Document& document, const TextService& text );
  ByteRange offsets( const DocumentRange& range ) const;
  SelectionSnapshot select( const DocumentRange& anchor, const DocumentRange& active ) const;
  std::string extract( const DocumentRange& range ) const;

private:
  struct ControlSpan
  {
    ByteRange object {};
    ByteRange display {};
  };
  std::size_t offset( const DocumentPoint& point ) const;
  void append_blocks( std::span<const Block> blocks, std::size_t depth );
  void append_block( const Block& block, std::size_t depth );
  void append_inline( std::span<const Inline> content );
  void append_text( const Text& text );
  void append_control( ControlId id );

  std::string text_ {};
  std::map<BlockId, ByteRange> blocks_ {};
  std::map<ControlId, ControlSpan> controls_ {};
  std::map<ControlId, const Control*> definitions_ {};
  // Prefix ends coincide with item starts; selecting that start includes its marker.
  std::vector<ByteRange> prefixes_ {};
  const TextService& service_;
};

} // namespace ricochet::tui::detail
