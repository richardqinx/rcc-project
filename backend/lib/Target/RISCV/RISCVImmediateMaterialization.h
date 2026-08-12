// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_RISCVIMMEDIATEMATERIALIZATION_H
#define RCC_LIB_TARGET_RISCV_RISCVIMMEDIATEMATERIALIZATION_H

#include <cstddef>
#include <cstdint>

namespace rcc::riscv {

inline constexpr std::size_t DefaultRV64IntegerBuildCost = 5;

[[nodiscard]] std::size_t estimateRV64IntegerBuildCost(std::int64_t Value) noexcept;

[[nodiscard]] bool shouldUseRV64IntegerConstantPool(
    std::int64_t Value, std::size_t MaximumBuildCost = DefaultRV64IntegerBuildCost) noexcept;

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_RISCVIMMEDIATEMATERIALIZATION_H
