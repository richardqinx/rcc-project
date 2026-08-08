// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveRegMatrix.h"

#include "RegAllocInternal.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::optional<LaneMask> relevantLanes(const LiveInterval &Interval,
                                                    RegisterUnitLane Unit) noexcept {
  if (Interval.SubRanges.empty())
    return Unit.Lanes;
  LaneMask Result;
  for (const LiveSubRange &SubRange : Interval.SubRanges) {
    if (SubRange.Lanes.intersects(Unit.Lanes))
      Result = Result | (SubRange.Lanes & Unit.Lanes);
  }
  if (Result.empty())
    return std::nullopt;
  return Result;
}

[[nodiscard]] LaneMask normalizeUnitLanes(LaneMask UnitViewLanes, LaneMask RegisterLanes) noexcept {
  std::uint64_t View = UnitViewLanes.rawValue();
  const std::uint64_t Selected = View & RegisterLanes.rawValue();
  std::uint64_t Normalized = 0;
  std::uint32_t UnitLane = 0;
  while (View != 0) {
    const std::uint64_t RegisterLane = std::uint64_t{1} << std::countr_zero(View);
    if ((Selected & RegisterLane) != 0)
      Normalized |= std::uint64_t{1} << UnitLane;
    View &= View - 1U;
    ++UnitLane;
  }
  return LaneMask::fromRaw(Normalized);
}

[[nodiscard]] LaneMask intervalUnitLanes(const LiveInterval &Interval,
                                         RegisterUnitLane Unit) noexcept {
  if (Interval.SubRanges.empty())
    return normalizeUnitLanes(Unit.Lanes, Unit.Lanes);
  LaneMask Result;
  for (const LiveSubRange &SubRange : Interval.SubRanges)
    Result = Result | normalizeUnitLanes(Unit.Lanes, SubRange.Lanes);
  return Result;
}

