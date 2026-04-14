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
#include <cstddef>
#include <string>

namespace ricochet::core {

void find_search_matches( PageData& page_data, const std::string& query );
void apply_search_highlight( PageData& page_data, const std::string& query );
void execute_search( PageData& page_data, const std::string& query, std::size_t& scroll_y, bool next_match );

} // namespace ricochet::core
