// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEBRANCHOPTIMIZATION_H
#define RCC_CODEGEN_MACHINEBRANCHOPTIMIZATION_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

class TargetInstrInfo;

struct MachineBranchOptimizationOptions final {
  std::size_t MaximumThreadedEdges = 10'000;
};

class MachineBranchOptimizationPass final : public MachineFunctionPass {
public:
  explicit MachineBranchOptimizationPass(const TargetInstrInfo &Info,
                                         MachineBranchOptimizationOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target) const;

  const TargetInstrInfo *Info;
  MachineBranchOptimizationOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEBRANCHOPTIMIZATION_H
