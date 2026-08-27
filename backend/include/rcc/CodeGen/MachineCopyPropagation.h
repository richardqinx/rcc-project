// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECOPYPROPAGATION_H
#define RCC_CODEGEN_MACHINECOPYPROPAGATION_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

class TargetInstrInfo;

struct MachineCopyPropagationOptions final {
  std::size_t MaximumPropagatedCopies = 10'000;
  bool UseTargetCopyInstructions = false;
};

class MachineCopyPropagationPass final : public MachineFunctionPass {
public:
  explicit MachineCopyPropagationPass(const TargetInstrInfo &Info,
                                      MachineCopyPropagationOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineFunctionAnalysisManager
                                                                  &Analyses) const;

  const TargetInstrInfo *Info;
  MachineCopyPropagationOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECOPYPROPAGATION_H
