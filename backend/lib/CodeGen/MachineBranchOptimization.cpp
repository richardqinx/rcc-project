// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineBranchOptimization.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &branchOptimizationContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-branch-optimization",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::CFG, MachinePassChange::BlockLayout};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

[[nodiscard]] bool isJumpTableTarget(const MachineBasicBlock &Block,
                                     const MachineFunction &Function) noexcept {
  for (const MachineJumpTableEntry &Entry : Function.jumpTableInfo().tables()) {
    const MachineJumpTableDesc *Description = Entry.description();
    if (Description != nullptr &&
        std::ranges::find(Description->Targets, &Block) != Description->Targets.end()) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasInstructionRecord(const MachineInstr &Instruction,
                                        const MachineFunction &Function) noexcept {
  const MachineInstrStableID ID = Instruction.stableID();
  return std::ranges::any_of(Function.records().callSites(),
                             [ID](const auto &Record) { return Record.CallInstruction == ID; }) ||
         std::ranges::any_of(Function.records().calledGlobals(),
                             [ID](const auto &Record) { return Record.CallInstruction == ID; });
}

[[nodiscard]] bool hasObservableBlockIdentity(const MachineBasicBlock &Block,
                                              const MachineFunction &Function) noexcept {
  const MachineBasicBlockAttributes &Attributes = Block.attributes();
  return Attributes.rawFlags() != 0 || !Attributes.sectionID().isDefault() ||
         Attributes.profileID().has_value() || isJumpTableTarget(Block, Function);
}

[[nodiscard]] bool canDeleteBlock(const MachineBasicBlock &Block,
                                  const MachineFunction &Function) noexcept {
  if (hasObservableBlockIdentity(Block, Function))
    return false;
  for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (hasProtectedAttachment(*Instruction) || hasInstructionRecord(*Instruction, Function)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool hasSuccessor(const MachineBasicBlock &Block,
                                const MachineBasicBlock &Target) noexcept {
  return std::ranges::any_of(Block.successors(), [&Target](const MachineSuccessor &Successor) {
    return Successor.Block == &Target;
  });
}

[[nodiscard]] Expected<bool> foldConstantBranches(MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  const TargetInstrInfo &Info) {
  bool Changed = false;
  MachineCFGUpdater CFG = Function.cfgUpdater();
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    auto Analysis = Info.analyzeBranch(*Block, Function, Target);
    if (!Analysis)
      return std::unexpected(Analysis.error());
    if (Analysis->HasUnanalyzableTerminator || Analysis->Conditional == nullptr ||
        hasProtectedAttachment(*Analysis->Conditional)) {
      continue;
    }
    auto Condition = Info.evaluateBranchCondition(*Analysis->Conditional, Function, Target);
    if (!Condition)
      return std::unexpected(Condition.error());
    if (!Condition->has_value())
      continue;

    MachineBasicBlock *Selected = **Condition ? Analysis->TrueTarget : Analysis->FalseTarget;
    MachineBasicBlock *Rejected = **Condition ? Analysis->FalseTarget : Analysis->TrueTarget;
    if (Selected == nullptr || !hasSuccessor(*Block, *Selected)) {
      return makeUnexpected(ErrorCode::CFGOptimizationFailed,
                            "constant branch has no selected CFG successor");
    }
    if (Analysis->Unconditional != nullptr && hasProtectedAttachment(*Analysis->Unconditional)) {
      continue;
    }

    auto RemovedConditional = Info.removeBranch(*Analysis->Conditional, Function, Target);
    if (!RemovedConditional)
      return std::unexpected(RemovedConditional.error());
    if (Analysis->Unconditional != nullptr) {
      auto RemovedUnconditional = Info.removeBranch(*Analysis->Unconditional, Function, Target);
      if (!RemovedUnconditional)
        return std::unexpected(RemovedUnconditional.error());
    }
    auto Inserted = Info.insertBranch(*Block, *Selected, Function, Target);
    if (!Inserted)
      return std::unexpected(Inserted.error());

    if (Rejected != nullptr && Rejected != Selected && hasSuccessor(*Block, *Rejected)) {
      auto RemovedEdge = CFG.removeSuccessor(*Block, *Rejected);
      if (!RemovedEdge)
        return std::unexpected(RemovedEdge.error());
    }
    auto Probability = CFG.updateBranchProbability(*Block, *Selected, BranchProbability::one());
    if (!Probability)
      return std::unexpected(Probability.error());
    Changed = true;
  }
  return Changed;
}

[[nodiscard]] std::unordered_set<MachineBasicBlock *> reachableBlocks(MachineFunction &Function) {
  std::unordered_set<MachineBasicBlock *> Reachable;
  std::vector<MachineBasicBlock *> Worklist;
  if (Function.firstBlock() != nullptr)
    Worklist.push_back(Function.firstBlock());
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    if (!canDeleteBlock(*Block, Function))
      Worklist.push_back(Block);
  }
  while (!Worklist.empty()) {
    MachineBasicBlock *Block = Worklist.back();
    Worklist.pop_back();
    if (Block == nullptr || !Reachable.insert(Block).second)
      continue;
    for (const MachineSuccessor &Successor : Block->successors())
      Worklist.push_back(Successor.Block);
  }
  return Reachable;
}

[[nodiscard]] Expected<bool> removeUnreachableBlocks(MachineFunction &Function) {
  const std::unordered_set<MachineBasicBlock *> Reachable = reachableBlocks(Function);
  MachineCFGUpdater CFG = Function.cfgUpdater();
  bool Changed = false;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;) {
    MachineBasicBlock *Next = Block->next();
    if (!Reachable.contains(Block) && canDeleteBlock(*Block, Function)) {
      auto Deleted = CFG.deleteBlock(*Block);
      if (!Deleted)
        return std::unexpected(Deleted.error());
      Changed = true;
    }
    Block = Next;
  }
  return Changed;
}

[[nodiscard]] Expected<bool> isForwardingBlock(MachineBasicBlock &Block, MachineFunction &Function,
                                               const TargetDescription &Target,
                                               const TargetInstrInfo &Info) {
  if (&Block == Function.firstBlock() || Block.successors().size() != 1 ||
      Block.successors().front().Block == nullptr || Block.successors().front().Block == &Block ||
      hasObservableBlockIdentity(Block, Function)) {
    return false;
  }
  auto Analysis = Info.analyzeBranch(Block, Function, Target);
  if (!Analysis)
    return std::unexpected(Analysis.error());
  if (Analysis->HasUnanalyzableTerminator || Analysis->Conditional != nullptr)
    return false;
  MachineBasicBlock *Successor = Block.successors().front().Block;
  if (Analysis->Unconditional == nullptr) {
    return Block.firstInstruction() == nullptr && Block.next() == Successor;
  }
  if (Analysis->TrueTarget != Successor || hasProtectedAttachment(*Analysis->Unconditional)) {
    return false;
  }
  return Block.firstInstruction() == Analysis->Unconditional &&
         Analysis->Unconditional->next() == nullptr;
}

[[nodiscard]] Expected<bool> threadForwardingBlocks(MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    const TargetInstrInfo &Info,
                                                    std::size_t MaximumEdges) {
  MachineCFGUpdater CFG = Function.cfgUpdater();
  std::size_t ThreadedEdges = 0;
  bool AnyChange = false;
  bool LocalChange = true;
  while (LocalChange && ThreadedEdges < MaximumEdges) {
    LocalChange = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      auto Forwarding = isForwardingBlock(*Block, Function, Target, Info);
      if (!Forwarding)
        return std::unexpected(Forwarding.error());
      if (!*Forwarding)
        continue;
      MachineBasicBlock *Successor = Block->successors().front().Block;
      const std::vector<MachineBasicBlock *> Predecessors(Block->predecessors().begin(),
                                                          Block->predecessors().end());
      for (MachineBasicBlock *Predecessor : Predecessors) {
        if (ThreadedEdges == MaximumEdges)
          break;
        if (Predecessor == nullptr || Predecessor->successors().size() != 1 ||
            Predecessor->successors().front().Block != Block) {
          continue;
        }
        auto PredAnalysis = Info.analyzeBranch(*Predecessor, Function, Target);
        if (!PredAnalysis)
          return std::unexpected(PredAnalysis.error());
        if (PredAnalysis->HasUnanalyzableTerminator || PredAnalysis->Conditional != nullptr)
          continue;

        if (PredAnalysis->Unconditional != nullptr) {
          if (PredAnalysis->TrueTarget != Block ||
              hasProtectedAttachment(*PredAnalysis->Unconditional)) {
            continue;
          }
          auto Rewritten =
              Info.rewriteBranchTarget(*Predecessor, *Block, *Successor, Function, Target);
          if (!Rewritten)
            return std::unexpected(Rewritten.error());
          if (!*Rewritten) {
            return makeUnexpected(ErrorCode::CFGOptimizationFailed,
                                  "branch threading did not rewrite an explicit branch");
          }
        } else {
          if (Predecessor->next() != Block)
            continue;
          auto Inserted = Info.insertBranch(*Predecessor, *Successor, Function, Target);
          if (!Inserted)
            return std::unexpected(Inserted.error());
        }
        auto Redirected = CFG.replaceSuccessor(*Predecessor, *Block, *Successor);
        if (!Redirected)
          return std::unexpected(Redirected.error());
        ++ThreadedEdges;
        AnyChange = true;
        LocalChange = true;
      }
      if (Block->predecessors().empty() && canDeleteBlock(*Block, Function)) {
        auto Deleted = CFG.deleteBlock(*Block);
        if (!Deleted)
          return std::unexpected(Deleted.error());
        AnyChange = true;
        LocalChange = true;
        break;
      }
    }
  }
  return AnyChange;
}

} // namespace

