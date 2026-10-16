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
#include <optional>
#include <string>
#include <vector>

namespace ricochet::tui {

enum class SelectionDirection : std::uint8_t
{
  Forward,
  Backward
};

struct SelectionSnapshot
{
  // The normalized half-open range also covers any controls between its endpoints.
  DocumentRange range {};
  SelectionDirection direction { SelectionDirection::Forward };
};

struct HorizontalScroll
{
  BlockId block {};
  std::size_t columns { 0 };
};

struct SearchSnapshot
{
  std::string query {};
  std::optional<DocumentRange> current_match {};
};

// All positions and ranges belong to scope and draft_revision.
// Editing sessions and control values (including passwords) are not exported here.
struct ViewSnapshot
{
  DocumentScope scope {};
  DraftRevision draft_revision {};
  DisplayPosition cursor {};
  DisplayPosition viewport_anchor {};
  std::optional<SelectionSnapshot> selection {};
  std::vector<HorizontalScroll> horizontal_scroll {};
  SearchSnapshot search {};
};

} // namespace ricochet::tui
