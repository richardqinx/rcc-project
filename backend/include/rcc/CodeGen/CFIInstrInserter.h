// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_CFIINSTRINSERTER_H
#define RCC_CODEGEN_CFIINSTRINSERTER_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class TargetFrameLowering;

class CFIInstrInserterPass final : public MachineFunctionPass {
public:
  explicit CFIInstrInserterPass(const TargetFrameLowering &FrameLowering) noexcept
      : FrameLowering(&FrameLowering) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetFrameLowering *FrameLowering = nullptr;
};

} // namespace rcc

#endif // RCC_CODEGEN_CFIINSTRINSERTER_H
