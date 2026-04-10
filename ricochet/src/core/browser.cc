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
#include "ricochet/parser/tree_builder.hh"
#include <iostream>

namespace {
void print_dom_tree(const ricochet::parser::DomNode& node, std::size_t depth = 0) // NOLINT(misc-no-recursion)
{
    const std::string indent(depth * 2, ' ');
    
    if (node.tag_name.empty()) {
        std::cout << indent << "\"" << node.text_content << "\"\n";
    } else {
        std::cout << indent << "<" << node.tag_name << ">\n";
        for (const auto& child : node.children) {
            print_dom_tree(child, depth + 1);
        }
    }
}
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
    
    const parser::HtmlLexer lexer;
    const auto tokens = lexer.tokenize(response.body);

    const parser::TreeBuilder builder;
    const auto dom_root = builder.build(tokens);

    std::cout << "\n[=== Abstract Syntax Tree (DOM) ===]\n";
    print_dom_tree(dom_root);

    return 0;
}

} // namespace ricochet::core
