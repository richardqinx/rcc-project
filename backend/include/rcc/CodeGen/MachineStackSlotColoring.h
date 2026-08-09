// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINESTACKSLOTCOLORING_H
#define RCC_CODEGEN_MACHINESTACKSLOTCOLORING_H

#include "rcc/CodeGen/MachinePass.h"

#include <cstddef>

namespace rcc {

struct MachineStackSlotColoringOptions final {
  std::size_t MaximumColoredSlots = 4'096;
};

class MachineStackSlotColoringPass final : public MachineFunctionPass {
public:
  explicit MachineStackSlotColoringPass(MachineStackSlotColoringOptions Options = {}) noexcept
      : Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function) const;

  MachineStackSlotColoringOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINESTACKSLOTCOLORING_H
