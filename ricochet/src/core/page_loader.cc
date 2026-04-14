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

#include "ricochet/core/page_loader.hh"
#include "ricochet/core/local_data.hh"
#include "ricochet/core/page_processor.hh"
#include "ricochet/core/utils.hh"
#include "ricochet/parser/html_lexer.hh"
#include "ricochet/parser/tree_builder.hh"
#include "ricochet/render/renderer.hh"

namespace ricochet::core {

PageData load_page( const HttpRequest& req )
{
  std::string html_body;
  if ( req.url == "ricochet://bookmarks" ) {
    html_body = generate_bookmarks_html();
  } else if ( req.url == "ricochet://history" ) {
    html_body = generate_history_html();
  } else if ( req.url == "ricochet://help" ) {
    html_body = generate_help_html();
  } else {
    const net::HttpClient client;
    auto response_result = client.fetch( req.url, req.method, req.body );
    if ( !response_result.has_value() ) {
      return { .lines
               = { "\033[1;31m[!] Failed to load:\033[0m " + req.url, "", "Reason: " + response_result.error() },
               .links = {},
               .inputs = {},
               .title = "Error" };
    }

    const auto& resp = response_result.value();
    std::string c_type = resp.content_type;
    for ( char& c : c_type ) {
      if ( c >= 'A' && c <= 'Z' ) {
        c = static_cast<char>( c + 32 );
      }
    }

    const bool is_text
      = c_type.starts_with( "text/" ) || c_type.contains( "xml" ) || c_type.contains( "json" ) || c_type.empty();

    if ( !is_text ) {
      return process_binary_download( req, resp );
    }
    html_body = response_result.value().body;
  }

  const parser::HtmlLexer lexer;
  const parser::TreeBuilder builder;
  const render::Renderer renderer;

  const auto dom_root = builder.build( lexer.tokenize( html_body ) );
  const render::RenderResult result = renderer.render( dom_root );
  const std::string title = extract_title( dom_root );

  return {
    .lines = split_into_lines( result.text ), .links = result.links, .inputs = result.inputs, .title = title };
}

} // namespace ricochet::core
