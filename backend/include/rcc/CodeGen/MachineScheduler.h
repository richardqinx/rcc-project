// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINESCHEDULER_H
#define RCC_CODEGEN_MACHINESCHEDULER_H

#include "rcc/CodeGen/MachinePass.h"

#include <cstddef>

namespace rcc {

class TargetSchedulingModel;

struct MachineSchedulerOptions final {
  std::size_t MaximumRegionSize = 512;
  bool EnableBidirectionalScheduling = true;
};

class MachineSchedulerPass final : public MachineFunctionPass {
public:
  explicit MachineSchedulerPass(const TargetSchedulingModel &Model,
                                MachineSchedulerOptions Options = {}) noexcept
      : Model(&Model), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetSchedulingModel *Model;
  MachineSchedulerOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINESCHEDULER_H
