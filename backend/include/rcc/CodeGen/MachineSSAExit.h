// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINESSAEXIT_H
#define RCC_CODEGEN_MACHINESSAEXIT_H

#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

namespace rcc {

class PHIEliminationPass final : public MachineFunctionPass {
public:
  PHIEliminationPass() noexcept = default;
  explicit PHIEliminationPass(const TargetInstrInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  const TargetInstrInfo *Info = nullptr;
};

class TwoAddressRewritePass final : public MachineFunctionPass {
public:
  TwoAddressRewritePass() noexcept = default;
  explicit TwoAddressRewritePass(const TargetInstrInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  const TargetInstrInfo *Info = nullptr;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINESSAEXIT_H
