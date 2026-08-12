// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineAnalyses.h"

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::vector<MachineBasicBlock *> linkedBlocks(MachineFunction &Function) {
  std::vector<MachineBasicBlock *> Result;
  Result.reserve(Function.blockCount());
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next())
    Result.push_back(Block);
  return Result;
}

[[nodiscard]] std::unordered_map<const MachineBasicBlock *, std::size_t>
blockOrder(std::span<MachineBasicBlock *const> Blocks) {
  std::unordered_map<const MachineBasicBlock *, std::size_t> Result;
  for (std::size_t Index = 0; Index < Blocks.size(); ++Index)
    Result.emplace(Blocks[Index], Index);
  return Result;
}

[[nodiscard]] bool containsBlock(std::span<MachineBasicBlock *const> Blocks,
                                 const MachineBasicBlock *Block) {
  return std::ranges::find(Blocks, Block) != Blocks.end();
}

[[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
  if (std::numeric_limits<std::uint64_t>::max() - Left < Right)
    return std::numeric_limits<std::uint64_t>::max();
  return Left + Right;
}

[[nodiscard]] std::uint64_t saturatingMultiply(std::uint64_t Value,
                                               std::uint64_t Multiplier) noexcept {
  if (Value != 0 && Multiplier > std::numeric_limits<std::uint64_t>::max() / Value)
    return std::numeric_limits<std::uint64_t>::max();
  return Value * Multiplier;
}

[[nodiscard]] std::uint64_t scaleByProbability(std::uint64_t Value,
                                               BranchProbability Probability) noexcept {
  constexpr std::uint64_t Denominator = std::numeric_limits<std::uint32_t>::max();
  const std::uint64_t Numerator = Probability.rawValue();
  const std::uint64_t Quotient = Value / Denominator;
  const std::uint64_t Remainder = Value % Denominator;
  return saturatingAdd(saturatingMultiply(Quotient, Numerator),
                       (Remainder * Numerator) / Denominator);
}

[[nodiscard]] bool isKnownDistinctObject(const MachineMemOperandDesc &Left,
                                         const MachineMemOperandDesc &Right) noexcept {
  const auto IsUniqueObject = [](MemoryBase::Kind Kind) {
    return Kind == MemoryBase::Kind::FrameIndex || Kind == MemoryBase::Kind::ConstantPool ||
           Kind == MemoryBase::Kind::JumpTable;
  };
  if (IsUniqueObject(Left.Base.kind()) && IsUniqueObject(Right.Base.kind()) &&
      Left.Base.kind() != Right.Base.kind()) {
    return true;
  }
  if (Left.Base.kind() != Right.Base.kind())
    return false;
  switch (Left.Base.kind()) {
  case MemoryBase::Kind::FrameIndex:
    return Left.Base.frameIndex() != Right.Base.frameIndex();
  case MemoryBase::Kind::ConstantPool:
    return Left.Base.constantPoolIndex() != Right.Base.constantPoolIndex();
  case MemoryBase::Kind::JumpTable:
    return Left.Base.jumpTableIndex() != Right.Base.jumpTableIndex();
  default:
    return false;
  }
}

[[nodiscard]] bool hasOrderedMemory(const MachineInstr &Instruction) noexcept {
  for (const MachineMemOperand *Memory : Instruction.memOperands()) {
    if (Memory != nullptr && (Memory->isAtomic() || Memory->isVolatile()))
      return true;
  }
  return false;
}

} // namespace

Expected<void> MachineDominatorTreeAnalysis::Result::build(MachineFunction &Function) {
  Blocks = linkedBlocks(Function);
  BlockToIndex = blockOrder(Blocks);
  Dominators.assign(Blocks.size(), std::vector<bool>(Blocks.size(), false));
  ImmediateDominators.assign(Blocks.size(), nullptr);
  Reachable.assign(Blocks.size(), false);
  UnreachableBlocks.clear();
  if (Blocks.empty())
    return {};

  std::vector<MachineBasicBlock *> Worklist{Blocks.front()};
  Reachable[0] = true;
  for (std::size_t Position = 0; Position < Worklist.size(); ++Position) {
    MachineBasicBlock *Block = Worklist[Position];
    for (const MachineSuccessor &Successor : Block->successors()) {
      const auto Found = BlockToIndex.find(Successor.Block);
      if (Successor.Block == nullptr || Found == BlockToIndex.end()) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "dominator analysis encountered an out-of-function CFG edge");
      }
      if (!Reachable[Found->second]) {
        Reachable[Found->second] = true;
        Worklist.push_back(Successor.Block);
      }
    }
  }

  for (std::size_t BlockIndex = 0; BlockIndex < Blocks.size(); ++BlockIndex) {
    if (!Reachable[BlockIndex]) {
      UnreachableBlocks.push_back(Blocks[BlockIndex]);
      Dominators[BlockIndex][BlockIndex] = true;
      continue;
    }
    if (BlockIndex == 0) {
      Dominators[BlockIndex][BlockIndex] = true;
      continue;
    }
    for (std::size_t Candidate = 0; Candidate < Blocks.size(); ++Candidate)
      Dominators[BlockIndex][Candidate] = Reachable[Candidate];
  }

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (std::size_t BlockIndex = 1; BlockIndex < Blocks.size(); ++BlockIndex) {
      if (!Reachable[BlockIndex])
        continue;
      std::vector<bool> NewDominators(Blocks.size(), false);
      bool HasReachablePredecessor = false;
      for (MachineBasicBlock *Predecessor : Blocks[BlockIndex]->predecessors()) {
        const auto Found = BlockToIndex.find(Predecessor);
        if (Found == BlockToIndex.end() || !Reachable[Found->second])
          continue;
        if (!HasReachablePredecessor) {
          NewDominators = Dominators[Found->second];
          HasReachablePredecessor = true;
        } else {
          for (std::size_t Candidate = 0; Candidate < Blocks.size(); ++Candidate)
            NewDominators[Candidate] =
                NewDominators[Candidate] && Dominators[Found->second][Candidate];
        }
      }
      if (!HasReachablePredecessor) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "reachable non-entry block has no reachable predecessor");
      }
      NewDominators[BlockIndex] = true;
      if (NewDominators != Dominators[BlockIndex]) {
        Dominators[BlockIndex] = std::move(NewDominators);
        Changed = true;
      }
    }
  }

  for (std::size_t BlockIndex = 1; BlockIndex < Blocks.size(); ++BlockIndex) {
    if (!Reachable[BlockIndex])
      continue;
    std::size_t Best = Blocks.size();
    std::size_t BestDepth = 0;
    for (std::size_t Candidate = 0; Candidate < Blocks.size(); ++Candidate) {
      if (Candidate == BlockIndex || !Dominators[BlockIndex][Candidate])
        continue;
      const std::size_t Depth =
          static_cast<std::size_t>(std::ranges::count(Dominators[Candidate], true));
      if (Best == Blocks.size() || Depth > BestDepth) {
        Best = Candidate;
        BestDepth = Depth;
      }
    }
    if (Best == Blocks.size())
      return makeUnexpected(ErrorCode::InternalError, "reachable block has no immediate dominator");
    ImmediateDominators[BlockIndex] = Blocks[Best];
  }
  return {};
}

Expected<void>
MachineDominatorTreeAnalysis::Result::updateAfterCFGChange(MachineFunction &Function) {
  return build(Function);
}

bool MachineDominatorTreeAnalysis::Result::isReachable(
    const MachineBasicBlock &Block) const noexcept {
  const auto Found = BlockToIndex.find(&Block);
  return Found != BlockToIndex.end() && Reachable[Found->second];
}

bool MachineDominatorTreeAnalysis::Result::dominates(
    const MachineBasicBlock &Dominator, const MachineBasicBlock &Block) const noexcept {
  const auto DominatorPosition = BlockToIndex.find(&Dominator);
  const auto BlockPosition = BlockToIndex.find(&Block);
  if (DominatorPosition == BlockToIndex.end() || BlockPosition == BlockToIndex.end() ||
      !Reachable[BlockPosition->second]) {
    return false;
  }
  return Dominators[BlockPosition->second][DominatorPosition->second];
}

bool MachineDominatorTreeAnalysis::Result::dominates(
    const MachineInstr &Dominator, const MachineInstr &Instruction) const noexcept {
  const MachineBasicBlock *DominatorBlock = Dominator.parent();
  const MachineBasicBlock *InstructionBlock = Instruction.parent();
  if (DominatorBlock == nullptr || InstructionBlock == nullptr)
    return false;
  if (DominatorBlock != InstructionBlock)
    return dominates(*DominatorBlock, *InstructionBlock);
  for (const MachineInstr *Current = DominatorBlock->firstInstruction(); Current != nullptr;
       Current = Current->next()) {
    if (Current == &Dominator)
      return true;
    if (Current == &Instruction)
      return false;
  }
  return false;
}

const MachineBasicBlock *MachineDominatorTreeAnalysis::Result::immediateDominator(
    const MachineBasicBlock &Block) const noexcept {
  const auto Found = BlockToIndex.find(&Block);
  return Found == BlockToIndex.end() ? nullptr : ImmediateDominators[Found->second];
}

