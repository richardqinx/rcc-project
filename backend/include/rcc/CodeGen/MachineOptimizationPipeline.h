// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H
#define RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/MachineScheduler.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

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
  MachineSchedulerOptions SchedulerOptions;
  FrameLoweringOptions FrameOptions;
};

struct MachineOptimizationPipelineOptions final {
  bool UseTargetCopyInstructions = false;
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
                                  const MachineOptimizationPipelineTargetInfo &TargetInfo,
                                  MachineOptimizationPipelineOptions Options = {});

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEOPTIMIZATIONPIPELINE_H
