// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFINALIZATION_H
#define RCC_CODEGEN_MACHINEFINALIZATION_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class TargetInstrInfo;

class FinalizeEmissionPseudosPass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;
};

class FinalEncodabilityVerifierPass final : public MachineFunctionPass {
public:
  explicit FinalEncodabilityVerifierPass(const TargetInstrInfo &BranchInfo) noexcept
      : BranchInfo(&BranchInfo) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetInstrInfo *BranchInfo;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFINALIZATION_H
