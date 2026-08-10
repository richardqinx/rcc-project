// SPDX-License-Identifier: GPL-3.0-or-later

#include "RegAllocInternal.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/Spiller.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] Expected<void> clearDebugRegisterOperand(MachineOperand &Operand) {
  RegisterOperandFlags Flags = Operand.registerFlags();
  Flags.set(RegisterOperandFlag::Undef);
  auto SetReg = Operand.setRegister(Register{});
  if (!SetReg)
    return std::unexpected(SetReg.error());
  auto SetSubReg = Operand.setSubReg(SubRegIndex{});
  if (!SetSubReg)
    return std::unexpected(SetSubReg.error());
  return Operand.setRegisterFlags(Flags);
}

} // namespace

Expected<std::optional<std::vector<Register>>> Spiller::tryRematerialize(Register VirtualReg) {
  if (!EnableRematerialization)
    return std::optional<std::vector<Register>>{};
  const MachineOperand *Definition = Function->registerInfo().getUniqueVRegDef(VirtualReg);
  const MachineInstr *DefiningInstruction = Definition == nullptr ? nullptr : Definition->parent();
  if (DefiningInstruction == nullptr || DefiningInstruction->parent() == nullptr)
    return std::optional<std::vector<Register>>{};
  auto Rematerializable =
      TargetInfo->isRematerializable(*DefiningInstruction, VirtualReg, *Function, *Target);
  if (!Rematerializable)
    return std::unexpected(Rematerializable.error());
  if (!*Rematerializable)
    return std::optional<std::vector<Register>>{};

  struct UseSite final {
    MachineInstr *Instruction = nullptr;
    std::vector<std::size_t> OperandIndices;
  };
  std::vector<UseSite> Sites;
  for (MachineBasicBlock *Block = Function->firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      UseSite Site{Instruction, {}};
      bool HasDefinition = false;
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        const MachineOperand *Operand = Instruction->operandAt(Index);
        const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
        if (Reg == nullptr || *Reg != VirtualReg)
          continue;
        HasDefinition |= Operand->isDef();
        if (Operand->isUse() && !Operand->isDebugUse())
          Site.OperandIndices.push_back(Index);
      }
      if (HasDefinition && !Site.OperandIndices.empty())
        return std::optional<std::vector<Register>>{};
      if (!Site.OperandIndices.empty())
        Sites.push_back(std::move(Site));
    }
  }
  if (Sites.empty())
    return std::optional<std::vector<Register>>{};

  if (Actions != nullptr) {
    for (const UseSite &Site : Sites) {
      auto Accepted = Actions->request("rematerialize", *Function, Site.Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted)
        return std::optional<std::vector<Register>>{};
    }
  }

  std::vector<Register> NewRegisters;
  NewRegisters.reserve(Sites.size());
  for (UseSite &Site : Sites) {
    auto Child = Edit.createFrom(VirtualReg, false);
    if (!Child)
      return std::unexpected(Child.error());
    MachineIRBuilder Builder(*Function, *Target);
    auto Positioned = Builder.setInsertPointBefore(*Site.Instruction);
    if (!Positioned)
      return std::unexpected(Positioned.error());
    auto Materialized =
        TargetInfo->rematerialize(Builder, *DefiningInstruction, *Child, VirtualReg, *Target);
    if (!Materialized)
      return std::unexpected(Materialized.error());
    for (const std::size_t Index : Site.OperandIndices) {
      MachineOperand *Operand = Site.Instruction->operandAt(Index);
      auto Replaced = Operand->setRegister(*Child);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    }
    VirtRegMapEntry *Entry = Mapping->entry(VirtualReg);
    if (Entry != nullptr)
      ++Entry->Spill.RematerializationCount;
    NewRegisters.push_back(*Child);
  }

  bool HasNonDebugUse = false;
  for (const MachineOperand &Operand : Function->registerInfo().useOperands(VirtualReg, false)) {
    if (Operand.parent() != nullptr) {
      HasNonDebugUse = true;
      break;
    }
  }
  if (!HasNonDebugUse) {
    MachineBasicBlock *Block = const_cast<MachineBasicBlock *>(DefiningInstruction->parent());
    auto Removed = Block->removeInstruction(*const_cast<MachineInstr *>(DefiningInstruction));
    if (!Removed)
      return std::unexpected(Removed.error());
  }
  return std::optional<std::vector<Register>>(std::move(NewRegisters));
}

