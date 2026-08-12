// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/VirtRegMap.h"

#include "RegAllocInternal.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/LiveRegMatrix.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

namespace rcc {

Expected<void> VirtRegMap::add(Register VirtualReg, bool Spillable,
                               std::optional<Register> SplitParent,
                               std::optional<Register> Origin) {
  if (!VirtualReg.isVirtual() || !Function->registerInfo().isKnownVirtualRegister(VirtualReg))
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "VirtRegMap requires a known virtual register");
  if (SplitParent && !SplitParent->isVirtual())
    return makeUnexpected(ErrorCode::InvalidArgument, "split parent must be a virtual register");
  if (Origin && !Origin->isVirtual())
    return makeUnexpected(ErrorCode::InvalidArgument, "split origin must be a virtual register");
  auto [Position, Inserted] =
      Entries.try_emplace(VirtualReg, VirtRegMapEntry{VirtualReg,
                                                      VirtRegAllocationState::Unassigned,
                                                      std::nullopt,
                                                      SplitParent,
                                                      Origin,
                                                      Spillable,
                                                      {}});
  if (!Inserted) {
    Position->second.Spillable = Spillable;
    if (SplitParent)
      Position->second.SplitParent = SplitParent;
    if (Origin)
      Position->second.Origin = Origin;
  }
  return {};
}

Expected<void> VirtRegMap::assign(Register VirtualReg, PhysicalRegister PhysicalReg) {
  VirtRegMapEntry *Value = entry(VirtualReg);
  if (Value == nullptr || !PhysicalReg.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "VirtRegMap assignment is invalid");
  Value->PhysicalReg = PhysicalReg;
  Value->State = VirtRegAllocationState::Assigned;
  return {};
}

Expected<void> VirtRegMap::assignStackSlot(Register VirtualReg, FrameIndex StackSlot,
                                           LaneMask Lanes) {
  VirtRegMapEntry *Value = entry(VirtualReg);
  if (Value == nullptr || Function->frameInfo().object(StackSlot) == nullptr || Lanes.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "VirtRegMap spill assignment is invalid");
  Value->PhysicalReg.reset();
  Value->State = VirtRegAllocationState::Spilled;
  Value->Spill.StackSlot = StackSlot;
  Value->Spill.Lanes = Lanes;
  return {};
}

Expected<void> VirtRegMap::markSplit(Register VirtualReg, Register Parent, Register Origin) {
  VirtRegMapEntry *Value = entry(VirtualReg);
  if (Value == nullptr || !Parent.isVirtual() || !Origin.isVirtual() || Parent == VirtualReg ||
      Origin == VirtualReg || !Function->registerInfo().isKnownVirtualRegister(Parent) ||
      !Function->registerInfo().isKnownVirtualRegister(Origin))
    return makeUnexpected(ErrorCode::InvalidArgument, "VirtRegMap split relationship is invalid");
  Value->State = VirtRegAllocationState::Split;
  Value->SplitParent = Parent;
  Value->Origin = Origin;
  return {};
}

Expected<void> VirtRegMap::setSpillable(Register VirtualReg, bool Spillable) {
  VirtRegMapEntry *Value = entry(VirtualReg);
  if (Value == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "unknown virtual register in VirtRegMap");
  Value->Spillable = Spillable;
  return {};
}

VirtRegMapEntry *VirtRegMap::entry(Register VirtualReg) noexcept {
  const auto Position = Entries.find(VirtualReg);
  return Position == Entries.end() ? nullptr : &Position->second;
}

const VirtRegMapEntry *VirtRegMap::entry(Register VirtualReg) const noexcept {
  const auto Position = Entries.find(VirtualReg);
  return Position == Entries.end() ? nullptr : &Position->second;
}

std::optional<PhysicalRegister> VirtRegMap::physicalRegister(Register VirtualReg) const noexcept {
  const VirtRegMapEntry *Value = entry(VirtualReg);
  return Value == nullptr ? std::nullopt : Value->PhysicalReg;
}

