// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PHIELIMINATION_H
#define RCC_CODEGEN_PHIELIMINATION_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class TargetInstrInfo;

class PHIEliminationPass final : public MachineFunctionPass {
public:
  explicit PHIEliminationPass(const TargetInstrInfo &Info) noexcept : Info(Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target,
                    MachineFunctionAnalysisManager &Analyses);

  const TargetInstrInfo &Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_PHIELIMINATION_H
