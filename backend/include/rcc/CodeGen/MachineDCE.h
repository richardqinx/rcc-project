// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEDCE_H
#define RCC_CODEGEN_MACHINEDCE_H

#include "rcc/CodeGen/MachinePass.h"

namespace rcc {

class MachineDCEPass final : public MachineFunctionPass {
public:
  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target);
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEDCE_H
