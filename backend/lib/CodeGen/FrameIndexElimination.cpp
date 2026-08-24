// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/FrameIndexElimination.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterScavenger.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

#include <cstddef>

namespace rcc {

Expected<bool> eliminateFrameIndices(MachineFunction &Function, const TargetDescription &Target,
                                     const TargetFrameLowering &FrameLowering,
                                     RegisterScavenger &Scavenger) {
  bool Changed = false;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
      MachineInstr *Next = Instruction->next();
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        if (Instruction->operandAt(Index)->kind() != MachineOperand::Kind::FrameIndex)
          continue;
        auto Eliminated =
            FrameLowering.eliminateFrameIndex(*Instruction, Index, Function, Target, Scavenger);
        if (!Eliminated)
          return std::unexpected(Eliminated.error());
        Changed = true;
      }

      Instruction = Next;
    }
  }
  return Changed;
}

} // namespace rcc
