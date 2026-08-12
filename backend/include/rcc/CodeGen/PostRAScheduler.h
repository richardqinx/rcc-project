// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_POSTRASCHEDULER_H
#define RCC_CODEGEN_POSTRASCHEDULER_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/MachineScheduler.h"

namespace rcc {

class TargetSchedulingModel;

class PostRASchedulerPass final : public MachineFunctionPass {
public:
  explicit PostRASchedulerPass(const TargetSchedulingModel &Model,
                               MachineSchedulerOptions Options = {}) noexcept
      : Model(&Model), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetSchedulingModel *Model;
  MachineSchedulerOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_POSTRASCHEDULER_H
