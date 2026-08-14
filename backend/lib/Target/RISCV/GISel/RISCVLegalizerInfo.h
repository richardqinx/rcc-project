// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_GISEL_RISCVLEGALIZERINFO_H
#define RCC_LIB_TARGET_RISCV_GISEL_RISCVLEGALIZERINFO_H

#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"

namespace rcc::riscv {

class RISCVSubtarget;

class RISCVLegalizerInfo final : public LegalizerInfo {
public:
  explicit RISCVLegalizerInfo(const RISCVSubtarget &Subtarget);
  [[nodiscard]] Expected<void> legalizeCustom(LegalizerHelper &Helper, MachineInstr &Instruction,
                                              const LegalizationDecision &Decision) const override;
  [[nodiscard]] Expected<bool> legalizeIntrinsic(LegalizerHelper &Helper,
                                                 MachineInstr &Instruction) const override;
  [[nodiscard]] bool isSExtCheaperThanZExt(LowLevelType SourceType,
                                           LowLevelType DestinationType) const override;
};

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_GISEL_RISCVLEGALIZERINFO_H
