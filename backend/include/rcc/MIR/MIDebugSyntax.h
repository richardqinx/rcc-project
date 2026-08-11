// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_MIDEBUGSYNTAX_H
#define RCC_MIR_MIDEBUGSYNTAX_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rcc::mir {

[[nodiscard]] std::optional<std::uint64_t>
parseDIExpressionOpcodeName(std::string_view Name) noexcept;

[[nodiscard]] std::optional<std::uint64_t>
parseDIExpressionAttributeEncodingName(std::string_view Name) noexcept;

[[nodiscard]] std::string invalidDIExpressionElementMessage(std::string_view Name);

[[nodiscard]] std::string formatDIExpression(std::span<const std::uint64_t> Elements);

} // namespace rcc::mir

#endif // RCC_MIR_MIDEBUGSYNTAX_H
