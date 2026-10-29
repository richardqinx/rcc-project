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

#include "ricochet/core/form_handler.hh"
#include "ricochet/core/utils.hh"

namespace ricochet::core {

InputAction submit_form( const render::InputField& target_input,
                         const std::vector<render::InputField>& inputs,
                         const std::string& current_url,
                         std::vector<HttpRequest>& history,
                         std::size_t& history_idx )
{
  std::string body;
  for ( const auto& in : inputs ) {
    if ( in.action == target_input.action && !in.name.empty() && in.type != "submit" && in.type != "button" ) {
      if ( ( in.type == "checkbox" || in.type == "radio" ) && !in.checked ) {
        continue;
      }
      std::string encoded = in.value;
      for ( char& ch : encoded ) {
        if ( ch == ' ' ) {
          ch = '+';
        }
      }
      if ( !body.empty() ) {
        body += "&";
      }
      body += in.name + "=" + encoded;
    }
  }
  const std::string action
    = target_input.action.empty() ? current_url : normalize_url( target_input.action, current_url );
  HttpRequest next_req { .url = action, .method = "GET", .body = "" };
  if ( target_input.method == "post" || target_input.method == "POST" ) {
    next_req.method = "POST";
    next_req.body = body;
  } else {
    const char sep = action.contains( '?' ) ? '&' : '?';
    next_req.url += sep + body;
  }
  history.resize( history_idx + 1 );
  history.push_back( next_req );
  history_idx++;
  return InputAction::Navigate;
}

} // namespace ricochet::core
