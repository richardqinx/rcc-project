// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVInstrInfo.h"

namespace rcc::riscv {

MachineOptimizationPipelineTargetInfo
optimizationPipelineInfo(const RISCVSubtarget &Subtarget, bool EmitSchedulerDebugOutput) noexcept {
  return {
      .InstrInfo = instrInfo(),
      .Combiner = machineCombinerInfo(),
      .MemoryFolding = memoryFoldingInfo(),
      .FrameLowering = frameLowering(),
      .PseudoExpansion = pseudoExpansionInfo(),
      .SchedulingModel = Subtarget.schedulingModel(),
      .SchedulerOptions = {.EmitDebugOutput = EmitSchedulerDebugOutput},
  };
}

} // namespace rcc::riscv
