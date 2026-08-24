// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVInstrInfo.h"

namespace rcc::riscv {

MachineOptimizationPipelineTargetInfo optimizationPipelineInfo(const RISCVSubtarget &Subtarget,
                                                               bool EmitSchedulerDebugOutput,
                                                               bool EmitFrameDebugOutput) noexcept {
  return {
      .InstrInfo = instrInfo(),
      .Combiner = machineCombinerInfo(),
      .MemoryFolding = memoryFoldingInfo(),
      .FrameLowering = frameLowering(Subtarget.targetABI()),
      .PseudoExpansion = pseudoExpansionInfo(),
      .SchedulingModel = Subtarget.schedulingModel(),
      .SchedulerOptions = {.EmitDebugOutput = EmitSchedulerDebugOutput},
      .FrameOptions = {.EmitDebugOutput = EmitFrameDebugOutput},
  };
}

} // namespace rcc::riscv
