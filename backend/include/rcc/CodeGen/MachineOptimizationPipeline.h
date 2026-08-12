// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H
#define RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H

#include "rcc/CodeGen/MachinePassManager.h"

namespace rcc {

class TargetInstrInfo;
class TargetFrameLowering;
class TargetMachineCombinerInfo;
class TargetMemoryFoldingInfo;
class TargetPseudoExpansionInfo;
class TargetSchedulingModel;

struct MachineOptimizationPipelineTargetInfo final {
  const TargetInstrInfo &InstrInfo;
  const TargetMachineCombinerInfo &Combiner;
  const TargetMemoryFoldingInfo &MemoryFolding;
  const TargetFrameLowering &FrameLowering;
  const TargetPseudoExpansionInfo &PseudoExpansion;
  const TargetSchedulingModel &SchedulingModel;
};

[[nodiscard]] Expected<void>
registerMachineOptimizationAnalyses(MachineFunctionAnalysisManager &Analyses);

[[nodiscard]] Expected<void>
addSelectedSSAOptimizationPipeline(MachineFunctionPassManager &Pipeline,
                                   const MachineOptimizationPipelineTargetInfo &TargetInfo);

[[nodiscard]] Expected<void> addPreRegisterAllocationOptimizationPipeline(
    MachineFunctionPassManager &Pipeline, const MachineOptimizationPipelineTargetInfo &TargetInfo);

[[nodiscard]] Expected<void>
addPostRegisterAllocationPipeline(MachineFunctionPassManager &Pipeline,
                                  const MachineOptimizationPipelineTargetInfo &TargetInfo);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H
