// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineInstr.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

namespace rcc {

MachineInstr::MachineInstr(MachineFunction &OwnerFunction, Opcode InstructionOpcode,
                           MachineInstrStableID StableID)
    : OwnerFunction(&OwnerFunction), InstructionOpcode(InstructionOpcode), StableID(StableID) {}

MachineInstr::~MachineInstr() {
  for (MachineOperand &Operand : Operands)
    Operand.detachFromInstruction();
}

Expected<void> MachineInstr::setOpcode(Opcode NewOpcode) {
  if (!NewOpcode.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "machine opcode must be nonzero");
  InstructionOpcode = NewOpcode;
  return {};
}

MachineOperand *MachineInstr::operandAt(std::size_t Index) noexcept {
  if (Index >= Operands.size())
    return nullptr;
  return &Operands[Index];
}

const MachineOperand *MachineInstr::operandAt(std::size_t Index) const noexcept {
  if (Index >= Operands.size())
    return nullptr;
  return &Operands[Index];
}

void MachineInstr::reserveOperands(std::size_t Capacity) {
  Operands.reserve(Capacity);
}

Expected<void> MachineInstr::addOperand(MachineOperand Operand) {
  return insertOperand(Operands.size(), std::move(Operand));
}

Expected<void> MachineInstr::insertOperand(std::size_t Index, MachineOperand Operand) {
  if (Index > Operands.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "operand insertion index is out of range");
  if (Operand.Parent != nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument, "operand already belongs to an instruction");
  if (Operand.TiedOperandIndex)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "detached operand cannot carry a tied operand index");
  if (const auto *BlockOperand = Operand.getIf<BasicBlockOperand>();
      BlockOperand != nullptr && &BlockOperand->Block->function() != OwnerFunction) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "basic block operand belongs to another machine function");
  }
  if (const Register *Reg = Operand.getIf<Register>();
      Reg != nullptr && Reg->isVirtual() &&
      !OwnerFunction->registerInfo().isKnownVirtualRegister(*Reg)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "register operand references an unknown virtual register");
  }

  Operands.insert(Operands.begin() + static_cast<std::ptrdiff_t>(Index), std::move(Operand));
  adjustTiesForInsertion(Index);
  Operands[Index].attachTo(*this, linkedRegisterInfo());
  return {};
}

Expected<void> MachineInstr::eraseOperand(std::size_t Index) {
  if (Index >= Operands.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "operand erase index is out of range");

  adjustTiesForErasure(Index);
  Operands.erase(Operands.begin() + static_cast<std::ptrdiff_t>(Index));
  return {};
}

Expected<void> MachineInstr::tieOperands(std::size_t DefIndex, std::size_t UseIndex) {
  if (DefIndex >= Operands.size() || UseIndex >= Operands.size() || DefIndex == UseIndex) {
    return makeUnexpected(ErrorCode::InvalidArgument, "tied operand indices are invalid");
  }

  MachineOperand &DefOperand = Operands[DefIndex];
  MachineOperand &UseOperand = Operands[UseIndex];
  if (!DefOperand.isRegister() || !UseOperand.isRegister() || !DefOperand.isDef() ||
      !UseOperand.isUse()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "tie must connect a register definition and use");
  }
  if (DefOperand.TiedOperandIndex || UseOperand.TiedOperandIndex) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "an operand cannot participate in more than one tie");
  }

  DefOperand.setTiedOperandIndex(UseIndex);
  UseOperand.setTiedOperandIndex(DefIndex);
  return {};
}

Expected<void> MachineInstr::untieOperand(std::size_t Index) {
  if (Index >= Operands.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "operand tie index is out of range");
  const auto OtherIndex = Operands[Index].TiedOperandIndex;
  if (!OtherIndex)
    return makeUnexpected(ErrorCode::InvalidArgument, "operand is not tied");
  if (*OtherIndex >= Operands.size() || Operands[*OtherIndex].TiedOperandIndex != Index) {
    return makeUnexpected(ErrorCode::VerificationError, "operand tie relation is not reciprocal");
  }

  Operands[*OtherIndex].setTiedOperandIndex(std::nullopt);
  Operands[Index].setTiedOperandIndex(std::nullopt);
  return {};
}

Expected<void> MachineInstr::addMemOperand(const MachineMemOperand &MemOperand) {
  if (!OwnerFunction->ownsMemOperand(MemOperand)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine memory operand belongs to another function");
  }
  MemOperands.push_back(&MemOperand);
  return {};
}

Expected<void> MachineInstr::eraseMemOperand(std::size_t Index) {
  if (Index >= MemOperands.size()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine memory operand index is out of range");
  }
  MemOperands.erase(MemOperands.begin() + static_cast<std::ptrdiff_t>(Index));
  return {};
}

MachineRegisterInfo *MachineInstr::linkedRegisterInfo() noexcept {
  if (!isLinked())
    return nullptr;
  return &OwnerFunction->registerInfo();
}

Expected<void> MachineInstr::attachToBlock(MachineBasicBlock &Block) {
  if (ParentBlock != nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction is already linked to a basic block");
  }

  MachineRegisterInfo &RegInfo = OwnerFunction->registerInfo();
  for (const MachineOperand &Operand : Operands) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg != nullptr && Reg->isVirtual() && !RegInfo.isKnownVirtualRegister(*Reg)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "instruction references an unknown virtual register");
    }
  }

  ParentBlock = &Block;
  for (MachineOperand &Operand : Operands) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg != nullptr && !Reg->isNoRegister())
      Operand.linkToRegInfo(RegInfo);
  }
  return {};
}

void MachineInstr::detachFromBlock() noexcept {
  for (MachineOperand &Operand : Operands)
    Operand.unlinkFromRegInfo();
  ParentBlock = nullptr;
  PreviousInstruction = nullptr;
  NextInstruction = nullptr;
}

void MachineInstr::adjustTiesForInsertion(std::size_t Index) noexcept {
  for (MachineOperand &Operand : Operands) {
    if (Operand.TiedOperandIndex && *Operand.TiedOperandIndex >= Index)
      ++*Operand.TiedOperandIndex;
  }
}

void MachineInstr::adjustTiesForErasure(std::size_t Index) noexcept {
  for (MachineOperand &Operand : Operands) {
    if (!Operand.TiedOperandIndex)
      continue;
    if (*Operand.TiedOperandIndex == Index)
      Operand.TiedOperandIndex.reset();
    else if (*Operand.TiedOperandIndex > Index)
      --*Operand.TiedOperandIndex;
  }
}

} // namespace rcc
