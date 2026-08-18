// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVFIXUPKINDS_H
#define RCC_TARGET_RISCV_RISCVFIXUPKINDS_H

#include "rcc/MC/MCFixup.h"

#include <cstdint>

namespace rcc::riscv {

enum class RISCVFixupKind : mc::MCFixupKind {
  High20 = mc::fixup_kind::FirstTarget,
  Low12I,
  Low12S,
  PCRelativeHigh20,
  GOTPCRelativeHigh20,
  PCRelativeLow12I,
  PCRelativeLow12S,
  JAL,
  Branch,
  RVCJump,
  RVCBranch,
  CallPLT,
};

[[nodiscard]] constexpr mc::MCFixupKind fixupKind(RISCVFixupKind Kind) noexcept {
  return static_cast<mc::MCFixupKind>(Kind);
}

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVFIXUPKINDS_H
