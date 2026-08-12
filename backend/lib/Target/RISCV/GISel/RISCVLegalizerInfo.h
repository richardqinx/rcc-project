// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_GISEL_RISCVLEGALIZERINFO_H
#define RCC_LIB_TARGET_RISCV_GISEL_RISCVLEGALIZERINFO_H

#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"

namespace rcc::riscv {

class RISCVSubtarget;

class RISCVLegalizerInfo final : public LegalizerInfo {
public:
  explicit RISCVLegalizerInfo(const RISCVSubtarget &Subtarget) : Subtarget(Subtarget) {}

  [[nodiscard]] Expected<LegalizationAction>
  classify(const MachineInstr &Instruction, const MachineFunction &Function,
           const TargetDescription &Target) const override;
  [[nodiscard]] Expected<void> lower(MachineInstr &Instruction, MachineFunction &Function,
                                     const TargetDescription &Target,
                                     LegalizationAction Action) const override;

private:
  const RISCVSubtarget &Subtarget;
};

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_GISEL_RISCVLEGALIZERINFO_H