const MachineBasicBlock *MachineDominatorTreeAnalysis::Result::nearestCommonDominator(
    const MachineBasicBlock &Left, const MachineBasicBlock &Right) const noexcept {
  if (!isReachable(Left) || !isReachable(Right))
    return nullptr;
  std::unordered_set<const MachineBasicBlock *> LeftAncestors;
  for (const MachineBasicBlock *Current = &Left; Current != nullptr;
       Current = immediateDominator(*Current)) {
    LeftAncestors.insert(Current);
  }
  for (const MachineBasicBlock *Current = &Right; Current != nullptr;
       Current = immediateDominator(*Current)) {
    if (LeftAncestors.contains(Current))
      return Current;
  }
  return nullptr;
}

Expected<MachineDominatorTreeAnalysis::Result>
MachineDominatorTreeAnalysis::run(MachineFunction &Function, const TargetDescription &,
                                  MachineFunctionAnalysisManager &) {
  Result ResultValue;
  auto Built = ResultValue.build(Function);
  if (!Built)
    return std::unexpected(Built.error());
  return ResultValue;
}

const MachineLoop *
MachineLoopAnalysis::Result::loopFor(const MachineBasicBlock &Block) const noexcept {
  const auto Found = InnermostLoop.find(&Block);
  return Found == InnermostLoop.end() ? nullptr : &Loops[Found->second];
}

std::uint32_t
MachineLoopAnalysis::Result::loopDepth(const MachineBasicBlock &Block) const noexcept {
  const MachineLoop *Loop = loopFor(Block);
  return Loop == nullptr ? 0 : Loop->Depth;
}

bool MachineLoopAnalysis::Result::isIrreducible(const MachineBasicBlock &Block) const noexcept {
  return IrreducibleBlocks.contains(&Block);
}

Expected<MachineLoopAnalysis::Result>
MachineLoopAnalysis::run(MachineFunction &Function, const TargetDescription &Target,
                         MachineFunctionAnalysisManager &Analyses) {
  auto Dominators = Analyses.getResult<MachineDominatorTreeAnalysis>(Function, Target);
  if (!Dominators)
    return std::unexpected(Dominators.error());

  Result ResultValue;
  std::vector<MachineBasicBlock *> Blocks = linkedBlocks(Function);
  const auto Order = blockOrder(Blocks);
  std::map<MachineBasicBlock *, std::unordered_set<MachineBasicBlock *>> LoopBlocks;
  std::unordered_map<MachineBasicBlock *, std::vector<MachineBasicBlock *>> LoopLatches;

  for (MachineBasicBlock *Latch : Blocks) {
    if (!(*Dominators)->isReachable(*Latch))
      continue;
    for (const MachineSuccessor &Successor : Latch->successors()) {
      MachineBasicBlock *Header = Successor.Block;
      if (Header == nullptr || !(*Dominators)->dominates(*Header, *Latch))
        continue;
      auto &Members = LoopBlocks[Header];
      Members.insert(Header);
      Members.insert(Latch);
      LoopLatches[Header].push_back(Latch);
      std::vector<MachineBasicBlock *> Worklist;
      if (Latch != Header)
        Worklist.push_back(Latch);
      for (std::size_t Position = 0; Position < Worklist.size(); ++Position) {
        MachineBasicBlock *Current = Worklist[Position];
        for (MachineBasicBlock *Predecessor : Current->predecessors()) {
          if (!(*Dominators)->isReachable(*Predecessor) ||
              !(*Dominators)->dominates(*Header, *Predecessor)) {
            continue;
          }
          if (Members.insert(Predecessor).second && Predecessor != Header)
            Worklist.push_back(Predecessor);
        }
      }
    }
  }

  std::vector<MachineBasicBlock *> Headers;
  Headers.reserve(LoopBlocks.size());
  for (const auto &[Header, Members] : LoopBlocks) {
    (void)Members;
    Headers.push_back(Header);
  }
  std::ranges::sort(Headers,
                    [&Order](const MachineBasicBlock *Left, const MachineBasicBlock *Right) {
                      return Order.at(Left) < Order.at(Right);
                    });
  for (MachineBasicBlock *Header : Headers) {
    MachineLoop Loop;
    Loop.Header = Header;
    Loop.Blocks.assign(LoopBlocks[Header].begin(), LoopBlocks[Header].end());
    Loop.Latches = LoopLatches[Header];
    std::ranges::sort(Loop.Blocks,
                      [&Order](const MachineBasicBlock *Left, const MachineBasicBlock *Right) {
                        return Order.at(Left) < Order.at(Right);
                      });
    std::ranges::sort(Loop.Latches,
                      [&Order](const MachineBasicBlock *Left, const MachineBasicBlock *Right) {
                        return Order.at(Left) < Order.at(Right);
                      });
    Loop.Latches.erase(std::ranges::unique(Loop.Latches).begin(), Loop.Latches.end());
    ResultValue.Loops.push_back(std::move(Loop));
  }

  for (std::size_t Index = 0; Index < ResultValue.Loops.size(); ++Index) {
    const MachineLoop &Loop = ResultValue.Loops[Index];
    std::size_t Parent = static_cast<std::size_t>(-1);
    for (std::size_t Candidate = 0; Candidate < ResultValue.Loops.size(); ++Candidate) {
      if (Candidate == Index || ResultValue.Loops[Candidate].Blocks.size() <= Loop.Blocks.size()) {
        continue;
      }
      const bool Contains = std::ranges::all_of(Loop.Blocks, [&](MachineBasicBlock *Block) {
        return containsBlock(ResultValue.Loops[Candidate].Blocks, Block);
      });
      if (Contains &&
          (Parent == static_cast<std::size_t>(-1) ||
           ResultValue.Loops[Candidate].Blocks.size() < ResultValue.Loops[Parent].Blocks.size())) {
        Parent = Candidate;
      }
    }
    ResultValue.Loops[Index].Parent = Parent;
  }
  for (std::size_t Index = 0; Index < ResultValue.Loops.size(); ++Index) {
    const std::size_t Parent = ResultValue.Loops[Index].Parent;
    if (Parent != static_cast<std::size_t>(-1))
      ResultValue.Loops[Parent].Children.push_back(Index);
    std::uint32_t Depth = 1;
    for (std::size_t Ancestor = Parent; Ancestor != static_cast<std::size_t>(-1);
         Ancestor = ResultValue.Loops[Ancestor].Parent) {
      ++Depth;
    }
    ResultValue.Loops[Index].Depth = Depth;
  }
  for (std::size_t Index = 0; Index < ResultValue.Loops.size(); ++Index) {
    for (MachineBasicBlock *Block : ResultValue.Loops[Index].Blocks) {
      const auto Existing = ResultValue.InnermostLoop.find(Block);
      if (Existing == ResultValue.InnermostLoop.end() ||
          ResultValue.Loops[Existing->second].Depth < ResultValue.Loops[Index].Depth) {
        ResultValue.InnermostLoop[Block] = Index;
      }
    }
  }

  std::unordered_set<MachineBasicBlock *> Visited;
  std::vector<MachineBasicBlock *> FinishOrder;
  for (MachineBasicBlock *Root : Blocks) {
    if (!(*Dominators)->isReachable(*Root) || Visited.contains(Root))
      continue;
    std::vector<std::pair<MachineBasicBlock *, std::size_t>> Stack{{Root, 0}};
    Visited.insert(Root);
    while (!Stack.empty()) {
      auto &[Current, NextSuccessor] = Stack.back();
      if (NextSuccessor < Current->successors().size()) {
        MachineBasicBlock *Next = Current->successors()[NextSuccessor++].Block;
        if (Next != nullptr && (*Dominators)->isReachable(*Next) && Visited.insert(Next).second)
          Stack.emplace_back(Next, 0);
        continue;
      }
      FinishOrder.push_back(Current);
      Stack.pop_back();
    }
  }

  Visited.clear();
  for (auto RootPosition = FinishOrder.rbegin(); RootPosition != FinishOrder.rend();
       ++RootPosition) {
    MachineBasicBlock *Root = *RootPosition;
    if (!Visited.insert(Root).second)
      continue;
    std::vector<MachineBasicBlock *> Component;
    std::vector<MachineBasicBlock *> Worklist{Root};
    for (std::size_t Position = 0; Position < Worklist.size(); ++Position) {
      MachineBasicBlock *Current = Worklist[Position];
      Component.push_back(Current);
      for (MachineBasicBlock *Predecessor : Current->predecessors()) {
        if ((*Dominators)->isReachable(*Predecessor) && Visited.insert(Predecessor).second)
          Worklist.push_back(Predecessor);
      }
    }
    bool Cyclic = Component.size() > 1;
    if (!Cyclic) {
      Cyclic = std::ranges::any_of(
          Component.front()->successors(),
          [Root](const MachineSuccessor &Successor) { return Successor.Block == Root; });
    }
    if (!Cyclic)
      continue;

    std::unordered_set<MachineBasicBlock *> ComponentSet(Component.begin(), Component.end());
    std::unordered_set<MachineBasicBlock *> EntrySet;
    for (MachineBasicBlock *Block : Component) {
      if (std::ranges::any_of(Block->predecessors(), [&](MachineBasicBlock *Predecessor) {
            return !ComponentSet.contains(Predecessor);
          })) {
        EntrySet.insert(Block);
      }
    }
    if (EntrySet.size() <= 1)
      continue;

    std::ranges::sort(Component,
                      [&Order](const MachineBasicBlock *Left, const MachineBasicBlock *Right) {
                        return Order.at(Left) < Order.at(Right);
                      });
    IrreducibleMachineRegion Region;
    Region.Blocks = std::move(Component);
    Region.Entries.assign(EntrySet.begin(), EntrySet.end());
    std::ranges::sort(Region.Entries,
                      [&Order](const MachineBasicBlock *Left, const MachineBasicBlock *Right) {
                        return Order.at(Left) < Order.at(Right);
                      });
    for (MachineBasicBlock *Block : Region.Blocks)
      ResultValue.IrreducibleBlocks.insert(Block);
    ResultValue.IrreducibleRegions.push_back(std::move(Region));
  }
  std::ranges::sort(
      ResultValue.IrreducibleRegions,
      [&Order](const IrreducibleMachineRegion &Left, const IrreducibleMachineRegion &Right) {
        return Order.at(Left.Blocks.front()) < Order.at(Right.Blocks.front());
      });
  return ResultValue;
}

BranchProbability
MachineBranchProbabilityAnalysis::Result::probability(const MachineBasicBlock &From,
                                                      const MachineBasicBlock &To) const noexcept {
  const auto Found = Probabilities.find(&From);
  if (Found == Probabilities.end())
    return BranchProbability::zero();
  for (const MachineSuccessor &Successor : Found->second) {
    if (Successor.Block == &To)
      return Successor.Probability;
  }
  return BranchProbability::zero();
}

std::span<const MachineSuccessor> MachineBranchProbabilityAnalysis::Result::successors(
    const MachineBasicBlock &Block) const noexcept {
  const auto Found = Probabilities.find(&Block);
  return Found == Probabilities.end() ? std::span<const MachineSuccessor>{}
                                      : std::span<const MachineSuccessor>{Found->second};
}

Expected<MachineBranchProbabilityAnalysis::Result>
MachineBranchProbabilityAnalysis::run(MachineFunction &Function, const TargetDescription &,
                                      MachineFunctionAnalysisManager &) {
  Result ResultValue;
  const std::vector<MachineBasicBlock *> Blocks = linkedBlocks(Function);
  const auto Order = blockOrder(Blocks);
  for (MachineBasicBlock *Block : Blocks) {
    std::vector<MachineSuccessor> Successors(Block->successors().begin(),
                                             Block->successors().end());
    for (const MachineSuccessor &Successor : Successors) {
      if (Successor.Block == nullptr || !Order.contains(Successor.Block)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "branch-probability analysis encountered an invalid CFG edge");
      }
    }
    ResultValue.Probabilities.emplace(Block, std::move(Successors));
  }
  return ResultValue;
}

std::uint64_t
MachineBlockFrequencyAnalysis::Result::frequency(const MachineBasicBlock &Block) const noexcept {
  const auto Found = Frequencies.find(&Block);
  return Found == Frequencies.end() ? 0 : Found->second;
}

Expected<MachineBlockFrequencyAnalysis::Result>
MachineBlockFrequencyAnalysis::run(MachineFunction &Function, const TargetDescription &Target,
                                   MachineFunctionAnalysisManager &Analyses) {
  auto Dominators = Analyses.getResult<MachineDominatorTreeAnalysis>(Function, Target);
  auto Loops = Analyses.getResult<MachineLoopAnalysis>(Function, Target);
  auto Probabilities = Analyses.getResult<MachineBranchProbabilityAnalysis>(Function, Target);
  if (!Dominators)
    return std::unexpected(Dominators.error());
  if (!Loops)
    return std::unexpected(Loops.error());
  if (!Probabilities)
    return std::unexpected(Probabilities.error());

  Result ResultValue;
  MachineBasicBlock *Entry = Function.firstBlock();
  if (Entry == nullptr)
    return ResultValue;

  std::unordered_set<MachineBasicBlock *> Visited;
  std::vector<MachineBasicBlock *> PostOrder;
  std::vector<std::pair<MachineBasicBlock *, std::size_t>> Stack{{Entry, 0}};
  Visited.insert(Entry);
  while (!Stack.empty()) {
    auto &[Current, NextSuccessor] = Stack.back();
    const auto Successors = (*Probabilities)->successors(*Current);
    if (NextSuccessor < Successors.size()) {
      MachineBasicBlock *Next = Successors[NextSuccessor++].Block;
      if (Next != nullptr && (*Dominators)->isReachable(*Next) && Visited.insert(Next).second)
        Stack.emplace_back(Next, 0);
      continue;
    }
    PostOrder.push_back(Current);
    Stack.pop_back();
  }
  std::ranges::reverse(PostOrder);
  std::unordered_map<MachineBasicBlock *, std::uint64_t> BaseFrequencies;
  BaseFrequencies[Entry] = EntryFrequency;
  for (MachineBasicBlock *Block : PostOrder) {
    const std::uint64_t Frequency = BaseFrequencies[Block];

    const auto Successors = (*Probabilities)->successors(*Block);
    const bool UseUniformProbability =
        !Successors.empty() && std::ranges::all_of(Successors, [](const MachineSuccessor &Edge) {
          return Edge.Probability == BranchProbability::zero();
        });
    for (std::size_t SuccessorIndex = 0; SuccessorIndex < Successors.size(); ++SuccessorIndex) {
      const MachineSuccessor &Successor = Successors[SuccessorIndex];
      if (Successor.Block == nullptr || (*Dominators)->dominates(*Successor.Block, *Block))
        continue;
      std::uint64_t Contribution = 0;
      if (UseUniformProbability) {
        const std::uint64_t Count = Successors.size();
        Contribution = Frequency / Count;
        if (SuccessorIndex < Frequency % Count)
          ++Contribution;
      } else {
        Contribution = scaleByProbability(Frequency, Successor.Probability);
      }
      BaseFrequencies[Successor.Block] =
          saturatingAdd(BaseFrequencies[Successor.Block], Contribution);
    }
  }
  for (MachineBasicBlock *Block : linkedBlocks(Function)) {
    if (!(*Dominators)->isReachable(*Block)) {
      ResultValue.Frequencies[Block] = 0;
      continue;
    }
    std::uint64_t Frequency = std::max<std::uint64_t>(BaseFrequencies[Block], 1);
    for (std::uint32_t Level = 0; Level < (*Loops)->loopDepth(*Block); ++Level)
      Frequency = saturatingMultiply(Frequency, 8);
    ResultValue.Frequencies[Block] = Frequency;
  }
  return ResultValue;
}

MachineAliasResult
MachineAliasAnalysis::Result::alias(const MachineMemOperand &Left,
                                    const MachineMemOperand &Right) const noexcept {
  const MachineMemOperandDesc &LeftDesc = Left.description();
  const MachineMemOperandDesc &RightDesc = Right.description();
  if (isKnownDistinctObject(LeftDesc, RightDesc))
    return MachineAliasResult::NoAlias;

  const bool SameBase =
      LeftDesc.Base == RightDesc.Base && LeftDesc.Base.kind() != MemoryBase::Kind::Unknown;
  const bool SameAliasSource = LeftDesc.AliasSource && RightDesc.AliasSource &&
                               LeftDesc.AliasSource == RightDesc.AliasSource;
  if (!SameBase && !SameAliasSource)
    return MachineAliasResult::MayAlias;
  if (!LeftDesc.Size.isKnown() || !RightDesc.Size.isKnown() || LeftDesc.Size.isScalable() ||
      RightDesc.Size.isScalable()) {
    return MachineAliasResult::MayAlias;
  }

  const std::uint64_t LeftSize = LeftDesc.Size.minimumBytes();
  const std::uint64_t RightSize = RightDesc.Size.minimumBytes();
  if (LeftDesc.Offset == RightDesc.Offset)
    return LeftSize == RightSize ? MachineAliasResult::MustAlias : MachineAliasResult::PartialAlias;

  const bool LeftFirst = LeftDesc.Offset < RightDesc.Offset;
  const std::int64_t Lower = LeftFirst ? LeftDesc.Offset : RightDesc.Offset;
  const std::int64_t Higher = LeftFirst ? RightDesc.Offset : LeftDesc.Offset;
  const std::uint64_t LowerSize = LeftFirst ? LeftSize : RightSize;
  const std::uint64_t Distance =
      static_cast<std::uint64_t>(Higher) - static_cast<std::uint64_t>(Lower);
  return Distance >= LowerSize ? MachineAliasResult::NoAlias : MachineAliasResult::PartialAlias;
}

Expected<MachineAliasAnalysis::Result> MachineAliasAnalysis::run(MachineFunction &,
                                                                 const TargetDescription &,
                                                                 MachineFunctionAnalysisManager &) {
  return Result{};
}

