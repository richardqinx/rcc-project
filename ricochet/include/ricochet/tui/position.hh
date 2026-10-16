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

#include "ricochet/tui/identity.hh"

#include <cstddef>
#include <cstdint>
#include <variant>

namespace ricochet::tui {

// UTF-8 byte ranges are half-open. Text operations use grapheme boundaries.
struct ByteRange
{
  std::size_t start { 0 };
  std::size_t end { 0 };

  bool operator==( const ByteRange& ) const = default;
};

struct TextPosition
{
  BlockId block {};
  std::size_t byte_offset { 0 };

  bool operator==( const TextPosition& ) const = default;
};

struct ControlTextPosition
{
  ControlId control {};
  DraftRevision revision {};
  // Offset in display text; password offsets refer only to its mask.
  std::size_t byte_offset { 0 };

  bool operator==( const ControlTextPosition& ) const = default;
};

enum class BoundarySide : std::uint8_t
{
  Before,
  After
};

struct ObjectPosition
{
  std::variant<BlockId, ControlId> object {};
  BoundarySide side { BoundarySide::Before };

  bool operator==( const ObjectPosition& ) const = default;
};

struct DocumentStart
{ bool operator==( const DocumentStart& ) const = default; };

// Local to a document snapshot. Stored anchors cannot depend on mutable drafts.
using ContentPoint = std::variant<DocumentStart, TextPosition, ObjectPosition>;
using DocumentPoint = std::variant<DocumentStart, TextPosition, ControlTextPosition, ObjectPosition>;

struct LogicalPosition
{
  DocumentScope scope {};
  DocumentPoint point {};

  bool operator==( const LogicalPosition& ) const = default;
};

// Endpoints follow reading order, never the numeric order of their IDs.
struct DocumentRange
{
  DocumentPoint start {};
  DocumentPoint end {};

  bool operator==( const DocumentRange& ) const = default;
};

struct TextRange
{
  BlockId block {};
  ByteRange bytes {};

  bool operator==( const TextRange& ) const = default;
};

enum class WrapAffinity : std::uint8_t
{
  PreviousLine,
  NextLine
};

struct DisplayPosition
{
  DocumentPoint point {};
  WrapAffinity affinity { WrapAffinity::NextLine };

  bool operator==( const DisplayPosition& ) const = default;
};

} // namespace ricochet::tui
