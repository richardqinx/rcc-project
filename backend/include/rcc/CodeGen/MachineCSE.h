// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECSE_H
#define RCC_CODEGEN_MACHINECSE_H

#include "rcc/CodeGen/MachinePass.h"

#include <cstddef>

namespace rcc {

struct MachineCSEOptions final {
  std::size_t MaximumComparisons = 100'000;
};

class MachineCSEPass final : public MachineFunctionPass {
public:
  explicit MachineCSEPass(MachineCSEOptions Options = {}) : Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineAnalysisManager &Analyses);

  MachineCSEOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECSE_H
