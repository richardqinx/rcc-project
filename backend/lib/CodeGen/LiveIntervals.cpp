// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveIntervals.h"

#include "rcc/CodeGen/LiveRegUnits.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <vector>

namespace rcc {

namespace {
[[nodiscard]] LaneMask subtractLanes(LaneMask Left, LaneMask Right) noexcept {
  return LaneMask::fromRaw(Left.rawValue() & ~Right.rawValue());
}

[[nodiscard]] bool maskIntersects(LaneMask Access, std::optional<LaneMask> Filter) noexcept {
  return !Filter || Access.intersects(*Filter);
}

struct InstructionRangeEvent final {
  bool HasUse = false;
  bool HasNormalDef = false;
  bool HasEarlyDef = false;
  bool HasRegMaskClobber = false;
};

[[nodiscard]] Expected<InstructionRangeEvent>
virtualRangeEvent(const MachineInstr &Instruction, Register Reg, std::optional<LaneMask> Filter,
                  const MachineFunction &Function, const TargetDescription &Target) {
  InstructionRangeEvent Event;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *OperandReg = Operand.getIf<Register>();
    if (OperandReg == nullptr || *OperandReg != Reg)
      continue;
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    if (!maskIntersects(*Lanes, Filter))
      continue;
    Event.HasUse |= Operand.isUse() && !Operand.isDebugUse() &&
                    !Operand.registerFlags().has(RegisterOperandFlag::Undef);
    if (Operand.isDef()) {
      if (Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber))
        Event.HasEarlyDef = true;
      else
        Event.HasNormalDef = true;
    }
  }
  return Event;
}

[[nodiscard]] Expected<InstructionRangeEvent> unitRangeEvent(const MachineInstr &Instruction,
                                                             RegisterUnitID Unit,
                                                             std::optional<LaneMask> Filter,
                                                             const TargetDescription &Target) {
  InstructionRangeEvent Event;
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.isRegister()) {
      const Register *Reg = Operand.getIf<Register>();
      if (Reg == nullptr || !Reg->isPhysical())
        continue;
      auto Units = physicalOperandUnits(Operand, Target);
      if (!Units)
        return std::unexpected(Units.error());
      const auto Position = std::ranges::find_if(
          *Units, [Unit](const PhysicalUnitAccess &Access) { return Access.Unit == Unit; });
      if (Position == Units->end() || !maskIntersects(Position->Lanes, Filter))
        continue;
      Event.HasUse |= Operand.isUse() && !Operand.isDebugUse() &&
                      !Operand.registerFlags().has(RegisterOperandFlag::Undef);
      if (Operand.isDef()) {
        if (Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber))
          Event.HasEarlyDef = true;
        else
          Event.HasNormalDef = true;
      }
      continue;
    }
    if (const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
        MaskOperand != nullptr && MaskOperand->Mask) {
      for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
        if (!MaskOperand->Mask->clobbers(Description.Reg))
          continue;
        if (std::ranges::any_of(Description.Units, [Unit, Filter](const RegisterUnitLane &Access) {
              return Access.Unit == Unit && maskIntersects(Access.Lanes, Filter);
            })) {
          Event.HasRegMaskClobber = true;
          break;
        }
      }
      continue;
    }
    if (const auto *MaskOperand = Operand.getIf<LiveOutRegisterMaskOperand>();
        MaskOperand != nullptr && MaskOperand->Mask) {
      for (const PhysicalRegisterDesc &Description : Target.registers().physicalRegisters()) {
        if (!MaskOperand->Mask->preserves(Description.Reg))
          continue;
        if (std::ranges::any_of(Description.Units, [Unit, Filter](const RegisterUnitLane &Access) {
              return Access.Unit == Unit && maskIntersects(Access.Lanes, Filter);
            })) {
          Event.HasUse = true;
          break;
        }
      }
    }
  }
  return Event;
}

template <typename EventFn, typename LiveOutFn>
[[nodiscard]] Expected<LiveRange>
buildLiveRange(MachineFunction &Function, const SlotIndexesAnalysis::Result &Indexes,
               const EventFn &EventForInstruction, const LiveOutFn &IsLiveOut) {
  LiveRange Range;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    std::optional<SlotIndex> End;
    bool SawLaterDefinition = false;
    if (IsLiveOut(*Block))
      End = Indexes.blockEnd(*Block);

    for (MachineInstr *Instruction = Block->lastInstruction(); Instruction != nullptr;
         Instruction = Instruction->previous()) {
      auto Event = EventForInstruction(*Instruction);
      if (!Event)
        return std::unexpected(Event.error());
      if (!Event->HasUse && !Event->HasNormalDef && !Event->HasEarlyDef &&
          !Event->HasRegMaskClobber) {
        continue;
      }

      const SlotIndex InstructionSlot = Indexes.instructionIndex(*Instruction);
      if (!InstructionSlot.isValid()) {
        return makeUnexpected(ErrorCode::InternalError,
                              "live-range construction has no instruction SlotIndex");
      }
      auto AddDefinition = [&](SlotIndex DefSlot, bool IsRegMask) -> Expected<void> {
        const bool HasLiveValue = End && DefSlot < *End;
        if (IsRegMask && !HasLiveValue) {
          End.reset();
          SawLaterDefinition = true;
          return {};
        }
        const LiveValueState State = IsRegMask            ? LiveValueState::Redefined
                                     : HasLiveValue       ? LiveValueState::Normal
                                     : SawLaterDefinition ? LiveValueState::Redefined
                                                          : LiveValueState::Unused;
        auto Value = Range.createValue(DefSlot, State, Instruction->stableID());
        if (!Value)
          return std::unexpected(Value.error());
        const SlotIndex SegmentEnd = HasLiveValue ? *End : InstructionSlot.deadSlot();
        if (DefSlot < SegmentEnd) {
          auto Added = Range.addSegment({DefSlot, SegmentEnd, *Value});
          if (!Added)
            return std::unexpected(Added.error());
        }
        End.reset();
        SawLaterDefinition = true;
        return {};
      };

      if (Event->HasNormalDef) {
        auto Added = AddDefinition(InstructionSlot.registerSlot(), false);
        if (!Added)
          return std::unexpected(Added.error());
      }
      if (Event->HasUse) {
        const SlotIndex UseEnd =
            Event->HasNormalDef ? InstructionSlot.registerSlot() : InstructionSlot.deadSlot();
        if (!End || *End < UseEnd)
          End = UseEnd;
      }
      if (Event->HasEarlyDef) {
        auto Added = AddDefinition(InstructionSlot.earlyClobberSlot(), false);
        if (!Added)
          return std::unexpected(Added.error());
      } else if (Event->HasRegMaskClobber) {
        auto Added = AddDefinition(InstructionSlot.earlyClobberSlot(), true);
        if (!Added)
          return std::unexpected(Added.error());
      }
    }

    if (End) {
      const SlotIndex Start = Indexes.blockStart(*Block);
      if (!Start.isValid() || !(*End > Start)) {
        return makeUnexpected(ErrorCode::InternalError,
                              "live-in range has invalid block boundary indexes");
      }
      auto Value = Range.createValue(Start, LiveValueState::Phi);
      if (!Value)
        return std::unexpected(Value.error());
      auto Added = Range.addSegment({Start, *End, *Value});
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  auto Verification = Range.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Range;
}

[[nodiscard]] Expected<LaneMask> virtualRegisterFullLanes(Register Reg,
                                                          const MachineFunction &Function,
                                                          const TargetDescription &Target) {
  return virtualRegisterLanes(Reg, Function, Target);
}

[[nodiscard]] Expected<std::vector<LaneMask>>
partitionVirtualRegisterLanes(Register Reg, MachineFunction &Function,
                              const TargetDescription &Target) {
  auto Full = virtualRegisterFullLanes(Reg, Function, Target);
  if (!Full)
    return std::unexpected(Full.error());
  std::vector<LaneMask> AccessMasks;
  for (const MachineOperand &Operand : Function.registerInfo().regNoDebugOperands(Reg)) {
    auto Lanes = virtualOperandLanes(Operand, Function, Target);
    if (!Lanes)
      return std::unexpected(Lanes.error());
    if (*Lanes != *Full && std::ranges::find(AccessMasks, *Lanes) == AccessMasks.end())
      AccessMasks.push_back(*Lanes);
  }
  if (AccessMasks.empty())
    return {};

  std::vector<LaneMask> Groups{*Full};
  for (const LaneMask Access : AccessMasks) {
    std::vector<LaneMask> Refined;
    for (const LaneMask Group : Groups) {
      const LaneMask Intersection = Group & Access;
      const LaneMask Difference = subtractLanes(Group, Access);
      if (!Intersection.empty())
        Refined.push_back(Intersection);
      if (!Difference.empty())
        Refined.push_back(Difference);
    }
    Groups = std::move(Refined);
  }
  std::ranges::sort(
      Groups, [](LaneMask Left, LaneMask Right) { return Left.rawValue() < Right.rawValue(); });
  return Groups;
}
} // namespace

const LiveInterval *LiveIntervalsAnalysis::Result::interval(Register Reg) const noexcept {
  const auto Position = Intervals.find(Reg);
  return Position == Intervals.end() ? nullptr : &Position->second;
}

LiveInterval *LiveIntervalsAnalysis::Result::interval(Register Reg) noexcept {
  const auto Position = Intervals.find(Reg);
  return Position == Intervals.end() ? nullptr : &Position->second;
}

const LiveRange *LiveIntervalsAnalysis::Result::regUnitRange(RegisterUnitID Unit) const noexcept {
  const auto Position = RegisterUnitRanges.find(Unit);
  return Position == RegisterUnitRanges.end() ? nullptr : &Position->second;
}

bool LiveIntervalsAnalysis::Result::interferes(Register Left, Register Right) const noexcept {
  const LiveInterval *LeftInterval = interval(Left);
  const LiveInterval *RightInterval = interval(Right);
  return LeftInterval != nullptr && RightInterval != nullptr &&
         LeftInterval->overlaps(*RightInterval);
}

bool LiveIntervalsAnalysis::Result::regMaskClobbers(const LiveInterval &Interval,
                                                    PhysicalRegister Candidate) const noexcept {
  for (const RegMaskPosition &Position : RegMasks) {
    if (Position.Mask && Position.Mask->clobbers(Candidate) &&
        Interval.MainRange.liveAt(Position.Position)) {
      return true;
    }
  }
  return false;
}

