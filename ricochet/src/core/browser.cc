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

#include "ricochet/core/browser.hh"
#include "ricochet/net/http_client.hh"
#include "ricochet/parser/html_lexer.hh"
#include <iostream>

namespace {
struct TokenPrinter {
    void operator()(const ricochet::parser::TextToken& t) const {
        std::cout << "[TEXT] " << t.content << "\n";
    }
    void operator()(const ricochet::parser::TagOpenToken& t) const {
        std::cout << "[OPEN] <" << t.name << ">\n";
    }
    void operator()(const ricochet::parser::TagCloseToken& t) const {
        std::cout << "[CLOSE] </" << t.name << ">\n";
    }
};
} // namespace
namespace ricochet::core {

int Browser::run( std::string_view initial_url )
{
  std::cout << "-> Fetching: " << initial_url << "...\n";

    const net::HttpClient client;
    auto response_result = client.fetch(initial_url);

    if (!response_result.has_value()) {
        std::cerr << "[!] Ricochet failed to load page: " << response_result.error() << "\n";
        return 1;
    }

    const auto& response = response_result.value();
    std::cout << "[=== HTTP Status: " << response.status_code << " ===]\n";
    
    std::cout << "\n[=== Tokenizing HTML ===]\n";
    const parser::HtmlLexer lexer;
    auto tokens = lexer.tokenize(response.body);

    for (const auto& token : tokens) {
        std::visit(TokenPrinter{}, token);
    }

    return 0;
}

} // namespace ricochet::core
