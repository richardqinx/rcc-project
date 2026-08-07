// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVCALLLOWERING_H
#define RCC_TARGET_RISCV_RISCVCALLLOWERING_H

#include "rcc/CodeGen/TargetCallLowering.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"

#include <cstdint>

namespace rcc::riscv {

enum class CallTargetFlag : std::uint32_t {
  DirectCall = 1U << 0U,
};

enum class CallStackObjectFlag : std::uint64_t {
  IncomingArgument = 1ULL << 0U,
  OutgoingArgument = 1ULL << 1U,
  ByValCopy = 1ULL << 2U,
  VarArgSaveArea = 1ULL << 3U,
  VarArgAlignmentPadding = 1ULL << 4U,
};

enum class CallValueTransform : std::uint8_t {
  Full,
  AnyExtend,
  SignExtend,
  ZeroExtend,
  Truncate,
  Bitcast,
};

[[nodiscard]] constexpr CallingConventionID
callingConventionID(CallingConvention Convention) noexcept {
  return CallingConventionID::fromValue(static_cast<std::uint32_t>(Convention));
}

[[nodiscard]] const TargetCallLowering &callLowering() noexcept;

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVCALLLOWERING_H