std::span<const MachineMemoryDependency> MachineMemoryDependenceAnalysis::Result::dependencies(
    const MachineInstr &Instruction) const noexcept {
  const auto Found = DestinationRanges.find(Instruction.stableID());
  if (Found == DestinationRanges.end())
    return {};
  return std::span<const MachineMemoryDependency>(Dependencies)
      .subspan(Found->second.Begin, Found->second.Size);
}

const MachineMemoryDependenceRegion *
MachineMemoryDependenceAnalysis::Result::regionFor(const MachineBasicBlock &Block) const noexcept {
  const auto Found = BlockToRegion.find(&Block);
  return Found == BlockToRegion.end() ? nullptr : &Regions[Found->second];
}

std::span<const MachineMemoryDependency> MachineMemoryDependenceAnalysis::Result::dependencies(
    const MachineBasicBlock &Block) const noexcept {
  const auto Found = BlockRanges.find(&Block);
  if (Found == BlockRanges.end())
    return {};
  return std::span<const MachineMemoryDependency>(Dependencies)
      .subspan(Found->second.Begin, Found->second.Size);
}

Expected<MachineMemoryDependenceAnalysis::Result>
MachineMemoryDependenceAnalysis::run(MachineFunction &Function, const TargetDescription &Target,
                                     MachineFunctionAnalysisManager &Analyses) {
  auto Alias = Analyses.getResult<MachineAliasAnalysis>(Function, Target);
  if (!Alias)
    return std::unexpected(Alias.error());

  struct MemoryNode final {
    MachineInstr *Instruction = nullptr;
    bool Reads = false;
    bool Writes = false;
    bool Ordered = false;
    bool Barrier = false;
  };

  Result ResultValue;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    std::vector<MemoryNode> Nodes;
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "memory-dependence analysis encountered an unknown opcode");
      }
      MemoryNode Node;
      Node.Instruction = Instruction;
      Node.Reads = Descriptor->Flags.has(InstructionFlag::MayLoad);
      Node.Writes = Descriptor->Flags.has(InstructionFlag::MayStore);
      Node.Ordered = hasOrderedMemory(*Instruction);
      Node.Barrier = Descriptor->Flags.has(InstructionFlag::Call) ||
                     Descriptor->Flags.has(InstructionFlag::UnmodeledSideEffects) ||
                     (Descriptor->Flags.has(InstructionFlag::HasSideEffects) &&
                      Instruction->memOperands().empty());
      for (const MachineMemOperand *Memory : Instruction->memOperands()) {
        if (Memory == nullptr)
          continue;
        Node.Reads = Node.Reads || Memory->isLoad();
        Node.Writes = Node.Writes || Memory->isStore();
      }
      if (Node.Reads || Node.Writes || Node.Ordered || Node.Barrier)
        Nodes.push_back(Node);
    }

    MachineMemoryDependenceRegion Region;
    Region.Block = Block;
    Region.Instructions.reserve(Nodes.size());
    for (const MemoryNode &Node : Nodes)
      Region.Instructions.push_back(Node.Instruction->stableID());
    ResultValue.BlockToRegion.emplace(Block, ResultValue.Regions.size());
    ResultValue.Regions.push_back(std::move(Region));

    const std::size_t RegionBegin = ResultValue.Dependencies.size();
    for (std::size_t Destination = 0; Destination < Nodes.size(); ++Destination) {
      for (std::size_t Source = 0; Source < Destination; ++Source) {
        const MemoryNode &From = Nodes[Source];
        const MemoryNode &To = Nodes[Destination];
        if (!From.Writes && !To.Writes && !From.Ordered && !To.Ordered && !From.Barrier &&
            !To.Barrier) {
          continue;
        }

        bool MayAlias = From.Barrier || To.Barrier || From.Ordered || To.Ordered ||
                        From.Instruction->memOperands().empty() ||
                        To.Instruction->memOperands().empty();
        if (!MayAlias) {
          for (const MachineMemOperand *Left : From.Instruction->memOperands()) {
            for (const MachineMemOperand *Right : To.Instruction->memOperands()) {
              if (Left != nullptr && Right != nullptr &&
                  (*Alias)->alias(*Left, *Right) != MachineAliasResult::NoAlias) {
                MayAlias = true;
                break;
              }
            }
            if (MayAlias)
              break;
          }
        }
        if (!MayAlias)
          continue;

        MachineMemoryDependencyKind Kind = MachineMemoryDependencyKind::Ordered;
        if (From.Writes && To.Reads)
          Kind = MachineMemoryDependencyKind::Flow;
        else if (From.Reads && To.Writes)
          Kind = MachineMemoryDependencyKind::Anti;
        else if (From.Writes && To.Writes)
          Kind = MachineMemoryDependencyKind::Output;
        ResultValue.Dependencies.push_back(
            {From.Instruction->stableID(), To.Instruction->stableID(), Kind});
      }
    }

    const std::size_t RegionEnd = ResultValue.Dependencies.size();
    ResultValue.BlockRanges.emplace(Block, Result::Range{RegionBegin, RegionEnd - RegionBegin});
    for (std::size_t Begin = RegionBegin; Begin < RegionEnd;) {
      std::size_t End = Begin + 1;
      while (End < RegionEnd && ResultValue.Dependencies[End].Destination ==
                                    ResultValue.Dependencies[Begin].Destination) {
        ++End;
      }
      ResultValue.DestinationRanges.emplace(ResultValue.Dependencies[Begin].Destination,
                                            Result::Range{Begin, End - Begin});
      Begin = End;
    }
  }
  return ResultValue;
}

