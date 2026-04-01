/*
 * richard-atp-engine: A saturation-based automated theorem prover for first-order logic.
 * Copyright (C) 2026 Richard Qin
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


#pragma once

/// @file logger.h
/// @brief Minimal high-performance logging macros.

#include <cstdio>

#ifndef ATP_LOG_LEVEL
#define ATP_LOG_LEVEL 2  // 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR
#endif

#define ATP_LOG(level, level_str, fmt, ...)                        \
    do {                                                           \
        if constexpr (level >= ATP_LOG_LEVEL) {                    \
            std::fprintf(stderr, "[%s] " fmt "\n", level_str,     \
                         ##__VA_ARGS__);                           \
        }                                                          \
    } while (0)

#define ATP_TRACE(fmt, ...) ATP_LOG(0, "TRACE", fmt, ##__VA_ARGS__)
#define ATP_DEBUG(fmt, ...) ATP_LOG(1, "DEBUG", fmt, ##__VA_ARGS__)
#define ATP_INFO(fmt, ...)  ATP_LOG(2, "INFO",  fmt, ##__VA_ARGS__)
#define ATP_WARN(fmt, ...)  ATP_LOG(3, "WARN",  fmt, ##__VA_ARGS__)
#define ATP_ERROR(fmt, ...) ATP_LOG(4, "ERROR", fmt, ##__VA_ARGS__)