Expected<void> LiveIntervalsAnalysis::Result::shrinkToUses(Register Reg,
                                                           std::span<const SlotIndex> Uses) {
  LiveInterval *Value = interval(Reg);
  if (Value == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register has no live interval");
  auto Main = Value->MainRange.shrinkToUses(Uses);
  if (!Main)
    return std::unexpected(Main.error());
  for (LiveSubRange &SubRange : Value->SubRanges) {
    auto Shrunk = SubRange.Range.shrinkToUses(Uses);
    if (!Shrunk)
      return std::unexpected(Shrunk.error());
  }
  return {};
}

bool LiveIntervalsAnalysis::Result::invalidate(const MachineFunction &,
                                               const PreservedAnalyses &Preserved) const noexcept {
  return !Preserved.preserves(MachineAnalysisManager::id<LiveIntervalsAnalysis>());
}

Expected<LiveIntervalsAnalysis::Result>
LiveIntervalsAnalysis::run(MachineFunction &Function, const TargetDescription &Target,
                           MachineAnalysisManager &Analyses) {
  auto Indexes = Analyses.getResult<SlotIndexesAnalysis>(Function, Target);
  if (!Indexes)
    return std::unexpected(Indexes.error());
  auto Variables = Analyses.getResult<LiveVariablesAnalysis>(Function, Target);
  if (!Variables)
    return std::unexpected(Variables.error());

  Result ResultValue;
  ResultValue.Indexes = **Indexes;
  for (std::size_t Index = 0; Index < Function.registerInfo().virtualRegisters().size(); ++Index) {
    auto RegValue = Register::makeVirtual(Index);
    if (!RegValue || !Function.registerInfo().isKnownVirtualRegister(*RegValue))
      continue;
    const Register Reg = *RegValue;
    auto MainRange = buildLiveRange(
        Function, ResultValue.Indexes,
        [&Function, &Target, Reg](const MachineInstr &Instruction) {
          return virtualRangeEvent(Instruction, Reg, std::nullopt, Function, Target);
        },
        [&Variables, Reg](const MachineBasicBlock &Block) {
          const MachineBlockLiveness *Value = (*Variables)->block(Block);
          return Value != nullptr && Value->VirtualLiveOut.intersects(Reg);
        });
    if (!MainRange)
      return std::unexpected(MainRange.error());
    if (MainRange->empty())
      continue;

    LiveInterval Interval;
    Interval.Reg = Reg;
    Interval.MainRange = std::move(*MainRange);
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    // Preserve the target-provided hint order.  MachineRegisterInfo already de-duplicates
    // hints, and their order is the deterministic preference order consumed by allocators.
    Interval.AllocationHints = Info->get().AllocationHints;

    std::uint64_t Weight = 1;
    for (const MachineOperand &Operand : Function.registerInfo().useOperands(Reg, false)) {
      if (Operand.parent() == nullptr || Operand.registerFlags().has(RegisterOperandFlag::Undef)) {
        continue;
      }
      Weight = Weight > std::numeric_limits<std::uint64_t>::max() - 10U
                   ? std::numeric_limits<std::uint64_t>::max()
                   : Weight + 10U;
    }
    Interval.SpillWeight = Weight;

    auto LaneGroups = partitionVirtualRegisterLanes(Reg, Function, Target);
    if (!LaneGroups)
      return std::unexpected(LaneGroups.error());
    for (const LaneMask Lanes : *LaneGroups) {
      auto SubRange = buildLiveRange(
          Function, ResultValue.Indexes,
          [&Function, &Target, Reg, Lanes](const MachineInstr &Instruction) {
            return virtualRangeEvent(Instruction, Reg, Lanes, Function, Target);
          },
          [&Variables, Reg, Lanes](const MachineBasicBlock &Block) {
            const MachineBlockLiveness *Value = (*Variables)->block(Block);
            return Value != nullptr && Value->VirtualLiveOut.intersects(Reg, Lanes);
          });
      if (!SubRange)
        return std::unexpected(SubRange.error());
      if (!SubRange->empty())
        Interval.SubRanges.push_back({Lanes, std::move(*SubRange)});
    }
    ResultValue.Intervals.emplace(Reg, std::move(Interval));
  }

  for (const RegisterUnitDesc &Unit : Target.registers().registerUnits()) {
    auto Range = buildLiveRange(
        Function, ResultValue.Indexes,
        [&Target, Unit](const MachineInstr &Instruction) {
          return unitRangeEvent(Instruction, Unit.ID, std::nullopt, Target);
        },
        [&Variables, Unit](const MachineBasicBlock &Block) {
          const MachineBlockLiveness *Value = (*Variables)->block(Block);
          return Value != nullptr && Value->PhysicalLiveOut.intersects(Unit.ID);
        });
    if (!Range)
      return std::unexpected(Range.error());
    if (!Range->empty())
      ResultValue.RegisterUnitRanges.emplace(Unit.ID, std::move(*Range));
  }

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        const auto *MaskOperand = Operand.getIf<RegisterMaskOperand>();
        if (MaskOperand == nullptr)
          continue;
        if (!MaskOperand->Mask)
          return makeUnexpected(ErrorCode::VerificationError,
                                "live intervals encountered a null register mask");
        ResultValue.RegMasks.push_back(
            {ResultValue.Indexes.instructionIndex(*Instruction).earlyClobberSlot(),
             Instruction->stableID(), MaskOperand->Mask});
      }
    }
  }
  return ResultValue;
}

} // namespace rcc
