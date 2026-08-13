// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_GISEL_RISCVREGISTERBANKINFO_H
#define RCC_LIB_TARGET_RISCV_GISEL_RISCVREGISTERBANKINFO_H

#include "rcc/CodeGen/RegisterBankInfo.h"

namespace rcc::riscv {

class RISCVRegisterBankInfo final : public RegisterBankInfo {
public:
  [[nodiscard]] Expected<RegisterBankSelection>
  selectBank(const MachineFunction &Function, Register Reg, const VirtualRegisterInfo &Info,
             const MachineInstr *Instruction, const TargetDescription &Target) const override;
  [[nodiscard]] Expected<RegisterBankInstructionMapping>
  getInstructionMapping(const MachineInstr &Instruction, const MachineFunction &Function,
                        const TargetDescription &Target) const override;
  [[nodiscard]] Expected<void> constrainSelectedInstruction(MachineInstr &Instruction,
                                                            MachineFunction &Function,
                                                            const TargetDescription &Target) const;
  [[nodiscard]] Expected<void> constrainRegister(MachineFunction &Function, Register Reg,
                                                 const TargetDescription &Target) const;
  [[nodiscard]] Expected<void> validateInstruction(const MachineInstr &Instruction,
                                                   const MachineFunction &Function,
                                                   const TargetDescription &Target) const override;
};

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_GISEL_RISCVREGISTERBANKINFO_H
