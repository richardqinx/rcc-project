// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVCodeGen.h"

namespace rcc::riscv {

MachineOptimizationPipelineTargetInfo optimizationPipelineInfo() noexcept {
  return {
      .SSA = ssaInfo(),
      .Combiner = machineCombinerInfo(),
      .MemoryFolding = memoryFoldingInfo(),
      .FrameLowering = frameLowering(),
      .PseudoExpansion = pseudoExpansionInfo(),
      .SchedulingModel = schedulingModel(),
      .BranchInfo = branchInfo(),
  };
}

} // namespace rcc::riscv
