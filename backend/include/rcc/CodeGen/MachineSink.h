// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINESINK_H
#define RCC_CODEGEN_MACHINESINK_H

#include "rcc/CodeGen/MachineLICM.h"

#include <cstddef>

namespace rcc {

struct MachineSinkOptions final {
  std::size_t MaximumSunkInstructions = 10'000;
};

class MachineSinkPass final : public MachineFunctionPass {
public:
  explicit MachineSinkPass(MachineSinkOptions Options = {}) : Options(Options) {}
  MachineSinkPass(MachineCodeMotionStage Stage, MachineSinkOptions Options = {})
      : Stage(Stage), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target,
                    MachineFunctionAnalysisManager &Analyses);

  MachineCodeMotionStage Stage = MachineCodeMotionStage::SelectedSSA;
  MachineSinkOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINESINK_H
