// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/FinalizeISel.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <utility>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &finalizeISelContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "finalize-isel",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::CFG, MachinePassChange::BlockLayout,
                             MachinePassChange::Frame};
    return Value;
  }();
  return Contract;
}

} // namespace

const MachinePassContract &FinalizeISelPass::contract() const noexcept {
  return finalizeISelContract();
}

Expected<MachinePassResult> FinalizeISelPass::run(MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  MachineFunctionAnalysisManager &) {
  MachinePassResult Result;
  MachineBasicBlock *Block = Function.firstBlock();
  while (Block != nullptr) {
    MachineInstr *Instruction = Block->firstInstruction();
    while (Instruction != nullptr) {
      MachineInstr *Next = Instruction->next();
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "finalize-isel encountered an unknown opcode");
      }

      if (Instruction->flags().has(MachineInstrFlag::FrameSetup) ||
          Instruction->flags().has(MachineInstrFlag::FrameDestroy)) {
        MachineFrameState State = Function.frameInfo().state();
        if (!State.AdjustsStack) {
          State.AdjustsStack = true;
          auto Updated = Function.frameInfo().setState(std::move(State));
          if (!Updated)
            return std::unexpected(Updated.error());
          Result.Changes.add(MachinePassChange::Frame);
        }
      }

      if (!Descriptor->Flags.has(InstructionFlag::UsesCustomInserter)) {
        Instruction = Next;
        continue;
      }

      auto Expanded = Info->emitInstrWithCustomInserter(*Instruction, *Block, Function, Target);
      if (!Expanded)
        return std::unexpected(Expanded.error());
      if (*Expanded == nullptr || &(*Expanded)->function() != &Function) {
        return makeUnexpected(ErrorCode::InternalError,
                              "custom inserter returned an invalid basic block");
      }

      Result.Changes.add(MachinePassChange::Instructions);
      Result.Changes.add(MachinePassChange::Operands);
      if (*Expanded != Block) {
        Result.Changes.add(MachinePassChange::CFG);
        Result.Changes.add(MachinePassChange::BlockLayout);
        Block = *Expanded;
        Instruction = Block->firstInstruction();
      } else {
        Instruction = Next;
      }
    }
    Block = Block->next();
  }

  Result.Changed = !Result.Changes.empty();
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc
