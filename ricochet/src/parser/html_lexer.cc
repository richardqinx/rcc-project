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

#include "ricochet/parser/html_lexer.hh"

#include <cstdint>
#include <string_view>


namespace ricochet::parser {

namespace {
enum class LexerState : std::uint8_t {
    Data,              // Reading plain text
    TagOpen,           // Just encountered '<'
    TagName,           // Reading the name of an opening tag (e.g., "div")
    ClosingTagName,    // Reading the name of a closing tag (e.g., "/div")
    SwallowAttributes  // Ignore attributes mode (ignore until encountering '>')
};

void emit_text(std::string& buffer, std::vector<HtmlToken>& tokens) {
    if (buffer.empty()) {
        return; // 提前返回，减少 if 嵌套层级
    }

    bool all_whitespace = true;
    for (const char c : buffer) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            all_whitespace = false;
            break;
        }
    }
    
    if (!all_whitespace) {
        tokens.emplace_back(TextToken{buffer});
    }
    buffer.clear();
}

} // namespace

std::vector<HtmlToken> HtmlLexer::tokenize( std::string_view html ) const // NOLINT(readability-convert-member-functions-to-static)
{
  std::vector<HtmlToken> tokens;

  LexerState state = LexerState::Data;
  std::string buffer{};

  for (const char c : html) {
    switch (state) {
    case LexerState::Data:
        if (c == '<') {
            emit_text(buffer, tokens);
            state = LexerState::TagOpen;
        } else {
            buffer += c;
        }
        break;
    case LexerState::TagOpen:
        if (c == '/') {
            state = LexerState::ClosingTagName;
        } else if (std::isalpha(static_cast<unsigned char>(c))) {
            buffer.push_back(c);
            state = LexerState::TagName;
        } else {
            buffer.push_back('<');
            buffer.push_back(c);
            state = LexerState::Data;
        }
        break;
    case LexerState::TagName:
        if (c == '>') {
            tokens.emplace_back(TagOpenToken{buffer});
            buffer.clear();
            state = LexerState::Data;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            tokens.emplace_back(TagOpenToken{buffer});
            buffer.clear();
            state = LexerState::SwallowAttributes;
        } else {
            buffer += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        break;
    case LexerState::ClosingTagName:
        if (c == '>') {
            tokens.emplace_back(TagCloseToken{buffer});
            buffer.clear();
            state = LexerState::Data;
        } else {
            buffer += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        break;
    case LexerState::SwallowAttributes:
        if (c == '>') {
            state = LexerState::Data;
        }
        break;
    }
  }

  emit_text(buffer, tokens);

  return tokens;
}

} // namespace ricochet::parser