// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PROLOGUEEPILOGUEINSERTION_H
#define RCC_CODEGEN_PROLOGUEEPILOGUEINSERTION_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class TargetFrameLowering;

class PrologueEpilogueInsertionPass final : public MachineFunctionPass {
public:
  explicit PrologueEpilogueInsertionPass(const TargetFrameLowering &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target) const;

  const TargetFrameLowering *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_PROLOGUEEPILOGUEINSERTION_H
