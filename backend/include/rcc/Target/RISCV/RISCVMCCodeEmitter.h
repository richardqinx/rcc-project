// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVMCCODEEMITTER_H
#define RCC_TARGET_RISCV_RISCVMCCODEEMITTER_H

#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/MC/MCCodeEmitter.h"

namespace rcc::riscv {

class RISCVMCCodeEmitter final : public mc::MCCodeEmitter {
public:
  explicit RISCVMCCodeEmitter(const TargetDescription &Target) noexcept : Target(&Target) {}

  [[nodiscard]] Expected<mc::MCEncodedInstruction>
  encodeInstruction(const mc::MCInst &Instruction) const override;

private:
  const TargetDescription *Target;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVMCCODEEMITTER_H
