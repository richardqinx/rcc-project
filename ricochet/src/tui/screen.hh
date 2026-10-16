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
#include "ricochet/tui/session.hh"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

namespace ricochet::tui::detail {

struct InputBatch
{
  std::vector<InputEvent> events {};
  bool consumed { false };
};

class Screen
{
public:
  Screen( std::FILE& input, std::FILE& output, TerminalOptions options );
  ~Screen();
  Screen( const Screen& ) = delete;
  Screen& operator=( const Screen& ) = delete;

  ScreenSize size() const;
  static std::size_t glyph_capacity();
  TerminalCapabilities capabilities() const;
  InputBatch read_events( std::size_t limit );
  // Wait only polls descriptors; wgetch remains the sole terminal reader.
  std::optional<TerminalClosed> wait( int wake_fd, std::chrono::milliseconds max_wait );
  void draw( const Frame& frame );

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ricochet::tui::detail
