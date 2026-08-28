// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVMCTARGETLOWERING_H
#define RCC_TARGET_RISCV_RISCVMCTARGETLOWERING_H

#include "rcc/CodeGen/TargetMCInstLowering.h"
#include "rcc/Target/RISCV/RISCVInstPrinter.h"

namespace rcc::riscv {

class RISCVMCTargetLowering final : public TargetMCInstLowering {
public:
  explicit RISCVMCTargetLowering(const TargetDescription &Target) noexcept
      : Target(&Target), Printer(Target) {}

  [[nodiscard]] const mc::MCInstPrinter &instructionPrinter() const noexcept override {
    return Printer;
  }

  [[nodiscard]] Expected<mc::MCInst> lowerInstruction(const MachineInstr &Instruction,
                                                      MachineMCContext &Context) const override;
  [[nodiscard]] Expected<mc::MCExprRef>
  lowerSymbolExpression(mc::MCExprRef Expression, std::uint32_t TargetFlags) const override;
  [[nodiscard]] Expected<void> emitTerminalInstruction(const MachineInstr &Instruction,
                                                       MachineMCContext &Context,
                                                       mc::MCStreamer &Streamer) const override;

private:
  const TargetDescription *Target;
  RISCVInstPrinter Printer;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVMCTARGETLOWERING_H
