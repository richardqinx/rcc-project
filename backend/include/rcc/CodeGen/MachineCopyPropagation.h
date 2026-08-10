// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECOPYPROPAGATION_H
#define RCC_CODEGEN_MACHINECOPYPROPAGATION_H

#include "rcc/CodeGen/MachinePass.h"

#include <cstddef>
#include <cstdint>

namespace rcc {

class TargetSSAInfo;

enum class MachineCopyPropagationStage : std::uint8_t {
  PreRegisterAllocation,
  PostRegisterAllocation,
};

struct MachineCopyPropagationOptions final {
  std::size_t MaximumPropagatedCopies = 10'000;
};

class MachineCopyPropagationPass final : public MachineFunctionPass {
public:
  MachineCopyPropagationPass(const TargetSSAInfo &Info, MachineCopyPropagationStage Stage,
                             MachineCopyPropagationOptions Options = {}) noexcept
      : Info(&Info), Stage(Stage), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions) const;

  const TargetSSAInfo *Info;
  MachineCopyPropagationStage Stage;
  MachineCopyPropagationOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECOPYPROPAGATION_H
