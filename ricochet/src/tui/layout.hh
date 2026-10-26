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

#include "paint.hh"
#include "ricochet/tui/view.hh"
#include "text.hh"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace ricochet::tui::detail {

class DocumentText;

using HitTarget = std::variant<LinkId, ControlId>;

struct LayoutPoint
{
  std::size_t row { 0 };
  std::int64_t column { 0 };
};

struct LayoutHit
{
  DisplayPosition position {};
  DocumentRange unit {};
  std::optional<HitTarget> target {};
};

struct ScrollWindow
{
  BlockId block {};
  std::int64_t left { 0 };
  std::int64_t width { 0 };
  std::int64_t offset { 0 };
  std::int64_t maximum { 0 };
};

enum class WordMotion : std::uint8_t
{
  Next,
  Previous,
  End
};

struct LayoutGlyph
{
  LayoutPoint position {};
  int columns { 0 };
  std::wstring text {};
  TextStyle style {};
  DocumentRange source {};
  std::optional<HitTarget> target {};
  // Clip bounds are in document-layout columns and also constrain hit testing.
  std::int64_t clip_left { 0 };
  std::int64_t clip_right { 0 };
  // Structural boundaries locate anchors but do not displace nearby text hits.
  bool boundary_only { false };
  std::int64_t line_start { 0 };
  bool decoration { false };
  bool line_break { false };
  bool fill { false };
  bool word_start { false };
  bool word_end { false };
  // Inner-to-outer windows allow a code block inside a table to be revealed.
  std::vector<ScrollWindow> scrolls {};
};

struct LayoutData
{
  // Reading order, including table cells in row/cell order; not screen order.
  std::vector<LayoutGlyph> glyphs {};
  std::size_t rows { 1 };
  std::int64_t extent { 0 };
};

// A derived snapshot for one Document + initial control values + width/scrolls.
// Editing will supply a separate draft snapshot; this stage uses revision zero.
// No ncurses headers, terminal objects or business-module dependencies.
class DocumentLayout
{
public:
  DocumentLayout( const Document& document,
                  const TextService& text,
                  int columns,
                  std::span<const HorizontalScroll> scrolls );

  int columns() const;
  std::size_t rows() const;
  std::optional<LayoutPoint> locate( const DisplayPosition& position ) const;
  std::optional<LayoutHit> hit( LayoutPoint point ) const;
  DisplayPosition first() const;
  DisplayPosition last() const;
  DisplayPosition adjacent( const DisplayPosition& position, int direction ) const;
  DisplayPosition word( const DisplayPosition& position, WordMotion motion ) const;
  DisplayPosition on_row( LayoutPoint point ) const;
  DisplayPosition line_edge( const DisplayPosition& position, bool end ) const;
  DocumentRange unit( const DisplayPosition& position ) const;
  std::optional<HitTarget> target( const DisplayPosition& position ) const;
  std::optional<LayoutPoint> cursor_cell( const DisplayPosition& position ) const;
  std::vector<HorizontalScroll> reveal( const DisplayPosition& position ) const;
  std::vector<HorizontalScroll> pan( const DisplayPosition& position, int direction ) const;
  void append_frame( Frame& frame,
                     std::size_t first_row,
                     CellPoint origin,
                     ScreenSize viewport,
                     const DocumentText& content,
                     std::optional<DocumentRange> selection,
                     const DisplayPosition& cursor,
                     std::optional<HitTarget> focus = {} ) const;

private:
  struct ReadingPosition
  {
    std::size_t index { 0 };
    bool at_start { true };
  };
  ReadingPosition reading_position( const DisplayPosition& position ) const;
  std::size_t nearest_stop( LayoutPoint point ) const;
  const LayoutGlyph& cursor_glyph( const DisplayPosition& position ) const;
  DisplayPosition stop( std::size_t index ) const;

  int columns_;
  LayoutData data_;
  std::vector<std::size_t> stops_ {};
};

} // namespace ricochet::tui::detail