Expected<std::vector<Register>> Spiller::spillAtInstructions(Register VirtualReg) {
  auto ClassID = regalloc_detail::registerClassFor(*Function, VirtualReg);
  if (!ClassID)
    return std::unexpected(ClassID.error());
  auto Validated = TargetInfo->validateSpillReload(*ClassID, *Target);
  if (!Validated) {
    std::string Message =
        "target declares an allocatable spillable class without spill/reload support: ";
    Message.append(Validated.error().message());
    return makeUnexpected(ErrorCode::InternalError, std::move(Message));
  }
  auto StackSlot = Edit.getOrCreateSpillSlot(VirtualReg);
  if (!StackSlot)
    return std::unexpected(StackSlot.error());
  auto LiveInSpill = spillLiveIn(VirtualReg, *ClassID, *StackSlot);
  if (!LiveInSpill)
    return std::unexpected(LiveInSpill.error());

  struct SpillSite final {
    MachineInstr *Instruction = nullptr;
    std::vector<std::size_t> OperandIndices;
    std::vector<std::size_t> DebugIndices;
    bool HasUse = false;
    bool HasDef = false;
  };
  std::vector<SpillSite> Sites;
  for (MachineBasicBlock *Block = Function->firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      SpillSite Site;
      Site.Instruction = Instruction;
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        const MachineOperand *Operand = Instruction->operandAt(Index);
        const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
        if (Reg == nullptr || *Reg != VirtualReg)
          continue;
        if (Operand->isDebugUse()) {
          Site.DebugIndices.push_back(Index);
          continue;
        }
        Site.OperandIndices.push_back(Index);
        Site.HasUse |= Operand->isUse();
        Site.HasDef |= Operand->isDef();
      }
      if (!Site.OperandIndices.empty() || !Site.DebugIndices.empty())
        Sites.push_back(std::move(Site));
    }
  }

  std::vector<Register> NewRegisters;
  if (Actions != nullptr) {
    for (const SpillSite &Site : Sites) {
      if (Site.OperandIndices.empty())
        continue;
      auto Accepted = Actions->request("split-live-range", *Function, Site.Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted) {
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "required live-range split action was rejected");
      }
    }
  }
  for (SpillSite &Site : Sites) {
    for (const std::size_t Index : Site.DebugIndices) {
      MachineOperand *Operand = Site.Instruction->operandAt(Index);
      auto Cleared = clearDebugRegisterOperand(*Operand);
      if (!Cleared)
        return std::unexpected(Cleared.error());
    }
    if (Site.OperandIndices.empty())
      continue;

    auto Child = Edit.createFrom(VirtualReg, false);
    if (!Child)
      return std::unexpected(Child.error());
    if (Site.HasUse) {
      MachineIRBuilder Builder(*Function, *Target);
      auto Positioned = Builder.setInsertPointBefore(*Site.Instruction);
      if (!Positioned)
        return std::unexpected(Positioned.error());
      auto Reload = TargetInfo->emitReload(Builder, *Child, *ClassID, *StackSlot, LaneMask::all());
      if (!Reload)
        return std::unexpected(Reload.error());
      VirtRegMapEntry *Entry = Mapping->entry(VirtualReg);
      if (Entry != nullptr)
        ++Entry->Spill.ReloadCount;
    }
    for (const std::size_t Index : Site.OperandIndices) {
      MachineOperand *Operand = Site.Instruction->operandAt(Index);
      auto Replaced = Operand->setRegister(*Child);
      if (!Replaced)
        return std::unexpected(Replaced.error());
    }
    if (Site.HasDef) {
      const InstructionDescriptor *Descriptor =
          Target->instructionDescriptor(Site.Instruction->opcode());
      if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Terminator)) {
        return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                              "a terminator-defined value cannot be spilled on its outgoing edge");
      }
      MachineIRBuilder Builder(*Function, *Target);
      auto Positioned = Builder.setInsertPointAfter(*Site.Instruction);
      if (!Positioned)
        return std::unexpected(Positioned.error());
      auto Spill = TargetInfo->emitSpill(Builder, *Child, *ClassID, *StackSlot, LaneMask::all());
      if (!Spill)
        return std::unexpected(Spill.error());
      VirtRegMapEntry *Entry = Mapping->entry(VirtualReg);
      if (Entry != nullptr)
        ++Entry->Spill.SpillCount;
    }
    NewRegisters.push_back(*Child);
  }
  return NewRegisters;
}

Expected<void> Spiller::spillLiveIn(Register VirtualReg, RegisterClassID Class,
                                    FrameIndex StackSlot) {
  const auto LiveIn = std::ranges::find_if(Function->registerInfo().liveIns(),
                                           [VirtualReg](const PhysicalRegisterLiveIn &Value) {
                                             return Value.InitialVirtualRegister == VirtualReg;
                                           });
  if (LiveIn == Function->registerInfo().liveIns().end())
    return {};
  MachineBasicBlock *EntryBlock = Function->firstBlock();
  if (EntryBlock == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "function live-in spill requires an entry block");
  }
  MachineIRBuilder Builder(*Function, *Target);
  Expected<void> Positioned = EntryBlock->firstInstruction() != nullptr
                                  ? Builder.setInsertPointBefore(*EntryBlock->firstInstruction())
                                  : Builder.setInsertPoint(*EntryBlock);
  if (!Positioned)
    return std::unexpected(Positioned.error());
  auto Spill = TargetInfo->emitSpill(Builder, Register::fromPhysical(LiveIn->PhysReg), Class,
                                     StackSlot, LaneMask::all());
  if (!Spill)
    return std::unexpected(Spill.error());
  VirtRegMapEntry *Entry = Mapping->entry(VirtualReg);
  if (Entry != nullptr)
    ++Entry->Spill.SpillCount;
  return {};
}

Expected<std::vector<Register>> Spiller::spill(Register VirtualReg) {
  if (!VirtualReg.isVirtual() || !Function->registerInfo().isKnownVirtualRegister(VirtualReg))
    return makeUnexpected(ErrorCode::InvalidArgument, "Spiller requires a known virtual register");
  if (!Mapping->isSpillable(VirtualReg)) {
    return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                          "register pressure requires spilling a non-spillable value");
  }
  if (Actions != nullptr) {
    const MachineOperand *Definition = Function->registerInfo().getUniqueVRegDef(VirtualReg);
    const MachineInstr *Instruction = Definition == nullptr ? nullptr : Definition->parent();
    auto Accepted =
        Actions->request("spill", *Function,
                         Instruction == nullptr ? MachineInstrStableID{} : Instruction->stableID());
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted) {
      return makeUnexpected(ErrorCode::RegisterAllocationFailed,
                            "required spill action was rejected");
    }
  }
  auto Rematerialized = tryRematerialize(VirtualReg);
  if (!Rematerialized)
    return std::unexpected(Rematerialized.error());
  if (*Rematerialized)
    return std::move(**Rematerialized);
  return spillAtInstructions(VirtualReg);
}

} // namespace rcc
