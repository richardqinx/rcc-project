// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINELICM_H
#define RCC_CODEGEN_MACHINELICM_H

#include "rcc/CodeGen/MachinePass.h"

#include <cstddef>
#include <cstdint>

namespace rcc {

enum class MachineCodeMotionStage : std::uint8_t {
  SelectedSSA,
  PostRegisterAllocation,
};

struct MachineLICMOptions final {
  std::size_t MaximumHoistedInstructions = 10'000;
  std::size_t MaximumHoistedDefinitionsPerLoop = 256;
};

class MachineLICMPass final : public MachineFunctionPass {
public:
  explicit MachineLICMPass(MachineLICMOptions Options = {}) : Options(Options) {}
  MachineLICMPass(MachineCodeMotionStage Stage, MachineLICMOptions Options = {})
      : Stage(Stage), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineAnalysisManager &Analyses);

  MachineCodeMotionStage Stage = MachineCodeMotionStage::SelectedSSA;
  MachineLICMOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINELICM_H
