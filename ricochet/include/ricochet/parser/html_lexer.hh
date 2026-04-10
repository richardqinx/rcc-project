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

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ricochet::parser {

    struct TextToken {
    std::string content{};
};

struct TagOpenToken {
    std::string name{};
};

struct TagCloseToken {
    std::string name{};
};

using HtmlToken = std::variant<TextToken, TagOpenToken, TagCloseToken>;

class HtmlLexer
{
public:
  HtmlLexer() = default;
  ~HtmlLexer() = default;

  HtmlLexer( const HtmlLexer& ) = delete;
  HtmlLexer& operator=( const HtmlLexer& ) = delete;
  HtmlLexer( HtmlLexer&& ) = delete;
  HtmlLexer& operator=( HtmlLexer&& ) = delete;

  /**
   * @brief Perform lexical analysis to convert HTML source code into a structured sequence of tokens
   */
  std::vector<HtmlToken> tokenize(std::string_view html) const;

private:
};

} // namespace ricochet::parser