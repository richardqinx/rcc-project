// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_RISCVINSTRINFO_H
#define RCC_LIB_TARGET_RISCV_RISCVINSTRINFO_H

#include "rcc/CodeGen/TargetInstrInfo.h"

#include <cstdint>

namespace rcc::riscv {

enum class BranchConditionCode : std::uint32_t {
  Equal = 0,
  NotEqual = 1,
  SignedLessThan = 2,
  SignedGreaterEqual = 3,
  UnsignedLessThan = 4,
  UnsignedGreaterEqual = 5,
};

[[nodiscard]] const TargetInstrInfo &instrInfo() noexcept;

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_RISCVINSTRINFO_H