[[nodiscard]] bool intervalLiveAtFixedRegister(const LiveInterval &Interval,
                                               const PhysicalRegisterDesc &Candidate,
                                               const PhysicalRegisterDesc &Fixed,
                                               SlotIndex Position) noexcept {
  for (const RegisterUnitLane CandidateUnit : Candidate.Units) {
    const auto FixedUnit =
        std::ranges::find(Fixed.Units, CandidateUnit.Unit, &RegisterUnitLane::Unit);
    if (FixedUnit == Fixed.Units.end())
      continue;
    const LaneMask FixedLanes = normalizeUnitLanes(FixedUnit->Lanes, FixedUnit->Lanes);
    if (Interval.SubRanges.empty()) {
      if (normalizeUnitLanes(CandidateUnit.Lanes, CandidateUnit.Lanes).intersects(FixedLanes) &&
          Interval.MainRange.liveAt(Position)) {
        return true;
      }
      continue;
    }
    for (const LiveSubRange &SubRange : Interval.SubRanges) {
      if (normalizeUnitLanes(CandidateUnit.Lanes, SubRange.Lanes).intersects(FixedLanes) &&
          SubRange.Range.liveAt(Position)) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool rangeForUnitOverlaps(const LiveInterval &Interval, RegisterUnitLane Unit,
                                        const LiveRange &Other) noexcept {
  if (!relevantLanes(Interval, Unit))
    return false;
  if (Interval.SubRanges.empty())
    return Interval.MainRange.overlaps(Other);
  for (const LiveSubRange &SubRange : Interval.SubRanges) {
    if (SubRange.Lanes.intersects(Unit.Lanes) && SubRange.Range.overlaps(Other))
      return true;
  }
  return false;
}

} // namespace

bool regalloc_detail::intervalsOverlapOnPhysicalRegisters(
    const LiveInterval &Left, PhysicalRegister LeftReg, const LiveInterval &Right,
    PhysicalRegister RightReg, const TargetRegisterInfo &Registers) noexcept {
  const PhysicalRegisterDesc *LeftDescription = Registers.physicalRegister(LeftReg);
  const PhysicalRegisterDesc *RightDescription = Registers.physicalRegister(RightReg);
  if (LeftDescription == nullptr || RightDescription == nullptr)
    return true;
  for (const RegisterUnitLane LeftUnit : LeftDescription->Units) {
    for (const RegisterUnitLane RightUnit : RightDescription->Units) {
      if (LeftUnit.Unit != RightUnit.Unit)
        continue;
      const LaneMask LeftUnitLanes = intervalUnitLanes(Left, LeftUnit);
      const LaneMask RightUnitLanes = intervalUnitLanes(Right, RightUnit);
      if (!LeftUnitLanes.intersects(RightUnitLanes))
        continue;
      if (Left.SubRanges.empty() && Right.SubRanges.empty())
        return Left.MainRange.overlaps(Right.MainRange);
      const auto LeftRanges = Left.SubRanges.empty()
                                  ? std::span<const LiveSubRange>{}
                                  : std::span<const LiveSubRange>(Left.SubRanges);
      const auto RightRanges = Right.SubRanges.empty()
                                   ? std::span<const LiveSubRange>{}
                                   : std::span<const LiveSubRange>(Right.SubRanges);
      if (LeftRanges.empty()) {
        for (const LiveSubRange &RightRange : RightRanges) {
          const LaneMask RightRangeLanes = normalizeUnitLanes(RightUnit.Lanes, RightRange.Lanes);
          if (LeftUnitLanes.intersects(RightRangeLanes) &&
              Left.MainRange.overlaps(RightRange.Range))
            return true;
        }
        continue;
      }
      if (RightRanges.empty()) {
        for (const LiveSubRange &LeftRange : LeftRanges) {
          const LaneMask LeftRangeLanes = normalizeUnitLanes(LeftUnit.Lanes, LeftRange.Lanes);
          if (LeftRangeLanes.intersects(RightUnitLanes) &&
              LeftRange.Range.overlaps(Right.MainRange))
            return true;
        }
        continue;
      }
      for (const LiveSubRange &LeftRange : LeftRanges) {
        const LaneMask LeftRangeLanes = normalizeUnitLanes(LeftUnit.Lanes, LeftRange.Lanes);
        if (LeftRangeLanes.empty())
          continue;
        for (const LiveSubRange &RightRange : RightRanges) {
          const LaneMask RightRangeLanes = normalizeUnitLanes(RightUnit.Lanes, RightRange.Lanes);
          if (LeftRangeLanes.intersects(RightRangeLanes) &&
              LeftRange.Range.overlaps(RightRange.Range))
            return true;
        }
      }
    }
  }
  return false;
}

bool LiveRegMatrix::intervalOverlapsUnit(const LiveInterval &Interval, RegisterUnitLane Unit,
                                         const LiveRange &UnitRange) const noexcept {
  return rangeForUnitOverlaps(Interval, Unit, UnitRange);
}

bool LiveRegMatrix::intervalsOverlapOnCandidates(const LiveInterval &Left, PhysicalRegister LeftReg,
                                                 const LiveInterval &Right,
                                                 PhysicalRegister RightReg) const noexcept {
  return regalloc_detail::intervalsOverlapOnPhysicalRegisters(Left, LeftReg, Right, RightReg,
                                                              Target->registers());
}

std::vector<RegisterInterference>
LiveRegMatrix::collectInterferences(Register VirtualReg, PhysicalRegister PhysicalReg) const {
  std::vector<RegisterInterference> Result;
  const LiveInterval *Interval = Intervals->interval(VirtualReg);
  const PhysicalRegisterDesc *Candidate = Target->registers().physicalRegister(PhysicalReg);
  if (Interval == nullptr || Candidate == nullptr)
    return Result;

  for (const MachineBasicBlock *Block = Function->firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const SlotIndex Position = Intervals->slot(*Instruction);
      if (!Position.isValid())
        continue;
      for (const MachineOperand &Operand : Instruction->operands()) {
        const Register *Reg = Operand.getIf<Register>();
        if (Reg == nullptr || !Reg->isPhysical())
          continue;
        const auto Fixed = Reg->physicalRegister();
        if (!Fixed)
          continue;
        const PhysicalRegister FixedReg = Fixed.value();
        if (!Target->registers().regsOverlap(FixedReg, PhysicalReg))
          continue;
        const PhysicalRegisterDesc *FixedDescription =
            Target->registers().physicalRegister(FixedReg);
        if (FixedDescription == nullptr)
          continue;
        const SlotIndex OperandPosition =
            Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber)
                ? Position.earlyClobberSlot()
                : Position.registerSlot();
        if (intervalLiveAtFixedRegister(*Interval, *Candidate, *FixedDescription,
                                        OperandPosition)) {
          Result.push_back(RegisterInterference{RegisterInterferenceKind::FixedRegister,
                                                std::nullopt, FixedReg, std::nullopt,
                                                Instruction->stableID()});
        }
      }
    }
  }

  for (const RegMaskPosition &Mask : Intervals->regMaskPositions()) {
    if (Mask.Mask && Mask.Mask->clobbers(PhysicalReg) &&
        Interval->MainRange.liveAt(Mask.Position)) {
      Result.push_back(RegisterInterference{RegisterInterferenceKind::RegisterMask, std::nullopt,
                                            std::nullopt, std::nullopt, Mask.Instruction});
    }
  }

  for (const RegisterUnitLane Unit : Candidate->Units) {
    const LiveRange *FixedRange = Intervals->regUnitRange(Unit.Unit);
    if (FixedRange != nullptr && intervalOverlapsUnit(*Interval, Unit, *FixedRange)) {
      Result.push_back(RegisterInterference{RegisterInterferenceKind::RegisterUnit, std::nullopt,
                                            std::nullopt, Unit.Unit, std::nullopt});
    }
  }

  for (const auto &[OtherReg, OtherPhysical] : OrderedAssignments) {
    if (OtherReg == VirtualReg)
      continue;
    const LiveInterval *OtherInterval = Intervals->interval(OtherReg);
    if (OtherInterval != nullptr &&
        intervalsOverlapOnCandidates(*Interval, PhysicalReg, *OtherInterval, OtherPhysical)) {
      Result.push_back(RegisterInterference{RegisterInterferenceKind::VirtualRegister, OtherReg,
                                            std::nullopt, std::nullopt, std::nullopt});
    }
  }

  std::ranges::sort(
      Result, [](const RegisterInterference &Left, const RegisterInterference &Right) {
        return std::tuple{Left.Kind, Left.VirtualReg.value_or(Register{}).opaqueValue(),
                          Left.FixedReg.value_or(PhysicalRegister{}).number(),
                          Left.Unit ? Left.Unit->value() : 0U,
                          Left.Instruction ? Left.Instruction->value() : 0ULL} <
               std::tuple{Right.Kind, Right.VirtualReg.value_or(Register{}).opaqueValue(),
                          Right.FixedReg.value_or(PhysicalRegister{}).number(),
                          Right.Unit ? Right.Unit->value() : 0U,
                          Right.Instruction ? Right.Instruction->value() : 0ULL};
      });
  Result.erase(std::unique(Result.begin(), Result.end(),
                           [](const RegisterInterference &Left, const RegisterInterference &Right) {
                             return Left.Kind == Right.Kind &&
                                    Left.VirtualReg == Right.VirtualReg &&
                                    Left.FixedReg == Right.FixedReg && Left.Unit == Right.Unit &&
                                    Left.Instruction == Right.Instruction;
                           }),
               Result.end());
  return Result;
}

