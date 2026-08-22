// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PostRAScheduler.h"

#include "MachineSchedulerInternal.h"
#include "rcc/CodeGen/LivePhysRegs.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &postRASchedulerContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "postmisched",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<LiveVariablesAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.RequiredAnalyses.push_back(
        MachineFunctionAnalysisManager::id<MachineBlockFrequencyAnalysis>());
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

} // namespace

const MachinePassContract &PostRASchedulerPass::contract() const noexcept {
  return postRASchedulerContract();
}

Expected<MachinePassResult> PostRASchedulerPass::run(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     MachineFunctionAnalysisManager &Analyses) {
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "post-RA scheduler transaction setup failed: "));
  }
  MachineFunctionAnalysisManager TrialAnalyses;
  auto TrialLiveness =
      TrialAnalyses.getResult<LiveVariablesAnalysis>(*Trial->ClonedFunction, Target);
  if (!TrialLiveness)
    return std::unexpected(TrialLiveness.error());
  auto TrialFrequencies =
      TrialAnalyses.getResult<MachineBlockFrequencyAnalysis>(*Trial->ClonedFunction, Target);
  if (!TrialFrequencies)
    return std::unexpected(TrialFrequencies.error());
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  MachineSchedulerOptions TrialOptions = Options;
  TrialOptions.EmitDebugOutput = false;
  auto TrialRun = runMachineScheduler(*Trial->ClonedFunction, Target, *Model,
                                      ScheduleDAGMode::PostRegisterAllocation, **TrialLiveness,
                                      **TrialFrequencies, TrialOptions, &Actions);
  if (!TrialRun)
    return std::unexpected(TrialRun.error());
  auto TrialRepair =
      repairPostRAMachineBlockLiveness(TrialRun->ChangedBlocks, Target, **TrialLiveness);
  if (!TrialRepair)
    return std::unexpected(TrialRepair.error());

  auto Liveness = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Liveness)
    return std::unexpected(Liveness.error());
  auto Frequencies = Analyses.getResult<MachineBlockFrequencyAnalysis>(Function, Target);
  if (!Frequencies)
    return std::unexpected(Frequencies.error());
  Actions.beginReplay();
  auto Run = runMachineScheduler(Function, Target, *Model, ScheduleDAGMode::PostRegisterAllocation,
                                 **Liveness, **Frequencies, Options, &Actions);
  if (!Run) {
    return std::unexpected(withErrorContext(
        Run.error(), "post-RA scheduler diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  auto Repaired = repairPostRAMachineBlockLiveness(Run->ChangedBlocks, Target, **Liveness);
  if (!Repaired)
    return std::unexpected(Repaired.error());

  MachinePassResult Result;
  Result.Changed = Run->Changed || *Repaired;
  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
  Result.Preserved.preserve<LiveVariablesAnalysis>();
  return Result;
}

} // namespace rcc
