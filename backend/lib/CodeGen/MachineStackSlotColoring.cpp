// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineStackSlotColoring.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &stackSlotColoringContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-stack-slot-coloring",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Operands, MachinePassChange::Frame};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

struct SlotInterval final {
  MachineBasicBlock *Block = nullptr;
  std::size_t First = 0;
  std::size_t Last = 0;
  bool FirstStores = false;
  bool LastLoads = false;
  bool IsValid = true;
};

struct InstructionReference final {
  MachineInstr *Instruction = nullptr;
  std::size_t Position = 0;
  bool Loads = false;
  bool Stores = false;
  bool HasMemoryReference = false;
  bool HasFrameOperand = false;
};

struct SlotUseInfo final {
  FrameIndex Slot;
  SlotInterval Interval;
  std::vector<InstructionReference> References;
};

[[nodiscard]] InstructionReference &getReference(SlotUseInfo &Info, MachineInstr &Instruction,
                                                 std::size_t Position) {
  const auto Existing =
      std::ranges::find_if(Info.References, [&Instruction](const InstructionReference &Reference) {
        return Reference.Instruction == &Instruction;
      });
  if (Existing != Info.References.end())
    return *Existing;
  Info.References.push_back({.Instruction = &Instruction, .Position = Position});
  return Info.References.back();
}

[[nodiscard]] std::unordered_map<FrameIndex, SlotUseInfo>
collectSlotUses(MachineFunction &Function) {
  std::unordered_map<FrameIndex, SlotUseInfo> Result;
  std::unordered_map<MachineInstrStableID, std::pair<MachineBasicBlock *, std::size_t>> Locations;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    std::size_t Position = 0;
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next(), ++Position) {
      Locations.emplace(Instruction->stableID(), std::pair{Block, Position});
      for (const MachineOperand &Operand : Instruction->operands()) {
        const FrameIndexOperand *Frame = Operand.getIf<FrameIndexOperand>();
        if (Frame == nullptr)
          continue;
        SlotUseInfo &Info =
            Result
                .try_emplace(Frame->Index,
                             SlotUseInfo{.Slot = Frame->Index, .Interval = {}, .References = {}})
                .first->second;
        getReference(Info, *Instruction, Position).HasFrameOperand = true;
      }
      for (const MachineMemOperand *Memory : Instruction->memOperands()) {
        if (Memory == nullptr)
          continue;
        const std::optional<FrameIndex> Frame = Memory->description().Base.frameIndex();
        if (!Frame)
          continue;
        SlotUseInfo &Info =
            Result
                .try_emplace(*Frame, SlotUseInfo{.Slot = *Frame, .Interval = {}, .References = {}})
                .first->second;
        InstructionReference &Reference = getReference(Info, *Instruction, Position);
        Reference.HasMemoryReference = true;
        Reference.Loads |= Memory->isLoad();
        Reference.Stores |= Memory->isStore();
      }
    }
  }

  for (auto &[Slot, Info] : Result) {
    (void)Slot;
    if (Info.References.empty()) {
      Info.Interval.IsValid = false;
      continue;
    }
    Info.Interval.Block = Info.References.front().Instruction->parent();
    Info.Interval.First = Info.References.front().Position;
    Info.Interval.Last = Info.References.back().Position;
    Info.Interval.FirstStores = Info.References.front().Stores;
    Info.Interval.LastLoads = Info.References.back().Loads;
    for (const InstructionReference &Reference : Info.References) {
      if (Reference.Instruction->parent() != Info.Interval.Block || !Reference.HasMemoryReference ||
          (Reference.HasFrameOperand && !Reference.HasMemoryReference)) {
        Info.Interval.IsValid = false;
      }
    }
    Info.Interval.IsValid &=
        Info.Interval.Block != nullptr && Info.Interval.FirstStores && Info.Interval.LastLoads;
    const StackObject *Object = Function.frameInfo().object(Info.Slot);
    const StackObjectDesc *Description = Object == nullptr ? nullptr : Object->description();
    if (Description == nullptr) {
      Info.Interval.IsValid = false;
      continue;
    }
    for (const StackObjectLifetimeRange &Range : Description->Lifetime) {
      const auto Begin = Locations.find(Range.Begin);
      const auto End = Locations.find(Range.End);
      if (Begin == Locations.end() || End == Locations.end() ||
          Begin->second.first != Info.Interval.Block || End->second.first != Info.Interval.Block ||
          Begin->second.second > End->second.second) {
        Info.Interval.IsValid = false;
        break;
      }
      Info.Interval.First = std::min(Info.Interval.First, Begin->second.second);
      Info.Interval.Last = std::max(Info.Interval.Last, End->second.second);
    }
  }
  return Result;
}

[[nodiscard]] bool isEligibleSlot(const StackObjectDesc &Description) noexcept {
  return Description.Kind == StackObjectKind::SpillSlot && !Description.IsDead &&
         !Description.IsImmutable && !Description.IsAliased && !Description.AddressTaken &&
         !Description.AllocaName && !Description.Offset && !Description.LocalOffset &&
         !Description.CalleeSavedRegister && !Description.DebugInfo &&
         Description.TargetFlags == 0 && Description.Size.has_value();
}

[[nodiscard]] bool compatibleSlots(const StackObjectDesc &Left,
                                   const StackObjectDesc &Right) noexcept {
  return isEligibleSlot(Left) && isEligibleSlot(Right) && Left.Kind == Right.Kind &&
         Left.Size == Right.Size && Left.Align == Right.Align && Left.Stack == Right.Stack;
}

[[nodiscard]] bool intervalsOverlap(const SlotInterval &Left, const SlotInterval &Right) noexcept {
  if (Left.Block != Right.Block)
    return true;
  return !(Left.Last < Right.First || Right.Last < Left.First);
}

struct StackColor final {
  FrameIndex Canonical;
  const StackObjectDesc *Description = nullptr;
  std::vector<SlotInterval> Intervals;
};

[[nodiscard]] Expected<void> rewriteSlot(MachineFunction &Function, FrameIndex OldSlot,
                                         FrameIndex NewSlot) {
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        MachineOperand *Operand = Instruction->operandAt(Index);
        const FrameIndexOperand *Frame =
            Operand == nullptr ? nullptr : Operand->getIf<FrameIndexOperand>();
        if (Frame == nullptr || Frame->Index != OldSlot)
          continue;
        auto Rewritten = Operand->setFrameIndex(NewSlot);
        if (!Rewritten)
          return std::unexpected(Rewritten.error());
      }
      for (std::size_t Index = 0; Index < Instruction->memOperands().size(); ++Index) {
        const MachineMemOperand *Memory = Instruction->memOperands()[Index];
        if (Memory == nullptr || Memory->description().Base.frameIndex() != OldSlot)
          continue;
        MachineMemOperandDesc Description = Memory->description();
        Description.Base = MemoryBase::frameIndex(NewSlot);
        auto Replacement = Function.createMemOperand(std::move(Description));
        if (!Replacement)
          return std::unexpected(Replacement.error());
        auto Replaced = Instruction->replaceMemOperand(Index, **Replacement);
        if (!Replaced)
          return std::unexpected(Replaced.error());
      }
    }
  }
  return {};
}

} // namespace

const MachinePassContract &MachineStackSlotColoringPass::contract() const noexcept {
  return stackSlotColoringContract();
}

Expected<MachinePassResult> MachineStackSlotColoringPass::run(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "stack-slot coloring requires allocated PHI-free MachineIR");
  }
  if (Function.frameInfo().isFrozen()) {
    return makeUnexpected(ErrorCode::FrameLayoutFailed,
                          "stack-slot coloring must run before frame layout is frozen");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        prefixedError(Trial.error(), "stack-slot coloring transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = runImplementation(Function);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "stack-slot coloring diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachineStackSlotColoringPass::runImplementation(MachineFunction &Function) const {
  std::unordered_map<FrameIndex, SlotUseInfo> Uses = collectSlotUses(Function);
  std::vector<StackColor> Colors;
  std::vector<std::pair<FrameIndex, FrameIndex>> Rewrites;
  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    const auto Use = Uses.find(Object.index());
    if (Description == nullptr || Use == Uses.end() || !Use->second.Interval.IsValid ||
        !isEligibleSlot(*Description)) {
      continue;
    }
    auto Color = std::ranges::find_if(Colors, [&](const StackColor &Candidate) {
      return Candidate.Description != nullptr &&
             compatibleSlots(*Candidate.Description, *Description) &&
             std::ranges::none_of(Candidate.Intervals, [&](const SlotInterval &Interval) {
               return intervalsOverlap(Interval, Use->second.Interval);
             });
    });
    if (Color == Colors.end()) {
      Colors.push_back({Object.index(), Description, {Use->second.Interval}});
      continue;
    }
    if (Rewrites.size() == Options.MaximumColoredSlots)
      break;
    Rewrites.emplace_back(Object.index(), Color->Canonical);
    Color->Intervals.push_back(Use->second.Interval);
  }

  for (const auto &[OldSlot, NewSlot] : Rewrites) {
    const StackObject *OldObject = Function.frameInfo().object(OldSlot);
    const StackObject *NewObject = Function.frameInfo().object(NewSlot);
    if (OldObject == nullptr || NewObject == nullptr || OldObject->description() == nullptr ||
        NewObject->description() == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "stack-slot coloring lost a planned stack object");
    }
    auto Rewritten = rewriteSlot(Function, OldSlot, NewSlot);
    if (!Rewritten)
      return std::unexpected(Rewritten.error());
    StackObjectDesc Combined = *NewObject->description();
    Combined.Lifetime.insert(Combined.Lifetime.end(), OldObject->description()->Lifetime.begin(),
                             OldObject->description()->Lifetime.end());
    auto Updated = Function.frameInfo().updateObject(NewSlot, std::move(Combined));
    if (!Updated)
      return std::unexpected(Updated.error());
    auto Dead = Function.frameInfo().markObjectDead(OldSlot);
    if (!Dead)
      return std::unexpected(Dead.error());
  }

  MachinePassResult Result;
  Result.Changed = !Rewrites.empty();
  if (Result.Changed) {
    Result.Changes = {MachinePassChange::Operands, MachinePassChange::Frame};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
