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
#include "ricochet/tui/terminal.hh"
#include <string>

namespace ricochet::core {

std::string index_to_hint( std::size_t index );
void toggle_link_hints( PageData& page_data, bool show );
void handle_follow_link( const tui::Terminal& terminal,
                         PageData& page_data,
                         std::string& current_url,
                         std::size_t& scroll_y,
                         bool& navigate );

} // namespace ricochet::core
