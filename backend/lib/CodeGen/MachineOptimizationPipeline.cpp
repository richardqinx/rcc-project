// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineOptimizationPipeline.h"

#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineBlockPlacement.h"
#include "rcc/CodeGen/MachineBranchOptimization.h"
#include "rcc/CodeGen/MachineBranchRelaxation.h"
#include "rcc/CodeGen/MachineCSE.h"
#include "rcc/CodeGen/MachineCombiner.h"
#include "rcc/CodeGen/MachineCopyPropagation.h"
#include "rcc/CodeGen/MachineDCE.h"
#include "rcc/CodeGen/MachineFallthrough.h"
#include "rcc/CodeGen/MachineFinalization.h"
#include "rcc/CodeGen/MachineLICM.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineMemoryFolding.h"
#include "rcc/CodeGen/MachinePseudoExpansion.h"
#include "rcc/CodeGen/MachineScheduler.h"
#include "rcc/CodeGen/MachineShrinkWrapping.h"
#include "rcc/CodeGen/MachineSink.h"
#include "rcc/CodeGen/MachineStackSlotColoring.h"
#include "rcc/CodeGen/MachineTailDuplication.h"
#include "rcc/CodeGen/MachineTailMerge.h"
#include "rcc/CodeGen/PostRAScheduler.h"
#include "rcc/CodeGen/PrologueEpilogueInsertion.h"

#include <memory>
#include <utility>

namespace rcc {
namespace {

template <typename Pass, typename... Arguments>
[[nodiscard]] Expected<void> addPass(MachinePassManager &Pipeline, Arguments &&...Values) {
  return Pipeline.addPass(std::make_unique<Pass>(std::forward<Arguments>(Values)...));
}

} // namespace

Expected<void> registerMachineOptimizationAnalyses(MachineAnalysisManager &Analyses) {
  auto Registered = registerStage8MachineAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  return registerStage11MachineAnalyses(Analyses);
}

Expected<void>
addSelectedSSAOptimizationPipeline(MachinePassManager &Pipeline,
                                   const MachineOptimizationPipelineTargetInfo &TargetInfo) {
  auto Added = addPass<MachineCombinerPass>(Pipeline, TargetInfo.Combiner,
                                            MachineCombinerStage::SelectedSSA);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineMemoryFoldingPass>(Pipeline, TargetInfo.MemoryFolding);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineCSEPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineDCEPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineLICMPass>(Pipeline, MachineCodeMotionStage::SelectedSSA);
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<MachineSinkPass>(Pipeline, MachineCodeMotionStage::SelectedSSA);
}

Expected<void> addPreRegisterAllocationOptimizationPipeline(
    MachinePassManager &Pipeline, const MachineOptimizationPipelineTargetInfo &TargetInfo) {
  auto Added = addPass<MachineBranchOptimizationPass>(Pipeline, TargetInfo.InstrInfo);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineCopyPropagationPass>(Pipeline, TargetInfo.InstrInfo,
                                              MachineCopyPropagationStage::PreRegisterAllocation);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineCombinerPass>(Pipeline, TargetInfo.Combiner,
                                       MachineCombinerStage::PreRegisterAllocation);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineDCEPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<MachineSchedulerPass>(Pipeline, TargetInfo.SchedulingModel);
}

Expected<void>
addPostRegisterAllocationPipeline(MachinePassManager &Pipeline,
                                  const MachineOptimizationPipelineTargetInfo &TargetInfo) {
  auto Added = addPass<MachineStackSlotColoringPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineCopyPropagationPass>(Pipeline, TargetInfo.InstrInfo,
                                              MachineCopyPropagationStage::PostRegisterAllocation);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineCombinerPass>(Pipeline, TargetInfo.Combiner,
                                       MachineCombinerStage::PostRegisterAllocation);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineLICMPass>(Pipeline, MachineCodeMotionStage::PostRegisterAllocation);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineSinkPass>(Pipeline, MachineCodeMotionStage::PostRegisterAllocation);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineShrinkWrappingPass>(Pipeline, TargetInfo.FrameLowering);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<PrePEITargetPseudoExpansionPass>(Pipeline, TargetInfo.PseudoExpansion);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<PrologueEpilogueInsertionPass>(Pipeline, TargetInfo.FrameLowering);
  if (!Added)
    return std::unexpected(Added.error());
  Added =
      addPass<MachineCombinerPass>(Pipeline, TargetInfo.Combiner, MachineCombinerStage::PostFrame);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineDCEPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<ExpandPostRAPseudosPass>(Pipeline, TargetInfo.PseudoExpansion);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<PostRASchedulerPass>(Pipeline, TargetInfo.SchedulingModel);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineTailDuplicationPass>(Pipeline, TargetInfo.InstrInfo);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineTailMergePass>(Pipeline, TargetInfo.InstrInfo);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineBlockPlacementPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineFallthroughPass>(Pipeline, TargetInfo.InstrInfo);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<MachineBranchRelaxationPass>(Pipeline, TargetInfo.InstrInfo);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<TargetLatePseudoExpansionPass>(Pipeline, TargetInfo.PseudoExpansion,
                                                 TargetInfo.InstrInfo);
  if (!Added)
    return std::unexpected(Added.error());
  Added = addPass<FinalizeEmissionPseudosPass>(Pipeline);
  if (!Added)
    return std::unexpected(Added.error());
  return addPass<FinalEncodabilityVerifierPass>(Pipeline, TargetInfo.InstrInfo);
}

} // namespace rcc
