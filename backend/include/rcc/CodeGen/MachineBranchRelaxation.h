// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEBRANCHRELAXATION_H
#define RCC_CODEGEN_MACHINEBRANCHRELAXATION_H

#include "rcc/CodeGen/MachinePass.h"

#include <cstddef>

namespace rcc {

class TargetBranchInfo;

struct MachineBranchRelaxationOptions final {
  std::size_t ConvergenceWatchdog = 1000000;
};

class MachineBranchRelaxationPass final : public MachineFunctionPass {
public:
  explicit MachineBranchRelaxationPass(const TargetBranchInfo &Info,
                                       MachineBranchRelaxationOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target) const;

  const TargetBranchInfo *Info;
  MachineBranchRelaxationOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEBRANCHRELAXATION_H
