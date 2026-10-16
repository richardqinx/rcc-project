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
#include "ricochet/tui/input.hh"

#include <optional>
#include <string>
#include <vector>

namespace ricochet::tui::detail {

// Layout supplies one printable spacing character and its combining marks per
// glyph, with a measured width. Raw document text never goes directly to curses.
struct PaintGlyph
{
  CellPoint position {};
  int width { 1 };
  std::wstring text {};
  TextStyle style {};
};

struct Frame
{
  std::vector<PaintGlyph> glyphs {};
  std::optional<CellPoint> cursor {};
};

} // namespace ricochet::tui::detail
