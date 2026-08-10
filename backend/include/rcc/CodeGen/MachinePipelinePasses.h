// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEPIPELINEPASSES_H
#define RCC_CODEGEN_MACHINEPIPELINEPASSES_H

#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/TargetCodeGen.h"

namespace rcc {

class LegalizerPass final : public MachineFunctionPass {
public:
  explicit LegalizerPass(const TargetLegalizerInfo *Info) : Info(Info) {}
  explicit LegalizerPass(const TargetLegalizerInfo &Info) : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  const TargetLegalizerInfo *Info;
};

class RegisterBankSelectPass final : public MachineFunctionPass {
public:
  explicit RegisterBankSelectPass(const TargetRegisterBankInfo *Info) : Info(Info) {}
  explicit RegisterBankSelectPass(const TargetRegisterBankInfo &Info) : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetRegisterBankInfo *Info;
};

class InstructionSelectPass final : public MachineFunctionPass {
public:
  explicit InstructionSelectPass(const TargetInstructionSelector *Info) : Info(Info) {}
  explicit InstructionSelectPass(const TargetInstructionSelector &Info) : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  const TargetInstructionSelector *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEPIPELINEPASSES_H
