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
#include "text.hh"

#include <chrono>
#include <optional>

namespace ricochet::tui::detail {

enum class EditorKind : std::uint8_t
{
  SingleLine,
  MultiLine,
  Password
};

struct EditorOptions
{
  EditorKind kind { EditorKind::SingleLine };
  // The owner supplies history; password and multiline editors never request it.
  bool history { false };
};

enum class EditorCommand : std::uint8_t
{
  Left,
  Right,
  Up,
  Down,
  LineStart,
  LineEnd,
  PreviousWord,
  NextWord,
  Backspace,
  Delete,
  KillToStart,
  KillToEnd,
  KillWord,
  Yank,
  BeginSelection,
  SelectAll,
  CancelSelection,
  Clear,
  Newline,
  Undo,
  Redo
};

// Enter/Tab/Escape are context decisions, never implicit submit/finish operations.
// The owner may translate Enter into Newline for a multiline field.
// Unhandled keys stay with the owning editor context, never fall through to Browse.
enum class EditorIntent : std::uint8_t
{
  Consumed,
  Unhandled,
  Enter,
  Tab,
  BackTab,
  Escape,
  Cancel,
  Menu,
  Redraw,
  HistoryPrevious,
  HistoryNext
};

// One editing session. Owners recreate it when changing fields; its text, undo
// history and kill buffer are never shared with another editor or a view snapshot.
class TextEditor
{
public:
  TextEditor( std::string value, EditorOptions options, const TextService& text, ScreenSize viewport );
  TextEditor( const TextEditor& ) = delete;
  TextEditor& operator=( const TextEditor& ) = delete;
  TextEditor( TextEditor&& ) = delete;
  TextEditor& operator=( TextEditor&& ) = delete;
  ~TextEditor() = default;

  // Business value for the owning field; rendering and copying use the methods below.
  const std::string& value() const;
  std::uint64_t revision() const;
  std::size_t cursor() const;
  ByteRange selection() const;
  std::optional<std::string> copy_selection();
  std::optional<std::chrono::steady_clock::time_point> prefix_deadline() const;

  void resize( ScreenSize viewport );
  void insert( std::u32string_view input );
  // Host history replacement is one undoable edit, not a new editing session.
  void replace_value( std::string value );
  void execute( EditorCommand command, bool extend = false );
  EditorIntent handle( const InputEvent& event,
                       std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now() );
  // Region-local mouse positioning, with extend=true while selecting/dragging.
  void place( CellPoint point, bool extend );
  void draw( Frame& frame, CellPoint origin ) const;

private:
  struct State
  {
    std::string value {};
    std::size_t cursor { 0 };
    std::optional<std::size_t> anchor {};
    bool selecting { false };
  };
  struct Point
  {
    std::size_t row { 0 };
    std::int64_t column { 0 };
  };
  struct Glyph
  {
    Point point {};
    int columns { 0 };
  };

  std::string normalize( std::string_view value ) const;
  std::size_t unit_index() const;
  std::size_t boundary( std::size_t index ) const;
  std::size_t line_start() const;
  std::size_t line_end() const;
  std::size_t nearest( Point point ) const;
  void analyze();
  void layout();
  void reveal();
  void move( std::size_t destination, bool extend, bool preserve_column );
  void replace( ByteRange range, std::string_view replacement );
  void kill( ByteRange range );
  void undo( bool redo );
  EditorIntent key( const Key& key, std::chrono::steady_clock::time_point now );

  EditorOptions options_;
  const TextService& text_;
  ScreenSize viewport_;
  State state_ {};
  TextAnalysis analysis_ {};
  std::vector<Glyph> glyphs_ {};
  std::vector<Point> carets_ {};
  std::size_t top_row_ { 0 };
  std::int64_t left_column_ { 0 };
  std::int64_t desired_column_ { 0 };
  std::vector<State> past_ {};
  std::vector<State> future_ {};
  std::string killed_ {};
  bool killing_ { false };
  std::uint64_t revision_ { 0 };
  std::optional<std::chrono::steady_clock::time_point> prefix_ {};
};

} // namespace ricochet::tui::detail
