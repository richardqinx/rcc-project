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
  void append_frame( Frame& frame, std::size_t first_row, CellPoint origin, ScreenSize viewport ) const;

private:
  int columns_;
  LayoutData data_;
};

} // namespace ricochet::tui::detail
