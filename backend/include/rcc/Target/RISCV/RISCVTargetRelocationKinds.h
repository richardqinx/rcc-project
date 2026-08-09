// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVTARGETRELOCATIONKINDS_H
#define RCC_TARGET_RISCV_RISCVTARGETRELOCATIONKINDS_H

#include "rcc/CodeGen/MachineIRTypes.h"

#include <cstdint>

namespace rcc::riscv {

enum class RISCVTargetRelocationKind : std::uint32_t {
  Absolute32 = 1,
  Absolute64,
};

[[nodiscard]] constexpr TargetRelocationKindID
targetRelocationKind(RISCVTargetRelocationKind Kind) noexcept {
  return TargetRelocationKindID::fromValue(static_cast<std::uint32_t>(Kind));
}

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVTARGETRELOCATIONKINDS_H
