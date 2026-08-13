// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineBlockPlacement.h"

#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

enum class PlacementClass : std::uint8_t {
  Hot,
  Normal,
  Cold,
  Exception,
};

[[nodiscard]] const MachinePassContract &placementContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-block-placement",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses =
            {
                MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                MachineFunctionAnalysisManager::id<MachineBranchProbabilityAnalysis>(),
                MachineFunctionAnalysisManager::id<MachineBlockFrequencyAnalysis>(),
                MachineFunctionAnalysisManager::id<MachineLoopAnalysis>(),
            },
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::BlockLayout};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] PlacementClass classify(const MachineBasicBlock &Block,
                                      std::uint64_t Frequency) noexcept {
  const MachineBasicBlockAttributes &Attributes = Block.attributes();
  if (Attributes.sectionID().kind() == MachineBasicBlockSectionKind::Exception ||
      Attributes.has(MachineBasicBlockAttribute::LandingPad) ||
      Attributes.has(MachineBasicBlockAttribute::EHFuncletEntry) ||
      Attributes.has(MachineBasicBlockAttribute::EHScopeEntry)) {
    return PlacementClass::Exception;
  }
  if (Attributes.sectionID().kind() == MachineBasicBlockSectionKind::Cold)
    return PlacementClass::Cold;
  if (Frequency >= MachineBlockFrequencyAnalysis::EntryFrequency)
    return PlacementClass::Hot;
  if (Frequency < MachineBlockFrequencyAnalysis::EntryFrequency / 1024U)
    return PlacementClass::Cold;
  return PlacementClass::Normal;
}

[[nodiscard]] std::uint32_t classRank(PlacementClass Class) noexcept {
  return static_cast<std::uint32_t>(Class);
}

struct PlacementFacts final {
  MachineBasicBlock *Block = nullptr;
  PlacementClass Class = PlacementClass::Normal;
  std::uint64_t Frequency = 0;
  std::uint32_t LoopDepth = 0;
};

[[nodiscard]] bool betterRoot(const PlacementFacts &Left, const PlacementFacts &Right) noexcept {
  if (Left.Class != Right.Class)
    return classRank(Left.Class) < classRank(Right.Class);
  if (Left.Frequency != Right.Frequency)
    return Left.Frequency > Right.Frequency;
  if (Left.LoopDepth != Right.LoopDepth)
    return Left.LoopDepth > Right.LoopDepth;
  return Left.Block->stableID().value() < Right.Block->stableID().value();
}

} // namespace

const MachinePassContract &MachineBlockPlacementPass::contract() const noexcept {
  return placementContract();
}