RegisterInterference LiveRegMatrix::checkInterference(Register VirtualReg,
                                                      PhysicalRegister PhysicalReg) const noexcept {
  const std::vector<RegisterInterference> Values = collectInterferences(VirtualReg, PhysicalReg);
  if (Values.empty())
    return {};
  return Values.front();
}

Expected<void> LiveRegMatrix::assign(Register VirtualReg, PhysicalRegister PhysicalReg) {
  if (!VirtualReg.isVirtual() || Intervals->interval(VirtualReg) == nullptr ||
      Target->registers().physicalRegister(PhysicalReg) == nullptr) {
    return makeUnexpected(
        ErrorCode::InvalidArgument,
        "LiveRegMatrix assignment requires a known interval and physical register");
  }
  if (regalloc_detail::isUnavailablePhysicalRegister(*Function, Target->registers(), PhysicalReg)) {
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "LiveRegMatrix cannot assign an unavailable physical register");
  }
  auto Info = Function->registerInfo().virtualRegisterInfo(VirtualReg);
  if (!Info)
    return std::unexpected(Info.error());
  const auto ClassID = Info->get().RegClass;
  if (ClassID) {
    if (!Target->registers().registerClassContains(ClassID.value(), PhysicalReg)) {
      return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                            "LiveRegMatrix assignment is outside the virtual register class");
    }
  }
  if (!regalloc_detail::candidateSupportsSubRegisters(*Function, Target->registers(), VirtualReg,
                                                      PhysicalReg)) {
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "LiveRegMatrix assignment lacks a required subregister view");
  }
  if (Assignments.contains(VirtualReg))
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register is already assigned");
  const RegisterInterference Interference = checkInterference(VirtualReg, PhysicalReg);
  if (Interference.Kind != RegisterInterferenceKind::Free) {
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "physical register assignment has live interference");
  }
  Assignments.emplace(VirtualReg, PhysicalReg);
  OrderedAssignments.emplace_back(VirtualReg, PhysicalReg);
  std::ranges::sort(OrderedAssignments, [](const auto &Left, const auto &Right) {
    return Left.first.opaqueValue() < Right.first.opaqueValue();
  });
  return {};
}

Expected<void> LiveRegMatrix::unassign(Register VirtualReg) {
  if (!Assignments.erase(VirtualReg))
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register is not assigned");
  std::erase_if(OrderedAssignments,
                [VirtualReg](const auto &Entry) { return Entry.first == VirtualReg; });
  return {};
}

std::optional<PhysicalRegister> LiveRegMatrix::assignment(Register VirtualReg) const noexcept {
  const auto Position = Assignments.find(VirtualReg);
  if (Position == Assignments.end())
    return std::nullopt;
  return Position->second;
}

} // namespace rcc