const MachinePassContract &MachineBranchOptimizationPass::contract() const noexcept {
  return branchOptimizationContract();
}

Expected<MachinePassResult> MachineBranchOptimizationPass::run(MachineFunction &Function,
                                                               const TargetDescription &Target,
                                                               MachineFunctionAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "branch optimization requires selected PHI-free MachineIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        prefixedError(Trial.error(), "branch optimization transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "branch optimization diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachineBranchOptimizationPass::runImplementation(MachineFunction &Function,
                                                 const TargetDescription &Target) const {
  auto Folded = foldConstantBranches(Function, Target, *Info);
  if (!Folded)
    return std::unexpected(Folded.error());
  auto RemovedBeforeThreading = removeUnreachableBlocks(Function);
  if (!RemovedBeforeThreading)
    return std::unexpected(RemovedBeforeThreading.error());
  auto Threaded = threadForwardingBlocks(Function, Target, *Info, Options.MaximumThreadedEdges);
  if (!Threaded)
    return std::unexpected(Threaded.error());
  auto RemovedAfterThreading = removeUnreachableBlocks(Function);
  if (!RemovedAfterThreading)
    return std::unexpected(RemovedAfterThreading.error());

  MachinePassResult Result;
  Result.Changed = *Folded || *RemovedBeforeThreading || *Threaded || *RemovedAfterThreading;
  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                    MachinePassChange::CFG, MachinePassChange::BlockLayout};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
