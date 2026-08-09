// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H
#define RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H

#include "rcc/CodeGen/MachinePass.h"

namespace rcc {

class TargetBranchInfo;
class TargetFrameLowering;
class TargetMachineCombinerInfo;
class TargetMemoryFoldingInfo;
class TargetPseudoExpansionInfo;
class TargetSchedulingModel;
class TargetSSAInfo;

struct MachineOptimizationPipelineTargetInfo final {
  const TargetSSAInfo &SSA;
  const TargetMachineCombinerInfo &Combiner;
  const TargetMemoryFoldingInfo &MemoryFolding;
  const TargetFrameLowering &FrameLowering;
  const TargetPseudoExpansionInfo &PseudoExpansion;
  const TargetSchedulingModel &SchedulingModel;
  const TargetBranchInfo &BranchInfo;
};

[[nodiscard]] Expected<void> registerMachineOptimizationAnalyses(MachineAnalysisManager &Analyses);

[[nodiscard]] Expected<void>
addSelectedSSAOptimizationPipeline(MachinePassManager &Pipeline,
                                   const MachineOptimizationPipelineTargetInfo &TargetInfo);

[[nodiscard]] Expected<void> addPreRegisterAllocationOptimizationPipeline(
    MachinePassManager &Pipeline, const MachineOptimizationPipelineTargetInfo &TargetInfo);

[[nodiscard]] Expected<void>
addPostRegisterAllocationPipeline(MachinePassManager &Pipeline,
                                  const MachineOptimizationPipelineTargetInfo &TargetInfo);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H
