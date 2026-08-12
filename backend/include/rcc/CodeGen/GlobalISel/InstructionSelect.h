// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_INSTRUCTIONSELECT_H
#define RCC_CODEGEN_GLOBALISEL_INSTRUCTIONSELECT_H

#include "rcc/CodeGen/GlobalISel/InstructionSelector.h"
#include "rcc/CodeGen/MachinePass.h"

namespace rcc {

class InstructionSelect final : public MachineFunctionPass {
public:
  explicit InstructionSelect(const InstructionSelector *Info) : Info(Info) {}
  explicit InstructionSelect(const InstructionSelector &Info) : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  const InstructionSelector *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_INSTRUCTIONSELECT_H