std::optional<FrameIndex> VirtRegMap::stackSlot(Register VirtualReg) const noexcept {
  const VirtRegMapEntry *Value = entry(VirtualReg);
  return Value == nullptr ? std::nullopt : Value->Spill.StackSlot;
}

bool VirtRegMap::isSpillable(Register VirtualReg) const noexcept {
  const VirtRegMapEntry *Value = entry(VirtualReg);
  return Value != nullptr && Value->Spillable;
}

std::vector<const VirtRegMapEntry *> VirtRegMap::orderedEntries() const {
  std::vector<const VirtRegMapEntry *> Result;
  Result.reserve(Entries.size());
  for (const auto &[Reg, Value] : Entries) {
    (void)Reg;
    Result.push_back(&Value);
  }
  std::ranges::sort(Result, [](const VirtRegMapEntry *Left, const VirtRegMapEntry *Right) {
    return Left->VirtualReg.opaqueValue() < Right->VirtualReg.opaqueValue();
  });
  return Result;
}

Expected<void> VirtRegMap::verify() const {
  if (Function == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "VirtRegMap has no MachineFunction");
  for (const VirtRegMapEntry *Value : orderedEntries()) {
    if (Value == nullptr || !Value->VirtualReg.isVirtual() ||
        !Function->registerInfo().isKnownVirtualRegister(Value->VirtualReg))
      return makeUnexpected(ErrorCode::VerificationError, "VirtRegMap contains an invalid entry");
    if (Value->State == VirtRegAllocationState::Assigned &&
        (!Value->PhysicalReg || !Value->PhysicalReg->isValid()))
      return makeUnexpected(ErrorCode::VerificationError,
                            "assigned VirtRegMap entry has no register");
    if (Value->State == VirtRegAllocationState::Spilled && !Value->Spill.StackSlot)
      return makeUnexpected(ErrorCode::VerificationError,
                            "spilled VirtRegMap entry has no stack slot");
    if (Value->Spill.StackSlot && Value->Spill.Lanes.empty())
      return makeUnexpected(ErrorCode::VerificationError,
                            "VirtRegMap spill metadata has an empty lane mask");
    if (Value->SplitParent &&
        (!Value->SplitParent->isVirtual() || *Value->SplitParent == Value->VirtualReg ||
         !Function->registerInfo().isKnownVirtualRegister(*Value->SplitParent)))
      return makeUnexpected(ErrorCode::VerificationError, "VirtRegMap has an invalid split parent");
    if (Value->Origin && (!Value->Origin->isVirtual() || *Value->Origin == Value->VirtualReg ||
                          !Function->registerInfo().isKnownVirtualRegister(*Value->Origin)))
      return makeUnexpected(ErrorCode::VerificationError, "VirtRegMap has an invalid split origin");
    if (Value->State == VirtRegAllocationState::Split && (!Value->SplitParent || !Value->Origin))
      return makeUnexpected(ErrorCode::VerificationError,
                            "split VirtRegMap entry lacks parent or origin metadata");
  }
  return {};
}