Expected<MachinePassResult>
MachineBlockPlacementPass::run(MachineFunction &Function, const TargetDescription &Target,
                               MachineFunctionAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine block placement requires selected PHI-free MachineIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(withErrorContext(Trial.error(), "block placement setup failed: "));
  MachineFunctionAnalysisManager TrialAnalyses;
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, TrialAnalyses, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Analyses, Actions);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "block placement diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult> MachineBlockPlacementPass::runImplementation(
    MachineFunction &Function, const TargetDescription &Target,
    MachineFunctionAnalysisManager &Analyses, MachineActionReplay &Actions) const {
  auto Probabilities = Analyses.getResult<MachineBranchProbabilityAnalysis>(Function, Target);
  if (!Probabilities)
    return std::unexpected(Probabilities.error());
  auto Frequencies = Analyses.getResult<MachineBlockFrequencyAnalysis>(Function, Target);
  if (!Frequencies)
    return std::unexpected(Frequencies.error());
  auto Loops = Analyses.getResult<MachineLoopAnalysis>(Function, Target);
  if (!Loops)
    return std::unexpected(Loops.error());
  auto Dominators = Analyses.getResult<MachineDominatorTreeAnalysis>(Function, Target);
  if (!Dominators)
    return std::unexpected(Dominators.error());

  std::vector<PlacementFacts> Facts;
  Facts.reserve(Function.blockCount());
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    const std::uint64_t Frequency = (*Frequencies)->frequency(*Block);
    Facts.push_back({Block, classify(*Block, Frequency), Frequency, (*Loops)->loopDepth(*Block)});
  }
  if (Facts.size() < 2) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }

  auto FactsFor = [&Facts](const MachineBasicBlock &Block) -> const PlacementFacts & {
    return *std::ranges::find_if(
        Facts, [&Block](const PlacementFacts &Value) { return Value.Block == &Block; });
  };
  std::unordered_set<MachineBasicBlock *> Placed;
  std::vector<MachineBasicBlock *> Order;
  Order.reserve(Facts.size());
  MachineBasicBlock *Root = Function.firstBlock();
  while (Order.size() != Facts.size()) {
    if (Root == nullptr || Placed.contains(Root)) {
      const auto Best = std::ranges::min_element(
          Facts, [&Placed](const PlacementFacts &Left, const PlacementFacts &Right) {
            if (Placed.contains(Left.Block))
              return false;
            if (Placed.contains(Right.Block))
              return true;
            return betterRoot(Left, Right);
          });
      if (Best == Facts.end() || Placed.contains(Best->Block))
        return makeUnexpected(ErrorCode::InternalError, "block placement lost an unplaced block");
      Root = Best->Block;
    }

    while (Root != nullptr && Placed.insert(Root).second) {
      Order.push_back(Root);
      MachineBasicBlock *BestSuccessor = nullptr;
      BranchProbability BestProbability = BranchProbability::zero();
      for (const MachineSuccessor &Successor : (*Probabilities)->successors(*Root)) {
        if (Successor.Block == nullptr || Placed.contains(Successor.Block))
          continue;
        if (BestSuccessor == nullptr || BestProbability < Successor.Probability ||
            (BestProbability == Successor.Probability &&
             betterRoot(FactsFor(*Successor.Block), FactsFor(*BestSuccessor)))) {
          BestSuccessor = Successor.Block;
          BestProbability = Successor.Probability;
        }
      }
      Root = BestSuccessor;
    }
    Root = nullptr;
  }

  const auto &SavePoints = Function.frameInfo().state().SavePoints;
  if (!SavePoints.empty() && SavePoints.front().Block != Function.firstBlock()->stableID()) {
    const auto SavePosition = std::ranges::find_if(Order, [&](const MachineBasicBlock *Block) {
      return Block->stableID() == SavePoints.front().Block;
    });
    if (SavePosition == Order.end()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "block placement cannot find the frame save point");
    }
    MachineBasicBlock *SaveBlock = *SavePosition;
    std::vector<MachineBasicBlock *> OutsideFrame;
    std::vector<MachineBasicBlock *> InsideFrame;
    for (MachineBasicBlock *Block : Order) {
      ((*Dominators)->dominates(*SaveBlock, *Block) ? InsideFrame : OutsideFrame).push_back(Block);
    }
    const auto SaveInRegion = std::ranges::find(InsideFrame, SaveBlock);
    if (SaveInRegion == InsideFrame.end()) {
      return makeUnexpected(ErrorCode::InternalError, "block placement lost the frame save region");
    }
    std::rotate(InsideFrame.begin(), SaveInRegion, std::next(SaveInRegion));
    Order.clear();
    Order.insert(Order.end(), OutsideFrame.begin(), OutsideFrame.end());
    Order.insert(Order.end(), InsideFrame.begin(), InsideFrame.end());
  }

  bool Changed = false;
  for (std::size_t Index = 1; Index < Order.size(); ++Index) {
    if (Order[Index - 1U]->next() != Order[Index]) {
      Changed = true;
      break;
    }
  }
  if (Changed) {
    auto Accepted = Actions.request("apply-block-layout", Function);
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted) {
      MachinePassResult Result;
      Result.Preserved = PreservedAnalyses::all();
      return Result;
    }
  }
  for (std::size_t Index = 1; Index < Order.size(); ++Index) {
    auto Moved = Function.moveBlockAfter(*Order[Index - 1U], *Order[Index]);
    if (!Moved)
      return std::unexpected(Moved.error());
  }
  MachinePassResult Result;
  Result.Changed = Changed;
  if (Changed) {
    Result.Changes = {MachinePassChange::BlockLayout};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
