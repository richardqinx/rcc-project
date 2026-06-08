/* Source code for RCC-Legacy.
   Copyright (C) 2026 Richard Qin

This file is part of RCC.

RCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

RCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with RCC; see the file LICENSE.  If not see
<http://www.gnu.org/licenses/>.  */

#pragma once

#include <string>

namespace rcc {
[[noreturn]] inline void fatal(const std::string& msg) {
    fprintf(stderr, "rcc: error: %s\n", msg.c_str());
    exit(1);
}

[[noreturn]] inline void fatal(int line, const std::string& msg) {
    fprintf(stderr, "rcc: error (line %d): %s\n", line, msg.c_str());
    exit(1);
}
}  // namespace rcc