Expected<bool> rewriteVirtualRegisters(MachineFunction &Function, const TargetDescription &Target,
                                       const VirtRegMap &Mapping) {
  if (&Mapping.function() != &Function)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "VirtRegMap belongs to a different MachineFunction");
  auto MappingVerification = Mapping.verify();
  if (!MappingVerification)
    return std::unexpected(MappingVerification.error());

  MachineFunctionAnalysisManager ValidationAnalyses;
  auto Registered = registerStage11MachineAnalyses(ValidationAnalyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  auto Intervals = ValidationAnalyses.getResult<LiveIntervalsAnalysis>(Function, Target);
  if (!Intervals)
    return std::unexpected(Intervals.error());
  LiveRegMatrix ValidationMatrix(Function, Target, **Intervals);
  for (const VirtRegMapEntry *Entry : Mapping.orderedEntries()) {
    if (Entry == nullptr || !Entry->PhysicalReg)
      continue;
    auto ClassID = regalloc_detail::registerClassFor(Function, Entry->VirtualReg);
    if (!ClassID)
      return std::unexpected(ClassID.error());
    if (!Target.registers().registerClassContains(*ClassID, *Entry->PhysicalReg)) {
      return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                            "VirtRegMap physical register is outside the virtual register class");
    }
    const LiveInterval *Interval = (*Intervals)->interval(Entry->VirtualReg);
    if (Interval == nullptr || Interval->MainRange.empty())
      continue;
    auto Assigned = ValidationMatrix.assign(Entry->VirtualReg, *Entry->PhysicalReg);
    if (!Assigned)
      return std::unexpected(Assigned.error());
  }

  struct Rewrite final {
    MachineOperand *Operand = nullptr;
    Register NewRegister;
  };
  std::vector<Rewrite> Rewrites;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        MachineOperand *Operand = Instruction->operandAt(Index);
        const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
        if (Reg == nullptr || !Reg->isVirtual())
          continue;
        const auto Assigned = Mapping.physicalRegister(*Reg);
        if (!Assigned) {
          if (Operand->isDebugUse()) {
            Rewrites.push_back({Operand, Register{}});
            continue;
          }
          return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                "physical rewrite found an unassigned virtual register " +
                                    Reg->toString());
        }
        PhysicalRegister Effective = *Assigned;
        if (Operand->subReg().value() != 0) {
          const auto Sub = Target.registers().subRegister(*Assigned, Operand->subReg());
          if (!Sub) {
            return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                                  "physical assignment does not support a required subregister");
          }
          Effective = *Sub;
        }
        Rewrites.push_back({Operand, Register::fromPhysical(Effective)});
      }
    }
  }

  for (const Rewrite &Plan : Rewrites) {
    auto SetRegister = Plan.Operand->setRegister(Plan.NewRegister);
    if (!SetRegister)
      return std::unexpected(SetRegister.error());
    if (Plan.Operand->subReg().value() != 0) {
      auto Cleared = Plan.Operand->setSubReg(SubRegIndex{});
      if (!Cleared)
        return std::unexpected(Cleared.error());
    }
    if (Plan.NewRegister.isNoRegister()) {
      RegisterOperandFlags Flags = Plan.Operand->registerFlags();
      Flags.set(RegisterOperandFlag::Undef);
      auto Updated = Plan.Operand->setRegisterFlags(Flags);
      if (!Updated)
        return std::unexpected(Updated.error());
    }
  }

  Function.registerInfo().clearLiveInVirtualRegisters();
  for (std::size_t Index = 0; Index < Function.registerInfo().virtualRegisters().size(); ++Index) {
    auto Reg = regalloc_detail::virtualRegisterAt(Index);
    if (!Reg)
      return std::unexpected(Reg.error());
    if (!Function.registerInfo().isKnownVirtualRegister(*Reg))
      continue;
    auto Erased = Function.registerInfo().eraseVirtualRegister(*Reg);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  auto Liveness = rebuildMachineLiveness(Function, Target);
  if (!Liveness)
    return std::unexpected(Liveness.error());
  return !Rewrites.empty();
}

const MachinePassContract &VirtualRegisterRewritePass::contract() const noexcept {
  static const MachinePassContract Contract{
      .Name = "VirtualRegisterRewrite",
      .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                   MachineFunctionProperty::TiedOpsRewritten},
      .Sets = {MachineFunctionProperty::NoVRegs},
      .Clears = {},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::RegisterAllocated,
  };
  return Contract;
}

Expected<MachinePassResult> VirtualRegisterRewritePass::run(MachineFunction &Function,
                                                            const TargetDescription &Target,
                                                            MachineFunctionAnalysisManager &) {
  if (Mapping == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "VirtualRegisterRewrite requires a VirtRegMap");
  auto Rewritten = rewriteVirtualRegisters(Function, Target, *Mapping);
  if (!Rewritten)
    return std::unexpected(Rewritten.error());
  MachinePassResult Result;
  Result.Changed = *Rewritten;
  if (Result.Changed) {
    Result.Changes.add(MachinePassChange::Registers);
    Result.Changes.add(MachinePassChange::Operands);
  }
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
