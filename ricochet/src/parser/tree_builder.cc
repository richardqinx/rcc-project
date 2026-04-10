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

#include "ricochet/parser/tree_builder.hh"
#include "ricochet/parser/dom_tree.hh"
#include <utility>
#include <vector>

namespace ricochet::parser {

namespace {
struct TokenVisitor
{
  std::vector<DomNode>* stack;

  void operator()( const TextToken& token ) const
  {
    stack->back().children.emplace_back(
      DomNode { .tag_name = "", .text_content = token.content, .children = {} } );
  }

  void operator()( const TagOpenToken& token ) const
  {
    DomNode new_node { .tag_name = token.name, .text_content = "", .children = {} };

    if ( token.name == "meta" || token.name == "link" || token.name == "img" || token.name == "br"
         || token.name == "hr" || token.name == "input" || token.name == "base" || token.name == "col" ) {

      stack->back().children.push_back( std::move( new_node ) );
    } else {
      stack->push_back( std::move( new_node ) );
    }
  }

  void operator()( const TagCloseToken& /*token*/ ) const
  {
    if ( stack->size() > 1 ) {
      DomNode completed_node = std::move( stack->back() );
      stack->pop_back();
      stack->back().children.push_back( std::move( completed_node ) );
    }
  }
};
} // namespace

DomNode TreeBuilder::build( // NOLINT(readability-convert-member-functions-to-static)
  const std::vector<HtmlToken>& tokens ) const
{
  std::vector<DomNode> stack;
  stack.emplace_back( DomNode { .tag_name = "Document", .text_content = "", .children = {} } );

  TokenVisitor visitor { &stack };
  for ( const auto& token : tokens ) {
    std::visit( visitor, token );
  }

  while ( stack.size() > 1 ) {
    DomNode completed_node = std::move( stack.back() );
    stack.pop_back();
    stack.back().children.push_back( std::move( completed_node ) );
  }

  return std::move( stack.front() );
}

} // namespace ricochet::parser