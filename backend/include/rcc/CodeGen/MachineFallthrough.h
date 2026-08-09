// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFALLTHROUGH_H
#define RCC_CODEGEN_MACHINEFALLTHROUGH_H

#include "rcc/CodeGen/MachinePass.h"

namespace rcc {

class TargetBranchInfo;

class MachineFallthroughPass final : public MachineFunctionPass {
public:
  explicit MachineFallthroughPass(const TargetBranchInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetBranchInfo *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFALLTHROUGH_H
