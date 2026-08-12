// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINESHRINKWRAPPING_H
#define RCC_CODEGEN_MACHINESHRINKWRAPPING_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

class TargetFrameLowering;

struct MachineShrinkWrappingOptions final {
  std::size_t MaximumAnalyzedBlocks = 10'000;
};

class MachineShrinkWrappingPass final : public MachineFunctionPass {
public:
  explicit MachineShrinkWrappingPass(const TargetFrameLowering &Info,
                                     MachineShrinkWrappingOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetFrameLowering *Info;
  MachineShrinkWrappingOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINESHRINKWRAPPING_H
