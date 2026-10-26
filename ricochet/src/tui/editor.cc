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

#include "editor.hh"

#include <algorithm>
#include <limits>
#include <utility>

namespace ricochet::tui::detail {
namespace {

ScreenSize editor_size( ScreenSize viewport )
{ return { std::max( 1, viewport.rows ), std::max( 1, viewport.columns ) }; }

} // namespace

TextEditor::TextEditor( std::string value, EditorOptions options, const TextService& text, ScreenSize viewport )
  : options_( options ), text_( text ), viewport_( editor_size( viewport ) )
{
  state_.value = std::move( value );
  state_.cursor = state_.value.size();
  analyze();
  desired_column_ = carets_.back().column;
  reveal();
}

const std::string& TextEditor::value() const
{ return state_.value; }
std::uint64_t TextEditor::revision() const
{ return revision_; }
std::size_t TextEditor::cursor() const
{ return state_.cursor; }

ByteRange TextEditor::selection() const
{
  const auto anchor = state_.anchor.value_or( state_.cursor );
  return { std::min( anchor, state_.cursor ), std::max( anchor, state_.cursor ) };
}

std::optional<std::string> TextEditor::copy_selection()
{
  killing_ = false;
  prefix_.reset();
  const auto range = selection();
  if ( options_.kind == EditorKind::Password || range.start == range.end ) {
    return std::nullopt;
  }
  return state_.value.substr( range.start, range.end - range.start );
}

std::optional<std::chrono::steady_clock::time_point> TextEditor::prefix_deadline() const
{ return prefix_; }

std::string TextEditor::normalize( std::string_view value ) const
{
  std::string result;
  result.reserve( value.size() );
  for ( std::size_t index = 0; index < value.size(); ++index ) {
    const auto character = value[index];
    if ( character == '\r' ) {
      if ( index + 1 < value.size() && value[index + 1] == '\n' ) {
        ++index;
      }
      result += options_.kind == EditorKind::MultiLine ? '\n' : ' ';
    } else if ( character == '\n' && options_.kind != EditorKind::MultiLine ) {
      result += ' ';
    } else {
      result += character;
    }
  }
  return result;
}

std::size_t TextEditor::unit_index() const
{
  return static_cast<std::size_t>(
    std::ranges::lower_bound(
      analysis_.graphemes, state_.cursor, {}, []( const auto& glyph ) { return glyph.bytes.start; } )
    - analysis_.graphemes.begin() );
}

std::size_t TextEditor::boundary( std::size_t index ) const
{ return index == analysis_.graphemes.size() ? state_.value.size() : analysis_.graphemes[index].bytes.start; }

std::size_t TextEditor::line_start() const
{
  const auto newline
    = std::find_if( analysis_.graphemes.rbegin(), analysis_.graphemes.rend(), [this]( const auto& unit ) {
        return unit.kind == TextUnitKind::LineBreak && unit.bytes.end <= state_.cursor;
      } );
  return newline == analysis_.graphemes.rend() ? 0 : newline->bytes.end;
}

std::size_t TextEditor::line_end() const
{
  const auto newline = std::ranges::find_if( analysis_.graphemes, [this]( const auto& unit ) {
    return unit.kind == TextUnitKind::LineBreak && unit.bytes.start >= state_.cursor;
  } );
  return newline == analysis_.graphemes.end() ? state_.value.size() : newline->bytes.start;
}

void TextEditor::analyze()
{
  analysis_ = text_.analyze( state_.value, options_.kind == EditorKind::MultiLine, true );
  layout();
}

void TextEditor::layout()
{
  glyphs_.clear();
  carets_.assign( analysis_.graphemes.size() + 1, {} );
  Point point;
  for ( std::size_t index = 0; index < analysis_.graphemes.size(); ++index ) {
    const auto& unit = analysis_.graphemes[index];
    const bool password = options_.kind == EditorKind::Password;
    if ( unit.kind == TextUnitKind::LineBreak && !password ) {
      // A hard break after a full row stays on that row. Reuse the preceding
      // complete glyph's cell for its insertion cursor instead of wrapping twice.
      carets_[index] = point.column == viewport_.columns ? glyphs_.back().point : point;
      glyphs_.push_back( { point, 0 } );
      ++point.row;
      point.column = 0;
    } else {
      const auto width_at = [&]( std::int64_t column ) {
        const auto width = password                         ? 1
                           : unit.kind == TextUnitKind::Tab ? 8 - static_cast<int>( column % 8 )
                                                            : unit.columns;
        return options_.kind == EditorKind::MultiLine ? std::min( width, viewport_.columns ) : width;
      };
      int width = width_at( point.column );
      if ( options_.kind == EditorKind::MultiLine && point.column > 0
           && point.column + width > viewport_.columns ) {
        ++point.row;
        point.column = 0;
        width = width_at( 0 );
      }
      carets_[index] = point;
      glyphs_.push_back( { point, width } );
      point.column += width;
    }
    carets_[index + 1] = point;
  }
  // A full final soft row needs a real insertion cell after its final grapheme.
  if ( options_.kind == EditorKind::MultiLine && point.column == viewport_.columns ) {
    carets_.back() = { point.row + 1, 0 };
  }
}

void TextEditor::reveal()
{
  const auto point = carets_[unit_index()];
  const auto height = static_cast<std::size_t>( viewport_.rows );
  const auto rows = carets_.back().row + 1;
  top_row_ = std::min( top_row_, rows - std::min( rows, height ) );
  if ( point.row < top_row_ ) {
    top_row_ = point.row;
  } else if ( point.row - top_row_ >= height ) {
    top_row_ = point.row - height + 1;
  }
  if ( options_.kind == EditorKind::MultiLine ) {
    left_column_ = 0;
  } else {
    left_column_ = std::min( left_column_, point.column );
    left_column_ = std::max( left_column_, point.column - viewport_.columns + 1 );
  }
}

void TextEditor::resize( ScreenSize viewport )
{
  const auto next = editor_size( viewport );
  if ( next == viewport_ ) {
    return;
  }
  viewport_ = next;
  layout();
  desired_column_ = carets_[unit_index()].column;
  reveal();
}

std::size_t TextEditor::nearest( Point point ) const
{
  std::size_t chosen = 0;
  auto distance = std::pair { std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::int64_t>::max() };
  for ( std::size_t index = 0; index < carets_.size(); ++index ) {
    const auto& candidate = carets_[index];
    const auto delta = std::pair {
      candidate.row > point.row ? candidate.row - point.row : point.row - candidate.row,
      candidate.column > point.column ? candidate.column - point.column : point.column - candidate.column };
    if ( delta < distance ) {
      chosen = index;
      distance = delta;
    }
  }
  return boundary( chosen );
}

void TextEditor::move( std::size_t destination, bool extend, bool preserve_column )
{
  if ( extend || state_.selecting ) {
    state_.anchor = state_.anchor.value_or( state_.cursor );
  } else {
    state_.anchor.reset();
  }
  state_.cursor = destination;
  if ( !preserve_column ) {
    desired_column_ = carets_[unit_index()].column;
  }
  reveal();
}

void TextEditor::replace( ByteRange range, std::string_view replacement )
{
  auto next = state_.value;
  next.replace( range.start, range.end - range.start, replacement );
  if ( next != state_.value ) {
    past_.push_back( state_ );
    future_.clear();
    ++revision_;
  }
  state_.value = std::move( next );
  state_.cursor = range.start + replacement.size();
  state_.anchor.reset();
  state_.selecting = false;
  analyze();
  // Insertion/deletion can join surrounding graphemes. Snap to the next boundary
  // in the new text, never retain an offset inside a new combining/ZWJ cluster.
  state_.cursor = boundary( unit_index() );
  desired_column_ = carets_[unit_index()].column;
  reveal();
}

void TextEditor::insert( std::u32string_view input )
{
  killing_ = false;
  prefix_.reset();
  if ( input.empty() ) {
    return; // An empty paste must not delete an existing selection.
  }
  const auto value = normalize( text_.encode( input ) );
  replace( selection(), value );
}

void TextEditor::replace_value( std::string value )
{
  killing_ = false;
  prefix_.reset();
  replace( { 0, state_.value.size() }, value );
}

void TextEditor::kill( ByteRange range )
{
  if ( range.start == range.end ) {
    return; // Empty kills preserve both the buffer and the current kill chain.
  }
  auto removed = state_.value.substr( range.start, range.end - range.start );
  if ( !killing_ ) {
    killed_ = std::move( removed );
  } else if ( range.end <= state_.cursor ) {
    killed_.insert( 0, removed );
  } else {
    killed_ += removed;
  }
  replace( range, {} );
  killing_ = true;
}

void TextEditor::undo( bool redo )
{
  auto& source = redo ? future_ : past_;
  auto& destination = redo ? past_ : future_;
  if ( source.empty() ) {
    return;
  }
  destination.push_back( std::move( state_ ) );
  state_ = std::move( source.back() );
  source.pop_back();
  ++revision_;
  analyze();
  desired_column_ = carets_[unit_index()].column;
  reveal();
}

void TextEditor::execute( EditorCommand command, bool extend )
{
  prefix_.reset();
  const bool killing = command == EditorCommand::KillToStart || command == EditorCommand::KillToEnd
                       || command == EditorCommand::KillWord;
  killing_ = killing_ && killing;
  const auto range = selection();
  auto index = unit_index();
  switch ( command ) {
    case EditorCommand::Left:
      move( !extend && !state_.selecting && range.start != range.end ? range.start
                                                                     : boundary( index == 0 ? 0 : index - 1 ),
            extend,
            false );
      break;
    case EditorCommand::Right:
      move( !extend && !state_.selecting && range.start != range.end
              ? range.end
              : boundary( std::min( index + 1, analysis_.graphemes.size() ) ),
            extend,
            false );
      break;
    case EditorCommand::Up:
    case EditorCommand::Down: {
      if ( options_.kind != EditorKind::MultiLine ) {
        break;
      }
      auto row = carets_[index].row;
      row = command == EditorCommand::Up ? ( row == 0 ? 0 : row - 1 ) : std::min( row + 1, carets_.back().row );
      move( nearest( { row, desired_column_ } ), extend, true );
      break;
    }
    case EditorCommand::LineStart:
      move( line_start(), extend, false );
      break;
    case EditorCommand::LineEnd:
      move( line_end(), extend, false );
      break;
    case EditorCommand::PreviousWord: {
      const auto word = std::find_if( analysis_.words.rbegin(), analysis_.words.rend(), [this]( const auto& item ) {
        return item.start < state_.cursor;
      } );
      move( word == analysis_.words.rend() ? 0 : word->start, extend, false );
      break;
    }
    case EditorCommand::NextWord: {
      const auto word
        = std::ranges::find_if( analysis_.words, [this]( const auto& item ) { return item.end > state_.cursor; } );
      move( word == analysis_.words.end() ? state_.value.size() : word->end, extend, false );
      break;
    }
    case EditorCommand::Backspace:
      replace( range.start != range.end ? range
                                        : ByteRange { boundary( index == 0 ? 0 : index - 1 ), state_.cursor },
               {} );
      break;
    case EditorCommand::Delete:
      replace( range.start != range.end
                 ? range
                 : ByteRange { state_.cursor, boundary( std::min( index + 1, analysis_.graphemes.size() ) ) },
               {} );
      break;
    case EditorCommand::KillToStart:
      kill( range.start != range.end ? range : ByteRange { line_start(), state_.cursor } );
      break;
    case EditorCommand::KillToEnd:
      kill( range.start != range.end ? range : ByteRange { state_.cursor, line_end() } );
      break;
    case EditorCommand::KillWord:
      while ( index > 0 && analysis_.graphemes[index - 1].word == WordKind::Space ) {
        --index;
      }
      while ( index > 0 && analysis_.graphemes[index - 1].word != WordKind::Space ) {
        --index;
      }
      kill( range.start != range.end ? range : ByteRange { boundary( index ), state_.cursor } );
      break;
    case EditorCommand::Yank:
      if ( !killed_.empty() ) {
        replace( range, killed_ );
      }
      break;
    case EditorCommand::BeginSelection:
      state_.anchor = state_.cursor;
      state_.selecting = true;
      break;
    case EditorCommand::SelectAll:
      state_.anchor = 0;
      state_.cursor = state_.value.size();
      state_.selecting = false;
      desired_column_ = carets_.back().column;
      reveal();
      break;
    case EditorCommand::CancelSelection:
      state_.anchor.reset();
      state_.selecting = false;
      break;
    case EditorCommand::Clear:
      replace( { 0, state_.value.size() }, {} );
      break;
    case EditorCommand::Newline:
      if ( options_.kind == EditorKind::MultiLine ) {
        replace( range, "\n" );
      }
      break;
    case EditorCommand::Undo:
      undo( false );
      break;
    case EditorCommand::Redo:
      undo( true );
      break;
  }
}

void TextEditor::place( CellPoint point, bool extend )
{
  prefix_.reset();
  killing_ = false;
  // The owner routes body clicks/dragging; outside drag coordinates clamp to this viewport.
  const Point location { top_row_ + static_cast<std::size_t>( std::clamp( point.row, 0, viewport_.rows - 1 ) ),
                         left_column_ + std::clamp( point.column, 0, viewport_.columns - 1 ) };
  state_.selecting = false;
  move( nearest( location ), extend, false );
}

EditorIntent TextEditor::key( const Key& input, std::chrono::steady_clock::time_point now )
{
  if ( prefix_ ) {
    prefix_.reset();
    if ( input.code == KeyCode::Character && input.modifiers == Modifiers { .control = true }
         && input.character == U'u' ) {
      execute( EditorCommand::Undo );
    }
    return EditorIntent::Consumed; // Mismatches never execute a second command.
  }
  const bool kill_key = input.code == KeyCode::Character && input.modifiers == Modifiers { .control = true }
                        && ( input.character == U'u' || input.character == U'k' || input.character == U'w' );
  killing_ = killing_ && kill_key;
  if ( input.code == KeyCode::Character && input.modifiers == Modifiers { .control = true } ) {
    switch ( input.character ) {
      case U'b':
        execute( EditorCommand::Left );
        break;
      case U'f':
        execute( EditorCommand::Right );
        break;
      case U'p':
      case U'n':
        if ( options_.kind == EditorKind::SingleLine && options_.history ) {
          return input.character == U'p' ? EditorIntent::HistoryPrevious : EditorIntent::HistoryNext;
        }
        execute( input.character == U'p' ? EditorCommand::Up : EditorCommand::Down );
        break;
      case U'a':
        execute( EditorCommand::LineStart );
        break;
      case U'e':
        execute( EditorCommand::LineEnd );
        break;
      case U'd':
        execute( EditorCommand::Delete );
        break;
      case U'u':
        execute( EditorCommand::KillToStart );
        break;
      case U'k':
        execute( EditorCommand::KillToEnd );
        break;
      case U'w':
        execute( EditorCommand::KillWord );
        break;
      case U'y':
        execute( EditorCommand::Yank );
        break;
      case U'_':
        execute( EditorCommand::Undo );
        break;
      case U'x':
        prefix_ = now + std::chrono::seconds( 1 );
        break;
      case U'l':
        return EditorIntent::Redraw;
      case U'c':
        return EditorIntent::Cancel;
      default:
        return EditorIntent::Unhandled;
    }
    return EditorIntent::Consumed;
  }
  if ( input.code == KeyCode::Character && input.modifiers == Modifiers { .alt = true } ) {
    switch ( input.character ) {
      case U'b':
        execute( EditorCommand::PreviousWord );
        return EditorIntent::Consumed;
      case U'f':
        execute( EditorCommand::NextWord );
        return EditorIntent::Consumed;
      default:
        return EditorIntent::Unhandled;
    }
  }
  if ( input.modifiers != Modifiers {} && input.modifiers != Modifiers { .shift = true } ) {
    return EditorIntent::Unhandled;
  }
  switch ( input.code ) {
    case KeyCode::Left:
      execute( EditorCommand::Left, input.modifiers.shift );
      break;
    case KeyCode::Right:
      execute( EditorCommand::Right, input.modifiers.shift );
      break;
    case KeyCode::Up:
    case KeyCode::Down:
      if ( options_.kind == EditorKind::SingleLine && options_.history && !input.modifiers.shift ) {
        return input.code == KeyCode::Up ? EditorIntent::HistoryPrevious : EditorIntent::HistoryNext;
      }
      execute( input.code == KeyCode::Up ? EditorCommand::Up : EditorCommand::Down, input.modifiers.shift );
      break;
    case KeyCode::Home:
      execute( EditorCommand::LineStart, input.modifiers.shift );
      break;
    case KeyCode::End:
      execute( EditorCommand::LineEnd, input.modifiers.shift );
      break;
    case KeyCode::Tab:
      return input.modifiers.shift ? EditorIntent::BackTab : EditorIntent::Tab;
    default:
      if ( input.modifiers.shift ) {
        return EditorIntent::Unhandled;
      }
      switch ( input.code ) {
        case KeyCode::Backspace:
          execute( EditorCommand::Backspace );
          break;
        case KeyCode::Delete:
          execute( EditorCommand::Delete );
          break;
        case KeyCode::Enter:
          return EditorIntent::Enter;
        case KeyCode::Escape:
          return EditorIntent::Escape;
        case KeyCode::Function:
          return input.function == 10 ? EditorIntent::Menu : EditorIntent::Unhandled;
        default:
          return EditorIntent::Unhandled;
      }
      break;
  }
  return EditorIntent::Consumed;
}

EditorIntent TextEditor::handle( const InputEvent& event, std::chrono::steady_clock::time_point now )
{
  if ( const auto* tick = std::get_if<Tick>( &event ) ) {
    now = tick->now;
  }
  if ( prefix_ && now >= *prefix_ ) {
    prefix_.reset();
  }
  if ( const auto* input = std::get_if<Key>( &event ) ) {
    return key( *input, now );
  }
  if ( const auto* input = std::get_if<TextInput>( &event ) ) {
    killing_ = false;
    if ( prefix_ ) {
      prefix_.reset();
    } else {
      insert( std::u32string_view( &input->character, 1 ) );
    }
    return EditorIntent::Consumed;
  }
  if ( const auto* paste = std::get_if<Paste>( &event ) ) {
    insert( paste->text );
    return EditorIntent::Consumed;
  }
  if ( std::holds_alternative<Tick>( event ) ) {
    return EditorIntent::Consumed;
  }
  return EditorIntent::Unhandled;
}

void TextEditor::draw( Frame& frame, CellPoint origin ) const
{
  // Fields can overlay an already painted document. Clear their entire region so
  // shortening the value cannot expose the old field text beneath the editor.
  for ( int row = 0; row < viewport_.rows; ++row ) {
    for ( int column = 0; column < viewport_.columns; ++column ) {
      frame.glyphs.push_back( { .position = { origin.row + row, origin.column + column }, .text = L" " } );
    }
  }
  const auto selected = selection();
  const auto cursor_point = carets_[unit_index()];
  for ( std::size_t index = 0; index < glyphs_.size(); ++index ) {
    const auto& glyph = glyphs_[index];
    if ( glyph.point.row < top_row_ || glyph.point.row - top_row_ >= static_cast<std::size_t>( viewport_.rows ) ) {
      continue;
    }
    const auto& unit = analysis_.graphemes[index];
    const bool password = options_.kind == EditorKind::Password;
    const bool selected_unit = unit.bytes.start < selected.end && selected.start < unit.bytes.end;
    const int row = origin.row + static_cast<int>( glyph.point.row - top_row_ );
    const auto column = glyph.point.column - left_column_;
    const bool at_cursor = cursor_point.row == glyph.point.row && cursor_point.column == glyph.point.column;
    if ( unit.kind == TextUnitKind::Tab && !password ) {
      for ( auto cell = std::max<std::int64_t>( 0, column );
            cell < std::min<std::int64_t>( viewport_.columns, column + glyph.columns );
            ++cell ) {
        frame.glyphs.push_back( { .position = { row, origin.column + static_cast<int>( cell ) },
                                  .text = L" ",
                                  .selected = selected_unit,
                                  .cursor = at_cursor && cell == column } );
      }
    } else {
      const bool newline = unit.kind == TextUnitKind::LineBreak;
      const int width = newline ? 1 : glyph.columns;
      if ( column < 0 || column + width > viewport_.columns || ( newline && !selected_unit ) ) {
        continue;
      }
      const auto display = password ? L"*" : newline ? L" " : glyph.columns < unit.columns ? L"?" : unit.display;
      frame.glyphs.push_back( { .position = { row, origin.column + static_cast<int>( column ) },
                                .width = width,
                                .text = display,
                                .selected = selected_unit,
                                .cursor = at_cursor } );
    }
  }
  frame.cursor = CellPoint { origin.row + static_cast<int>( cursor_point.row - top_row_ ),
                             origin.column + static_cast<int>( cursor_point.column - left_column_ ) };
}

} // namespace ricochet::tui::detail
