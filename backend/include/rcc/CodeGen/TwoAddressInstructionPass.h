// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TWOADDRESSINSTRUCTIONPASS_H
#define RCC_CODEGEN_TWOADDRESSINSTRUCTIONPASS_H

#include "rcc/CodeGen/MachinePass.h"

namespace rcc {

class TargetInstrInfo;

class TwoAddressInstructionPass final : public MachineFunctionPass {
public:
  explicit TwoAddressInstructionPass(const TargetInstrInfo &Info) noexcept : Info(Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  const TargetInstrInfo &Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_TWOADDRESSINSTRUCTIONPASS_H
