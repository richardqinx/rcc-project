// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFallthrough.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineLivenessRepair.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &fallthroughContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-fallthrough-normalization",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::NoVRegs, MachineFunctionProperty::TracksLiveness},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineAnalysisManager::id<LiveVariablesAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

[[nodiscard]] Expected<std::vector<MachineBasicBlock *>>
normalizeFallthroughs(MachineFunction &Function, const TargetDescription &Target,
                      const TargetInstrInfo &Info) {
  std::vector<MachineBasicBlock *> ChangedBlocks;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    MachineBasicBlock *LayoutSuccessor = Block->next();
    if (LayoutSuccessor == nullptr)
      continue;
    auto Analysis = Info.analyzeBranch(*Block, Function, Target);
    if (!Analysis)
      return std::unexpected(Analysis.error());
    if (Analysis->HasUnanalyzableTerminator)
      continue;
    bool Changed = false;
    if (Analysis->Conditional != nullptr && Analysis->Unconditional != nullptr &&
        Analysis->FalseTarget == LayoutSuccessor) {
      auto Removed = Info.removeBranch(*Analysis->Unconditional, Function, Target);
      if (!Removed)
        return std::unexpected(Removed.error());
      Changed = true;
    } else if (Analysis->Conditional != nullptr && Analysis->Unconditional != nullptr &&
               Analysis->TrueTarget == LayoutSuccessor && Analysis->FalseTarget != nullptr) {
      auto Reversed = Info.reverseBranchCondition(*Analysis->Conditional, *Analysis->FalseTarget,
                                                  Function, Target);
      if (!Reversed)
        return std::unexpected(Reversed.error());
      auto Removed = Info.removeBranch(*Analysis->Unconditional, Function, Target);
      if (!Removed)
        return std::unexpected(Removed.error());
      Changed = true;
    } else if (Analysis->Conditional != nullptr && Analysis->Unconditional == nullptr &&
               Analysis->FalseTarget == LayoutSuccessor) {
      continue;
    } else if (Analysis->Conditional != nullptr && Analysis->Unconditional == nullptr &&
               Analysis->TrueTarget == LayoutSuccessor && Analysis->FalseTarget != nullptr) {
      auto Reversed = Info.reverseBranchCondition(*Analysis->Conditional, *Analysis->FalseTarget,
                                                  Function, Target);
      if (!Reversed)
        return std::unexpected(Reversed.error());
      Changed = true;
    } else if (Analysis->Conditional != nullptr && Analysis->Unconditional == nullptr &&
               Analysis->FalseTarget != nullptr) {
      auto Inserted = Info.insertBranch(*Block, *Analysis->FalseTarget, Function, Target);
      if (!Inserted)
        return std::unexpected(Inserted.error());
      Changed = true;
    } else if (Analysis->Conditional == nullptr && Analysis->Unconditional != nullptr &&
               Analysis->TrueTarget == LayoutSuccessor) {
      auto Removed = Info.removeBranch(*Analysis->Unconditional, Function, Target);
      if (!Removed)
        return std::unexpected(Removed.error());
      Changed = true;
    } else if (Analysis->Conditional == nullptr && Analysis->Unconditional == nullptr &&
               Block->successors().size() == 1 &&
               Block->successors().front().Block != LayoutSuccessor) {
      MachineBasicBlock *Successor = Block->successors().front().Block;
      if (Successor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "fallthrough normalization encountered a null successor");
      }
      auto Inserted = Info.insertBranch(*Block, *Successor, Function, Target);
      if (!Inserted)
        return std::unexpected(Inserted.error());
      Changed = true;
    }
    if (Changed)
      ChangedBlocks.push_back(Block);
  }
  return ChangedBlocks;
}

} // namespace

const MachinePassContract &MachineFallthroughPass::contract() const noexcept {
  return fallthroughContract();
}

Expected<MachinePassResult> MachineFallthroughPass::run(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "fallthrough normalization requires allocated tracked-liveness MIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(prefixedError(Trial.error(), "fallthrough setup failed: "));
  MachineAnalysisManager TrialAnalyses;
  auto TrialLiveness =
      TrialAnalyses.getResult<LiveVariablesAnalysis>(*Trial->ClonedFunction, Target);
  if (!TrialLiveness)
    return std::unexpected(TrialLiveness.error());
  auto TrialChanged = normalizeFallthroughs(*Trial->ClonedFunction, Target, *Info);
  if (!TrialChanged)
    return std::unexpected(TrialChanged.error());
  auto TrialRepair = repairPostRAMachineBlockLiveness(*TrialChanged, Target, **TrialLiveness);
  if (!TrialRepair)
    return std::unexpected(TrialRepair.error());

  auto Liveness = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Liveness)
    return std::unexpected(Liveness.error());
  auto ChangedBlocks = normalizeFallthroughs(Function, Target, *Info);
  if (!ChangedBlocks) {
    return std::unexpected(prefixedError(ChangedBlocks.error(),
                                         "fallthrough diverged after a successful preflight: "));
  }
  auto Repaired = repairPostRAMachineBlockLiveness(*ChangedBlocks, Target, **Liveness);
  if (!Repaired)
    return std::unexpected(Repaired.error());

  MachinePassResult Result;
  Result.Changed = !ChangedBlocks->empty() || *Repaired;
  if (Result.Changed) {
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
    Result.Preserved.preserve<LiveVariablesAnalysis>();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
