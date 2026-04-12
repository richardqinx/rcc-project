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

#include "ricochet/parser/dom_tree.hh"

namespace ricochet::render {

struct RenderResult
{
  std::string text {};
  std::vector<std::string> links {};
  std::vector<std::string> inputs {};
};

class Renderer
{
public:
  Renderer() = default;
  ~Renderer() = default;

  Renderer( const Renderer& ) = delete;
  Renderer& operator=( const Renderer& ) = delete;
  Renderer( Renderer&& ) = delete;
  Renderer& operator=( Renderer&& ) = delete;

  /**
   * @brief Traverse the DOM tree and output the formatted result to the terminal using ANSI escape codes
   * @param node The root node of the DOM tree
   */
  RenderResult render( const parser::DomNode& node ) const;
};

} // namespace ricochet::render