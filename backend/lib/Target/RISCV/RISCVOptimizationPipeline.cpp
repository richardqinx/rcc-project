// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVInstrInfo.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"

namespace rcc::riscv {

MachineOptimizationPipelineTargetInfo optimizationPipelineInfo() noexcept {
  return {
      .InstrInfo = instrInfo(),
      .Combiner = machineCombinerInfo(),
      .MemoryFolding = memoryFoldingInfo(),
      .FrameLowering = frameLowering(),
      .PseudoExpansion = pseudoExpansionInfo(),
      .SchedulingModel = schedulingModel(),
  };
}

} // namespace rcc::riscv
