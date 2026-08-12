// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/SlotIndexes.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"

#include <cstdint>
#include <limits>

namespace rcc {

namespace {
constexpr std::uint64_t SlotSpacing = 16;
constexpr std::uint64_t SlotAlignmentMask = 3;
} // namespace

SlotIndex
SlotIndexesAnalysis::Result::instructionIndex(const MachineInstr &Instruction) const noexcept {
  const auto Position = InstructionSlots.find(Instruction.stableID());
  return Position == InstructionSlots.end() ? SlotIndex{} : Position->second;
}

SlotIndex SlotIndexesAnalysis::Result::blockStart(const MachineBasicBlock &Block) const noexcept {
  const auto Position = BlockStarts.find(&Block);
  return Position == BlockStarts.end() ? SlotIndex{} : Position->second;
}

SlotIndex SlotIndexesAnalysis::Result::blockEnd(const MachineBasicBlock &Block) const noexcept {
  const auto Position = BlockEnds.find(&Block);
  return Position == BlockEnds.end() ? SlotIndex{} : Position->second;
}

const MachineInstr *
SlotIndexesAnalysis::Result::instructionFromIndex(SlotIndex Index) const noexcept {
  if (!Index.isValid())
    return nullptr;
  const auto Position = SlotToInstruction.find(Index.baseIndex().value());
  return Position == SlotToInstruction.end() ? nullptr : Position->second;
}

const MachineBasicBlock *
SlotIndexesAnalysis::Result::blockFromIndex(SlotIndex Index) const noexcept {
  if (!Index.isValid())
    return nullptr;
  for (const MachineBasicBlock *Block : BlocksByStart) {
    const SlotIndex Start = blockStart(*Block);
    const SlotIndex End = blockEnd(*Block);
    if (!(Index < Start) && Index < End)
      return Block;
  }
  return nullptr;
}

Expected<void> SlotIndexesAnalysis::Result::insertInstruction(const MachineInstr &Instruction) {
  const MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr || !BlockStarts.contains(Block) || !BlockEnds.contains(Block)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "SlotIndex insertion requires a linked indexed instruction");
  }
  if (InstructionSlots.contains(Instruction.stableID())) {
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction already has a SlotIndex");
  }

  const MachineInstr *Previous = Instruction.previous();
  const MachineInstr *Next = Instruction.next();
  const SlotIndex LowerIndex =
      Previous == nullptr ? blockStart(*Block) : instructionIndex(*Previous);
  const SlotIndex UpperIndex = Next == nullptr ? blockEnd(*Block) : instructionIndex(*Next);
  if (!LowerIndex.isValid() || !UpperIndex.isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "adjacent instructions are missing SlotIndexes");
  }
  const std::uint64_t Lower = LowerIndex.baseIndex().value();
  const std::uint64_t Upper = UpperIndex.baseIndex().value();
  if (Upper <= Lower || Upper - Lower <= SlotAlignmentMask + 1U) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "local SlotIndex space is exhausted; renumbering is required");
  }
  std::uint64_t Candidate = ((Lower + Upper) / 2U) & ~SlotAlignmentMask;
  if (Candidate <= Lower)
    Candidate = Lower + SlotAlignmentMask + 1U;
  if (Candidate >= Upper) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "local SlotIndex space is exhausted; renumbering is required");
  }
  const SlotIndex Index = SlotIndex::fromValue(Candidate).registerSlot();
  InstructionSlots.emplace(Instruction.stableID(), Index);
  SlotToInstruction.emplace(Index.baseIndex().value(), &Instruction);
  return {};
}

Expected<void> SlotIndexesAnalysis::Result::eraseInstruction(const MachineInstr &Instruction) {
  const auto Position = InstructionSlots.find(Instruction.stableID());
  if (Position == InstructionSlots.end())
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction has no SlotIndex");
  SlotToInstruction.erase(Position->second.baseIndex().value());
  InstructionSlots.erase(Position);
  return {};
}

Expected<void> SlotIndexesAnalysis::Result::renumber(MachineFunction &Function) {
  Result NewResult;
  std::uint64_t Position = SlotSpacing;
  auto Advance = [&Position]() -> Expected<void> {
    if (Position > std::numeric_limits<std::uint64_t>::max() - SlotSpacing) {
      return makeUnexpected(ErrorCode::Unsupported, "machine SlotIndex space is exhausted");
    }
    Position += SlotSpacing;
    return {};
  };

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    const SlotIndex Start = SlotIndex::fromValue(Position).withKind(SlotIndexKind::Block);
    NewResult.BlockStarts.emplace(Block, Start);
    NewResult.BlocksByStart.push_back(Block);
    auto Advanced = Advance();
    if (!Advanced)
      return std::unexpected(Advanced.error());

    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const SlotIndex Index = SlotIndex::fromValue(Position).registerSlot();
      if (!NewResult.InstructionSlots.emplace(Instruction->stableID(), Index).second ||
          !NewResult.SlotToInstruction.emplace(Index.baseIndex().value(), Instruction).second) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "duplicate instruction identity during SlotIndex construction");
      }
      Advanced = Advance();
      if (!Advanced)
        return std::unexpected(Advanced.error());
    }
    NewResult.BlockEnds.emplace(Block,
                                SlotIndex::fromValue(Position).withKind(SlotIndexKind::Block));
    Advanced = Advance();
    if (!Advanced)
      return std::unexpected(Advanced.error());
  }
  *this = std::move(NewResult);
  return {};
}

bool SlotIndexesAnalysis::Result::invalidate(const MachineFunction &,
                                             const PreservedAnalyses &Preserved) const noexcept {
  return !Preserved.preserves(MachineFunctionAnalysisManager::id<SlotIndexesAnalysis>());
}

Expected<SlotIndexesAnalysis::Result> SlotIndexesAnalysis::run(MachineFunction &Function,
                                                               const TargetDescription &,
                                                               MachineFunctionAnalysisManager &) {
  Result ResultValue;
  auto Numbered = ResultValue.renumber(Function);
  if (!Numbered)
    return std::unexpected(Numbered.error());
  return ResultValue;
}

} // namespace rcc
