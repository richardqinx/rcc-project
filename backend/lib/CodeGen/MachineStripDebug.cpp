// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineStripDebug.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"

namespace rcc {

Expected<bool> stripDebugMachineFunction(MachineFunction &Function) {
  bool Changed = false;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
      MachineInstr *Next = Instruction->next();
      if (Instruction->isDebugInstr() && Instruction->operandCount() > 1) {
        auto Removed = Instruction->eraseFromParent();
        if (!Removed)
          return std::unexpected(Removed.error());
        Changed = true;
        Instruction = Next;
        continue;
      }
      if (Instruction->debugLocation()) {
        Instruction->clearDebugLocation();
        Changed = true;
      }
      Instruction = Next;
    }
  }
  return Changed;
}

} // namespace rcc
