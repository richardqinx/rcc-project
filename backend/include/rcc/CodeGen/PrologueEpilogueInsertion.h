// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_PROLOGUEEPILOGUEINSERTION_H
#define RCC_CODEGEN_PROLOGUEEPILOGUEINSERTION_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

namespace rcc {

class PrologueEpilogueInsertionPass final : public MachineFunctionPass {
public:
  explicit PrologueEpilogueInsertionPass(const TargetFrameLowering &Info,
                                         FrameLoweringOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target,
                    FrameLoweringOptions RunOptions) const;

  const TargetFrameLowering *Info;
  FrameLoweringOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_PROLOGUEEPILOGUEINSERTION_H
