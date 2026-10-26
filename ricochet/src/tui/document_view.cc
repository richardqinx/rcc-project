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

#include "document_view.hh"

#include <algorithm>
#include <utility>

namespace ricochet::tui::detail {
namespace {

ScreenSize body_size( ScreenSize viewport )
{ return { std::max( 1, viewport.rows ), std::max( 1, viewport.columns ) }; }

std::size_t shifted_row( std::size_t row, std::int64_t distance, std::size_t last )
{
  // Clamp the displacement before addition; restored relative offsets can be signed.
  const auto origin = static_cast<std::int64_t>( row );
  return static_cast<std::size_t>( origin
                                   + std::clamp( distance, -origin, static_cast<std::int64_t>( last ) - origin ) );
}

} // namespace

DocumentView::DocumentView( Document document, ViewSnapshot snapshot, const TextService& text, ScreenSize viewport )
  : document_( std::move( document ) )
  , snapshot_( std::move( snapshot ) )
  , text_( text )
  , content_( document_, text_ )
  , viewport_( body_size( viewport ) )
  , layout_( document_, text_, viewport_.columns, snapshot_.horizontal_scroll )
{
  // A history snapshot can name a removed boundary. The new document starts at
  // its first reading unit; it never manufactures a byte offset into that text.
  if ( std::holds_alternative<DocumentStart>( snapshot_.cursor.point ) || !layout_.locate( snapshot_.cursor ) ) {
    snapshot_.cursor = layout_.first();
  }
  desired_column_ = layout_.locate( snapshot_.cursor ).value().column;
  restore_viewport();
  if ( snapshot_.selection ) {
    const auto& selection = *snapshot_.selection;
    // History endpoints must still be layout boundaries in this exact document/draft.
    if ( layout_.locate( { selection.anchor.start } ) && layout_.locate( { selection.anchor.end } )
         && layout_.locate( { selection.active.start } ) && layout_.locate( { selection.active.end } ) ) {
      snapshot_.selection = content_.select( selection.anchor, selection.active );
    } else {
      snapshot_.selection.reset();
    }
  }
}

const Document& DocumentView::document() const
{ return document_; }
const ViewSnapshot& DocumentView::snapshot() const
{ return snapshot_; }

std::size_t DocumentView::last_top_row() const
{ return layout_.rows() - std::min( layout_.rows(), static_cast<std::size_t>( viewport_.rows ) ); }

void DocumentView::set_top_row( std::size_t row )
{
  top_row_ = std::min( row, last_top_row() );
  snapshot_.viewport_anchor = layout_.on_row( { top_row_, 0 } );
  const auto anchor = layout_.locate( snapshot_.viewport_anchor ).value();
  snapshot_.viewport_row_offset = static_cast<std::int64_t>( top_row_ ) - static_cast<std::int64_t>( anchor.row );
}

void DocumentView::restore_viewport()
{
  auto anchor = layout_.locate( snapshot_.viewport_anchor );
  if ( !anchor ) {
    snapshot_.viewport_anchor = layout_.first();
    snapshot_.viewport_row_offset = 0;
    anchor = layout_.locate( snapshot_.viewport_anchor );
  }
  set_top_row( shifted_row( anchor->row, snapshot_.viewport_row_offset, last_top_row() ) );
}

void DocumentView::resize( ScreenSize viewport )
{
  const auto next = body_size( viewport );
  if ( next == viewport_ ) {
    return;
  }
  stop_drag();
  const bool reflow = next.columns != viewport_.columns;
  viewport_ = next;
  if ( reflow ) {
    layout_ = DocumentLayout( document_, text_, viewport_.columns, snapshot_.horizontal_scroll );
    desired_column_ = layout_.locate( snapshot_.cursor ).value().column;
  }
  restore_viewport();
  // A wheel/pan action deliberately allows the cursor to stay outside the view.
  if ( follow_cursor_ ) {
    reveal_cursor();
  }
}

void DocumentView::apply_scrolls( std::span<const HorizontalScroll> changes )
{
  if ( changes.empty() ) {
    return;
  }
  for ( const auto& change : changes ) {
    const auto found = std::ranges::find( snapshot_.horizontal_scroll, change.block, &HorizontalScroll::block );
    if ( found == snapshot_.horizontal_scroll.end() ) {
      snapshot_.horizontal_scroll.push_back( change );
    } else {
      *found = change;
    }
  }
  layout_ = DocumentLayout( document_, text_, viewport_.columns, snapshot_.horizontal_scroll );
  restore_viewport();
}

void DocumentView::reveal_cursor()
{
  apply_scrolls( layout_.reveal( snapshot_.cursor ) );
  const auto point = layout_.locate( snapshot_.cursor ).value();
  if ( point.row < top_row_ ) {
    set_top_row( point.row );
  } else if ( point.row - top_row_ >= static_cast<std::size_t>( viewport_.rows ) ) {
    set_top_row( point.row - static_cast<std::size_t>( viewport_.rows - 1 ) );
  }
}

void DocumentView::finish_move( bool preserve_column )
{
  follow_cursor_ = true;
  extend_selection();
  reveal_cursor();
  if ( !preserve_column ) {
    desired_column_ = layout_.locate( snapshot_.cursor ).value().column;
  }
}

void DocumentView::move_rows( std::int64_t distance, bool page )
{
  const auto origin = layout_.locate( snapshot_.cursor ).value();
  const auto row = shifted_row( origin.row, distance, layout_.rows() - 1 );
  snapshot_.cursor = layout_.on_row( { row, desired_column_ } );
  if ( page ) {
    // Shift both by the same amount before revealing, retaining screen-relative
    // cursor position wherever document boundaries permit it.
    set_top_row( shifted_row( top_row_, distance, last_top_row() ) );
  }
  finish_move( true );
}

void DocumentView::move( ViewMotion motion )
{
  stop_drag();
  switch ( motion ) {
    case ViewMotion::Left:
      snapshot_.cursor = layout_.adjacent( snapshot_.cursor, -1 );
      break;
    case ViewMotion::Right:
      snapshot_.cursor = layout_.adjacent( snapshot_.cursor, 1 );
      break;
    case ViewMotion::NextWord:
      snapshot_.cursor = layout_.word( snapshot_.cursor, WordMotion::Next );
      break;
    case ViewMotion::PreviousWord:
      snapshot_.cursor = layout_.word( snapshot_.cursor, WordMotion::Previous );
      break;
    case ViewMotion::WordEnd:
      snapshot_.cursor = layout_.word( snapshot_.cursor, WordMotion::End );
      break;
    case ViewMotion::LineStart:
      snapshot_.cursor = layout_.line_edge( snapshot_.cursor, false );
      break;
    case ViewMotion::LineEnd:
      snapshot_.cursor = layout_.line_edge( snapshot_.cursor, true );
      break;
    case ViewMotion::DocumentStart:
      snapshot_.cursor = layout_.first();
      break;
    case ViewMotion::DocumentEnd:
      snapshot_.cursor = layout_.last();
      break;
    case ViewMotion::Up:
      move_rows( -1, false );
      return;
    case ViewMotion::Down:
      move_rows( 1, false );
      return;
    case ViewMotion::PageUp:
      move_rows( -std::max( 1, viewport_.rows - 1 ), true );
      return;
    case ViewMotion::PageDown:
      move_rows( std::max( 1, viewport_.rows - 1 ), true );
      return;
    case ViewMotion::HalfPageUp:
      move_rows( -std::max( 1, viewport_.rows / 2 ), true );
      return;
    case ViewMotion::HalfPageDown:
      move_rows( std::max( 1, viewport_.rows / 2 ), true );
      return;
    case ViewMotion::PanLeft:
    case ViewMotion::PanRight: {
      const auto changes = layout_.pan( snapshot_.cursor, motion == ViewMotion::PanLeft ? -1 : 1 );
      if ( changes.empty() ) {
        return;
      }
      const auto old_column = layout_.locate( snapshot_.cursor ).value().column;
      apply_scrolls( changes );
      // Panning changes screen coordinates, not the desired logical column for
      // the next vertical move (including one remembered across a short line).
      desired_column_ += layout_.locate( snapshot_.cursor ).value().column - old_column;
      follow_cursor_ = false;
      return;
    }
  }
  finish_move( false );
}

void DocumentView::begin_selection()
{
  stop_drag();
  const auto unit = layout_.unit( snapshot_.cursor );
  snapshot_.selection = content_.select( unit, unit );
}

void DocumentView::cancel_selection()
{
  stop_drag();
  snapshot_.selection.reset();
}

void DocumentView::extend_selection()
{
  if ( snapshot_.selection ) {
    snapshot_.selection = content_.select( snapshot_.selection->anchor, layout_.unit( snapshot_.cursor ) );
  }
}

std::optional<std::string> DocumentView::selected_text() const
{
  if ( !snapshot_.selection ) {
    return std::nullopt;
  }
  return content_.extract( snapshot_.selection->range );
}

void DocumentView::press( CellPoint point, std::chrono::steady_clock::time_point now )
{
  const auto hit = layout_.hit( { top_row_ + static_cast<std::size_t>( point.row ), point.column } );
  // A completely clipped wide glyph has no visible cell to hit.
  if ( !hit ) {
    return;
  }
  snapshot_.cursor = hit->position;
  cancel_selection();
  desired_column_ = layout_.locate( snapshot_.cursor ).value().column;
  follow_cursor_ = false;
  drag_ = Drag { point, point, layout_.unit( snapshot_.cursor ), false, now + std::chrono::milliseconds( 100 ) };
}

void DocumentView::drag_to_pointer()
{
  const auto row = std::clamp( drag_->pointer.row, 0, viewport_.rows - 1 );
  const auto column = std::clamp( drag_->pointer.column, 0, viewport_.columns - 1 );
  const auto hit = layout_.hit( { top_row_ + static_cast<std::size_t>( row ), column } );
  if ( !hit ) {
    return;
  }
  snapshot_.cursor = hit->position;
  snapshot_.selection = content_.select( drag_->anchor, layout_.unit( snapshot_.cursor ) );
  desired_column_ = layout_.locate( snapshot_.cursor ).value().column;
  follow_cursor_ = false;
}

void DocumentView::drag( CellPoint point )
{
  drag_->pointer = point;
  // One cell is the terminal's movement threshold; wide halves still share a unit.
  drag_->selecting = drag_->selecting || point.row != drag_->press.row || point.column != drag_->press.column;
  if ( drag_->selecting ) {
    drag_to_pointer();
  }
}

void DocumentView::release( CellPoint point )
{
  drag( point );
  stop_drag();
}

void DocumentView::stop_drag()
{ drag_.reset(); }

bool DocumentView::dragging() const
{ return drag_.has_value(); }

bool DocumentView::tick( std::chrono::steady_clock::time_point now )
{
  if ( !drag_ || !drag_->selecting || now < drag_->next_scroll ) {
    return false;
  }
  drag_->next_scroll = now + std::chrono::milliseconds( 100 );
  // With a one-row body both edges coincide; leaving it above/below disambiguates.
  const int direction = drag_->pointer.row < 0                     ? -1
                        : drag_->pointer.row >= viewport_.rows     ? 1
                        : viewport_.rows == 1                      ? 0
                        : drag_->pointer.row == 0                  ? -1
                        : drag_->pointer.row == viewport_.rows - 1 ? 1
                                                                   : 0;
  const auto next = shifted_row( top_row_, direction, last_top_row() );
  if ( next == top_row_ ) {
    return false;
  }
  set_top_row( next );
  drag_to_pointer();
  return true;
}

void DocumentView::wheel( int steps )
{
  set_top_row( shifted_row( top_row_, static_cast<std::int64_t>( steps ) * 3, last_top_row() ) );
  follow_cursor_ = false;
  if ( drag_ && drag_->selecting ) {
    drag_to_pointer();
  }
}

bool DocumentView::locate( const DocumentPoint& point )
{
  const DisplayPosition requested { point, WrapAffinity::NextLine };
  const auto location = layout_.locate( requested );
  // Host jumps can refer to absent anchors, removed leaves or non-grapheme offsets.
  if ( !location ) {
    return false;
  }
  snapshot_.cursor = requested;
  if ( std::holds_alternative<DocumentStart>( point ) ) {
    snapshot_.cursor = layout_.first();
  } else if ( const auto* object = std::get_if<ObjectPosition>( &point );
              object && std::holds_alternative<BlockId>( object->object ) ) {
    snapshot_.cursor = layout_.on_row( *location );
  }
  cancel_selection();
  finish_move( false );
  return true;
}

std::optional<HitTarget> DocumentView::focused_target() const
{ return layout_.target( snapshot_.cursor ); }

FocusResult DocumentView::focus( bool backward )
{
  const auto next = layout_.focus( snapshot_.cursor, backward );
  if ( !next ) {
    return FocusResult::Empty;
  }
  stop_drag();
  snapshot_.cursor = next->position;
  finish_move( false );
  return next->wrapped ? FocusResult::Wrapped : FocusResult::Moved;
}

const Link* DocumentView::focused_link() const
{
  const auto target = focused_target();
  if ( !target || !std::holds_alternative<LinkId>( *target ) ) {
    return nullptr;
  }
  // Layout link targets originate from this same immutable document.
  return &*std::ranges::find( document_.data().links, std::get<LinkId>( *target ), &Link::id );
}

void DocumentView::draw( Frame& frame, CellPoint origin ) const
{
  const auto selection = snapshot_.selection ? std::optional( snapshot_.selection->range ) : std::nullopt;
  layout_.append_frame(
    frame, top_row_, origin, viewport_, content_, selection, snapshot_.cursor, focused_target() );
  const auto cursor = layout_.cursor_cell( snapshot_.cursor );
  if ( cursor && cursor->row >= top_row_ && cursor->row - top_row_ < static_cast<std::size_t>( viewport_.rows )
       && cursor->column >= 0 && cursor->column < viewport_.columns ) {
    frame.cursor = CellPoint { origin.row + static_cast<int>( cursor->row - top_row_ ),
                               origin.column + static_cast<int>( cursor->column ) };
  }
}

} // namespace ricochet::tui::detail
