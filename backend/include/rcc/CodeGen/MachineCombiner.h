// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECOMBINER_H
#define RCC_CODEGEN_MACHINECOMBINER_H

#include "rcc/CodeGen/MachineCombinerPattern.h"
#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

struct MachineCombinerOptions final {
  std::size_t MaximumRewrites = 4096;
  std::size_t MaximumIterations = 8;
};

class MachineCombinerPass final : public MachineFunctionPass {
public:
  MachineCombinerPass(const TargetMachineCombinerInfo &Info,
                      MachineCombinerOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions) const;

  const TargetMachineCombinerInfo *Info;
  MachineCombinerOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECOMBINER_H
