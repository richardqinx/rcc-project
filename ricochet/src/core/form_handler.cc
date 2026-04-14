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
#include "ricochet/core/ui_input.hh"
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

void update_input_value( PageData& page_data, std::size_t index, const std::string& query )
{
  page_data.inputs[index - 1].value = query;
  const std::string marker = "[I" + std::to_string( index ) + ":";
  const std::string replacement = "[I" + std::to_string( index ) + ":" + query + "]";

  auto update_lines = [&]( std::vector<std::string>& lines_arr ) {
    for ( auto& line : lines_arr ) {
      const std::size_t pos = line.find( marker );
      if ( pos != std::string::npos ) {
        const std::size_t end = line.find( ']', pos );
        if ( end != std::string::npos ) {
          line.replace( pos, end - pos + 1, replacement );
        }
      }
    }
  };
  update_lines( page_data.lines );
  update_lines( page_data.original_lines );
}

void update_single_line_ui( std::string& line, const std::string& marker, const render::InputField& in )
{
  const std::string checked_str = marker + ( in.type == "checkbox" ? "[X] " : "(X) " ) + in.value + "]";
  const std::string unchecked_str = marker + ( in.type == "checkbox" ? "[ ] " : "( ) " ) + in.value + "]";

  const std::string& target_str = in.checked ? checked_str : unchecked_str;
  const std::string& old_str = in.checked ? unchecked_str : checked_str;

  const std::size_t pos = line.find( old_str );
  if ( pos != std::string::npos ) {
    line.replace( pos, old_str.length(), target_str );
  }
}

void update_toggle_ui( PageData& page_data )
{
  for ( std::size_t i = 0; i < page_data.inputs.size(); ++i ) {
    const auto& in = page_data.inputs[i];
    if ( in.type != "checkbox" && in.type != "radio" ) {
      continue;
    }
    const std::string marker = "[I" + std::to_string( i + 1 ) + ":";
    for ( auto& line : page_data.lines ) {
      update_single_line_ui( line, marker, in );
    }
    for ( auto& line : page_data.original_lines ) {
      update_single_line_ui( line, marker, in );
    }
  }
}

InputAction handle_form_input( const tui::Terminal& terminal,
                               PageData& page_data,
                               const std::string& current_url,
                               std::vector<HttpRequest>& history,
                               std::size_t& history_idx )
{
  const std::string num_input = get_terminal_input( terminal, "Select Input [#]:" );
  if ( num_input.empty() ) {
    return InputAction::None;
  }
  std::size_t index = 0;
  try {
    index = std::stoul( num_input );
  } catch ( ... ) {
    return InputAction::None;
  }
  if ( index == 0 || index > page_data.inputs.size() ) {
    return InputAction::None;
  }

  auto& target_input = page_data.inputs[index - 1];
  if ( target_input.type == "submit" || target_input.type == "button" ) {
    return submit_form( target_input, page_data.inputs, current_url, history, history_idx );
  }

  if ( target_input.type == "checkbox" || target_input.type == "radio" ) {
    if ( target_input.type == "radio" ) {
      for ( auto& in : page_data.inputs ) {
        if ( in.name == target_input.name && in.action == target_input.action ) {
          in.checked = false;
        }
      }
      target_input.checked = true;
    } else {
      target_input.checked = !target_input.checked;
    }
    update_toggle_ui( page_data );
    return InputAction::None;
  }

  const std::string query = get_terminal_input( terminal, "Enter Text [I" + std::to_string( index ) + "]:" );
  if ( query.empty() ) {
    return InputAction::None;
  }
  update_input_value( page_data, index, query );
  return InputAction::None;
}

} // namespace ricochet::core
