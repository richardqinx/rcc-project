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

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace ricochet::tui {

struct ScreenSize
{
  int rows { 0 };
  int columns { 0 };
  bool operator==( const ScreenSize& ) const = default;
};

struct CellPoint
{
  int row { 0 };
  int column { 0 };
};

struct Modifiers
{
  bool shift { false };
  bool control { false };
  bool alt { false };
  bool operator==( const Modifiers& ) const = default;
};

enum class KeyCode : std::uint8_t
{
  Character,
  Escape,
  Enter,
  Tab,
  Backspace,
  Delete,
  Insert,
  Left,
  Right,
  Up,
  Down,
  Home,
  End,
  PageUp,
  PageDown,
  Function,
  Unsupported
};

struct Key
{
  KeyCode code { KeyCode::Unsupported };
  Modifiers modifiers {};
  char32_t character { 0 };
  unsigned int function { 0 };
};

struct TextInput
{ char32_t character { 0 }; };

struct Paste
{ std::u32string text {}; };

struct MouseButtons
{
  bool left { false };
  bool middle { false };
  bool right { false };
};

struct Mouse
{
  // Screen coordinates; the view router converts these to region-local coordinates.
  CellPoint position {};
  Modifiers modifiers {};
  MouseButtons pressed {};
  MouseButtons released {};
  bool motion { false };
  int wheel_steps { 0 }; // Positive scrolls down.
};

struct Resize
{ ScreenSize size {}; };

struct Tick
{ std::chrono::steady_clock::time_point now {}; };

enum class TerminalCloseReason : std::uint8_t
{
  Disconnected,
  TerminationSignal
};

struct TerminalClosed
{ TerminalCloseReason reason { TerminalCloseReason::Disconnected }; };

using InputEvent = std::variant<Key, TextInput, Paste, Mouse, Resize, Tick, TerminalClosed>;

} // namespace ricochet::tui
