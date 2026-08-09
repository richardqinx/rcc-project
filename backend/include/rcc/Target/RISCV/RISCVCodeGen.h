// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVCODEGEN_H
#define RCC_TARGET_RISCV_RISCVCODEGEN_H

#include "rcc/CodeGen/TargetCodeGen.h"
#include "rcc/CodeGen/TargetFrameLowering.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"

namespace rcc::riscv {

[[nodiscard]] const TargetLegalizerInfo &legalizerInfo() noexcept;
[[nodiscard]] const TargetRegisterBankInfo &registerBankInfo() noexcept;
[[nodiscard]] const TargetInstructionSelector &instructionSelector() noexcept;
[[nodiscard]] const TargetSSAInfo &ssaInfo() noexcept;
[[nodiscard]] const TargetRegisterAllocationInfo &registerAllocationInfo() noexcept;
[[nodiscard]] const TargetFrameLowering &frameLowering() noexcept;

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVCODEGEN_H
