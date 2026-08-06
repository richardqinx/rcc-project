// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_MIDEBUGSYNTAX_H
#define RCC_MIR_MIDEBUGSYNTAX_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace rcc::mir {

[[nodiscard]] std::optional<std::uint64_t>
parseDIExpressionOpcodeName(std::string_view Name) noexcept;

} // namespace rcc::mir

#endif // RCC_MIR_MIDEBUGSYNTAX_H
