/*
 * Ricochet - A minimalist CLI web browser for fast, distraction-free navigation.
 * Copyright (C) 2026  Richard Qin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "ricochet/core/types.hh"
#include "ricochet/render/renderer.hh"
#include "ricochet/tui/terminal.hh"
#include <cstddef>
#include <string>
#include <vector>

namespace ricochet::core {

InputAction submit_form( const render::InputField& target_input,
                         const std::vector<render::InputField>& inputs,
                         const std::string& current_url,
                         std::vector<HttpRequest>& history,
                         std::size_t& history_idx );

void update_input_value( PageData& page_data, std::size_t index, const std::string& query );
void update_single_line_ui( std::string& line, const std::string& marker, const render::InputField& in );
void update_toggle_ui( PageData& page_data );

InputAction handle_form_input( const tui::Terminal& terminal,
                               PageData& page_data,
                               const std::string& current_url,
                               std::vector<HttpRequest>& history,
                               std::size_t& history_idx );

} // namespace ricochet::core
