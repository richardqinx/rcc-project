// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_SUPPORT_SHA256_H
#define RCC_SUPPORT_SHA256_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace rcc {

using SHA256Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] SHA256Digest sha256(std::span<const std::byte> Input);
[[nodiscard]] SHA256Digest sha256(std::string_view Input);
[[nodiscard]] std::string sha256Hex(std::span<const std::byte> Input);
[[nodiscard]] std::string sha256Hex(std::string_view Input);

} // namespace rcc

#endif // RCC_SUPPORT_SHA256_H