Expected<void> registerStage8MachineAnalyses(MachineFunctionAnalysisManager &Analyses) {
  const std::array IDs{
      MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>(),
      MachineFunctionAnalysisManager::id<MachineLoopAnalysis>(),
      MachineFunctionAnalysisManager::id<MachineBranchProbabilityAnalysis>(),
      MachineFunctionAnalysisManager::id<MachineBlockFrequencyAnalysis>(),
      MachineFunctionAnalysisManager::id<MachineAliasAnalysis>(),
      MachineFunctionAnalysisManager::id<MachineMemoryDependenceAnalysis>(),
  };
  const std::size_t Registered = static_cast<std::size_t>(
      std::ranges::count_if(IDs, [&Analyses](AnalysisID ID) { return Analyses.isRegistered(ID); }));
  if (Registered == IDs.size())
    return {};
  if (Registered != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "standard machine analyses are only partially registered");
  }

  auto Candidate = Analyses.forkEmpty();
  if (!Candidate)
    return std::unexpected(Candidate.error());
  MachineFunctionAnalysisManager &Staging = **Candidate;

  auto Dominators = Staging.registerAnalysis<MachineDominatorTreeAnalysis>({
      .Name = "MachineDominatorTree",
      .Dependencies = {},
      .InvalidatedBy = {MachinePassChange::CFG, MachinePassChange::BlockLayout},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Dominators)
    return std::unexpected(Dominators.error());
  auto Loops = Staging.registerAnalysis<MachineLoopAnalysis>({
      .Name = "MachineLoopInfo",
      .Dependencies = {MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>()},
      .InvalidatedBy = {MachinePassChange::CFG, MachinePassChange::BlockLayout},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Loops)
    return std::unexpected(Loops.error());
  auto Probabilities = Staging.registerAnalysis<MachineBranchProbabilityAnalysis>({
      .Name = "MachineBranchProbability",
      .Dependencies = {},
      .InvalidatedBy = {MachinePassChange::CFG},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Probabilities)
    return std::unexpected(Probabilities.error());
  auto Frequencies = Staging.registerAnalysis<MachineBlockFrequencyAnalysis>({
      .Name = "MachineBlockFrequency",
      .Dependencies = {MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                       MachineFunctionAnalysisManager::id<MachineLoopAnalysis>(),
                       MachineFunctionAnalysisManager::id<MachineBranchProbabilityAnalysis>()},
      .InvalidatedBy = {MachinePassChange::CFG, MachinePassChange::BlockLayout},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Frequencies)
    return std::unexpected(Frequencies.error());
  auto Alias = Staging.registerAnalysis<MachineAliasAnalysis>({
      .Name = "MachineAliasAnalysis",
      .Dependencies = {},
      .InvalidatedBy = {MachinePassChange::Operands, MachinePassChange::Frame,
                        MachinePassChange::ConstantPool, MachinePassChange::JumpTable,
                        MachinePassChange::TargetState},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!Alias)
    return std::unexpected(Alias.error());
  auto MemoryDependence = Staging.registerAnalysis<MachineMemoryDependenceAnalysis>({
      .Name = "MachineMemoryDependence",
      .Dependencies = {MachineFunctionAnalysisManager::id<MachineAliasAnalysis>()},
      .InvalidatedBy = {MachinePassChange::Instructions, MachinePassChange::Operands,
                        MachinePassChange::CFG, MachinePassChange::BlockLayout,
                        MachinePassChange::Frame, MachinePassChange::ConstantPool,
                        MachinePassChange::JumpTable, MachinePassChange::TargetState},
      .ThreadSafety = PassThreadSafety::ThreadSafe,
  });
  if (!MemoryDependence)
    return std::unexpected(MemoryDependence.error());
  return Analyses.replaceRegistrations(std::move(Staging));
}

} // namespace rcc
