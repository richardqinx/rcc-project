// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_RISCVINSTRINFO_H
#define RCC_LIB_TARGET_RISCV_RISCVINSTRINFO_H

#include "rcc/CodeGen/TargetInstrInfo.h"

namespace rcc::riscv {

[[nodiscard]] const TargetInstrInfo &instrInfo() noexcept;

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_RISCVINSTRINFO_H
