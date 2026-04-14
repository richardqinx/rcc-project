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

#include "ricochet/render/input_renderer.hh"
#include "ricochet/render/internal.hh"
#include "ricochet/render/utils.hh"

namespace ricochet::render {

namespace {

std::string build_input_hint( const std::string& tag_name, bool checked )
{
  std::string placeholder = extract_attr( tag_name, "placeholder" );
  const std::string type = extract_attr( tag_name, "type" );
  std::string val = extract_attr( tag_name, "value" );

  if ( !placeholder.empty() ) {
    return placeholder;
  }
  if ( type == "submit" || type == "button" ) {
    return val.empty() ? "Submit" : val;
  }
  if ( type == "checkbox" ) {
    return checked ? "[X] " + val : "[ ] " + val;
  }
  if ( type == "radio" ) {
    return checked ? "(X) " + val : "( ) " + val;
  }
  if ( type == "hidden" ) {
    return "hidden";
  }
  if ( !val.empty() ) {
    return val;
  }
  return "text";
}

} // namespace

void render_input_node( const parser::DomNode& node,
                        RenderResult& result,
                        const FormContext& ctx,
                        std::string& output )
{
  std::string type = extract_attr( node.tag_name, "type" );
  const std::size_t space_pos = node.tag_name.find( ' ' );
  const std::string base_tag
    = ( space_pos == std::string::npos ) ? node.tag_name : node.tag_name.substr( 0, space_pos );
  if ( base_tag == "button" && type.empty() ) {
    type = "submit";
  }
  const std::string default_val = extract_attr( node.tag_name, "value" );
  const bool is_checked = node.tag_name.contains( "checked" );
  result.inputs.push_back(
    { extract_attr( node.tag_name, "name" ), ctx.action, ctx.method, default_val, type, is_checked } );

  std::string hint = build_input_hint( node.tag_name, is_checked );
  if ( base_tag == "button" ) {
    hint = "Submit";
  }
  if ( hint != "hidden" ) {
    output += " \033[7;33m[I" + std::to_string( result.inputs.size() ) + ":" + hint + "]\033[0m ";
  }
}

} // namespace ricochet::render
