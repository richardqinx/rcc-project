// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineBasicBlock.h"

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"

#include <algorithm>
#include <cstddef>

namespace rcc {

std::size_t MachineBasicBlock::instructionCount() const noexcept {
  std::size_t Count = 0;
  for (const MachineInstr *Instruction = FirstInstruction; Instruction != nullptr;
       Instruction = Instruction->next()) {
    ++Count;
  }
  return Count;
}

Expected<void> MachineBasicBlock::appendInstruction(MachineInstr &Instruction) {
  if (&Instruction.function() != OwnerFunction) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction belongs to another machine function");
  }
  if (Instruction.isLinked()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction is already linked to a basic block");
  }

  auto AttachResult = Instruction.attachToBlock(*this);
  if (!AttachResult)
    return std::unexpected(AttachResult.error());

  Instruction.PreviousInstruction = LastInstruction;
  Instruction.NextInstruction = nullptr;
  if (LastInstruction != nullptr)
    LastInstruction->NextInstruction = &Instruction;
  else
    FirstInstruction = &Instruction;
  LastInstruction = &Instruction;
  if (OwnerFunction->TheDelegate != nullptr)
    OwnerFunction->TheDelegate->handleInsertion(Instruction);
  return {};
}

Expected<void> MachineBasicBlock::insertInstructionBefore(MachineInstr &Position,
                                                          MachineInstr &Instruction) {
  if (Position.parent() != this) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction insertion position is not in this block");
  }
  if (&Instruction.function() != OwnerFunction || Instruction.isLinked()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "inserted instruction is linked or belongs to another function");
  }

  auto AttachResult = Instruction.attachToBlock(*this);
  if (!AttachResult)
    return std::unexpected(AttachResult.error());

  Instruction.PreviousInstruction = Position.PreviousInstruction;
  Instruction.NextInstruction = &Position;
  if (Position.PreviousInstruction != nullptr)
    Position.PreviousInstruction->NextInstruction = &Instruction;
  else
    FirstInstruction = &Instruction;
  Position.PreviousInstruction = &Instruction;
  if (OwnerFunction->TheDelegate != nullptr)
    OwnerFunction->TheDelegate->handleInsertion(Instruction);
  return {};
}

Expected<void> MachineBasicBlock::insertInstructionAfter(MachineInstr &Position,
                                                         MachineInstr &Instruction) {
  if (Position.parent() != this) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "instruction insertion position is not in this block");
  }
  if (&Instruction.function() != OwnerFunction || Instruction.isLinked()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "inserted instruction is linked or belongs to another function");
  }

  auto AttachResult = Instruction.attachToBlock(*this);
  if (!AttachResult)
    return std::unexpected(AttachResult.error());

  Instruction.PreviousInstruction = &Position;
  Instruction.NextInstruction = Position.NextInstruction;
  if (Position.NextInstruction != nullptr)
    Position.NextInstruction->PreviousInstruction = &Instruction;
  else
    LastInstruction = &Instruction;
  Position.NextInstruction = &Instruction;
  if (OwnerFunction->TheDelegate != nullptr)
    OwnerFunction->TheDelegate->handleInsertion(Instruction);
  return {};
}

Expected<void> MachineBasicBlock::removeInstruction(MachineInstr &Instruction) {
  if (Instruction.parent() != this) {
    return makeUnexpected(ErrorCode::InvalidArgument, "instruction does not belong to this block");
  }

  if (OwnerFunction->TheDelegate != nullptr)
    OwnerFunction->TheDelegate->handleRemoval(Instruction);

  if (Instruction.PreviousInstruction != nullptr)
    Instruction.PreviousInstruction->NextInstruction = Instruction.NextInstruction;
  else
    FirstInstruction = Instruction.NextInstruction;

  if (Instruction.NextInstruction != nullptr)
    Instruction.NextInstruction->PreviousInstruction = Instruction.PreviousInstruction;
  else
    LastInstruction = Instruction.PreviousInstruction;

  Instruction.detachFromBlock();
  return {};
}

void MachineBasicBlock::clearInstructions() noexcept {
  MachineInstr *Instruction = FirstInstruction;
  while (Instruction != nullptr) {
    MachineInstr *NextInstruction = Instruction->NextInstruction;
    Instruction->detachFromBlock();
    Instruction = NextInstruction;
  }
  FirstInstruction = nullptr;
  LastInstruction = nullptr;
}

Expected<void> MachineBasicBlock::addLiveIn(PhysicalRegister PhysReg, LaneMask Lanes) {
  if (!PhysReg.isValid() || Lanes.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "block live-in requires a physical register and nonempty lanes");
  }

  const auto Position =
      std::ranges::find_if(LiveIns, [PhysReg](const MachineBasicBlockLiveIn &LiveIn) {
        return LiveIn.PhysReg == PhysReg;
      });
  if (Position != LiveIns.end()) {
    Position->Lanes = Position->Lanes | Lanes;
    return {};
  }

  LiveIns.push_back(MachineBasicBlockLiveIn{PhysReg, Lanes});
  return {};
}

Expected<void> MachineBasicBlock::removeLiveIn(PhysicalRegister PhysReg) {
  const auto Position =
      std::ranges::find_if(LiveIns, [PhysReg](const MachineBasicBlockLiveIn &LiveIn) {
        return LiveIn.PhysReg == PhysReg;
      });
  if (Position == LiveIns.end()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "block physical live-in is not present");
  }
  LiveIns.erase(Position);
  return {};
}

} // namespace rcc
