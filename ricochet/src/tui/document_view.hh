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

#include "document_text.hh"
#include "layout.hh"

#include <chrono>

namespace ricochet::tui::detail {

enum class ViewMotion : std::uint8_t
{
  Left,
  Right,
  Up,
  Down,
  NextWord,
  PreviousWord,
  WordEnd,
  LineStart,
  LineEnd,
  DocumentStart,
  DocumentEnd,
  PageUp,
  PageDown,
  HalfPageUp,
  HalfPageDown,
  PanLeft,
  PanRight
};

enum class FocusResult : std::uint8_t
{
  Empty,
  Moved,
  Wrapped
};

// One tab's reading state. It owns the logical snapshot; screen rows are caches.
// Keyboard, mouse and host navigation all use the same layout/source mappings.
class DocumentView
{
public:
  DocumentView( Document document, ViewSnapshot snapshot, const TextService& text, ScreenSize viewport );

  const Document& document() const;
  const ViewSnapshot& snapshot() const;
  void resize( ScreenSize viewport );
  void move( ViewMotion motion );
  void press( CellPoint point, std::chrono::steady_clock::time_point now );
  // The session routes these only while dragging() owns mouse capture.
  void drag( CellPoint point );
  void release( CellPoint point );
  void stop_drag();
  bool dragging() const;
  bool tick( std::chrono::steady_clock::time_point now );
  void begin_selection();
  void cancel_selection();
  std::optional<std::string> selected_text() const;
  void wheel( int steps );
  bool locate( const DocumentPoint& point );
  std::optional<HitTarget> focused_target() const;
  // Browse/field-editor routing owns mode eligibility; this follows the shared order.
  FocusResult focus( bool backward );
  const Link* focused_link() const;
  void draw( Frame& frame, CellPoint origin ) const;

private:
  std::size_t last_top_row() const;
  void set_top_row( std::size_t row );
  void restore_viewport();
  void apply_scrolls( std::span<const HorizontalScroll> changes );
  void reveal_cursor();
  void finish_move( bool preserve_column );
  void move_rows( std::int64_t distance, bool page );
  void extend_selection();
  void drag_to_pointer();

  struct Drag
  {
    CellPoint press {};
    CellPoint pointer {};
    DocumentRange anchor {};
    bool selecting { false };
    std::chrono::steady_clock::time_point next_scroll {};
  };

  Document document_;
  ViewSnapshot snapshot_;
  const TextService& text_;
  DocumentText content_;
  ScreenSize viewport_;
  DocumentLayout layout_;
  std::size_t top_row_ { 0 };
  std::int64_t desired_column_ { 0 };
  bool follow_cursor_ { false };
  std::optional<Drag> drag_ {};
};

} // namespace ricochet::tui::detail
