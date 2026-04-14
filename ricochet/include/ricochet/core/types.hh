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

#include "ricochet/render/renderer.hh"
#include <cstdint>
#include <string>
#include <vector>

namespace ricochet::core {

struct PageData
{
  std::vector<std::string> lines;
  std::vector<std::string> links;
  std::vector<render::InputField> inputs;
  std::string title;
  std::vector<std::string> original_lines {};
  std::string last_search {};
  std::size_t current_match_idx { 0 };
  std::vector<std::size_t> match_lines {};
};

struct HttpRequest
{
  std::string url {};
  std::string method = "GET";
  std::string body {};
};

enum class InputAction : std::uint8_t
{
  None,
  Quit,
  Navigate
};

} // namespace ricochet::core
