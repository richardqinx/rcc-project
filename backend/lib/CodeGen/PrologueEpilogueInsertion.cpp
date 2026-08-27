// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PrologueEpilogueInsertion.h"

#include "rcc/CodeGen/FrameIndexElimination.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterScavenger.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

#include <utility>
#include <vector>

namespace rcc {
namespace {} // namespace

const MachinePassContract &PrologueEpilogueInsertionPass::contract() const noexcept {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "prologepilog",
        .Requires = {MachineFunctionProperty::NoPHIs, MachineFunctionProperty::NoVRegs},
        .Sets = {MachineFunctionProperty::FrameFinalized, MachineFunctionProperty::NoFrameIndices},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::FrameFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers, MachinePassChange::Frame};
    return Value;
  }();
  return Contract;
}

Expected<MachinePassResult> PrologueEpilogueInsertionPass::run(MachineFunction &Function,
                                                               const TargetDescription &Target,
                                                               MachineFunctionAnalysisManager &) {
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "PrologueEpilogueInsertion requires target frame lowering");
  return runImplementation(Function, Target, Options);
}

Expected<MachinePassResult>
PrologueEpilogueInsertionPass::runImplementation(MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 FrameLoweringOptions RunOptions) const {
  auto Prepared = Info->prepareFrame(Function, Target, RunOptions);
  if (!Prepared)
    return std::unexpected(Prepared.error());

  RegisterScavenger Scavenger(Function, Target);
  std::vector<MachineInstr *> CallFramePseudos;
  std::vector<std::pair<MachineBasicBlock *, MachineInstr *>> Epilogues;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (Info->isCallFramePseudo(*Instruction))
        CallFramePseudos.push_back(Instruction);
      if (Info->isEpilogueTerminator(*Instruction, Target))
        Epilogues.emplace_back(Block, Instruction);
    }
  }
  for (MachineInstr *Pseudo : CallFramePseudos) {
    auto Eliminated = Info->eliminateCallFramePseudo(*Pseudo, Function, Target, Scavenger);
    if (!Eliminated)
      return std::unexpected(Eliminated.error());
  }

  auto CalleeSaved = Info->insertCalleeSavedSpillsAndRestores(Function, Target);
  if (!CalleeSaved)
    return std::unexpected(CalleeSaved.error());

  auto PreFinalized = Info->processFunctionBeforeFrameFinalized(Function, Target);
  if (!PreFinalized)
    return std::unexpected(PreFinalized.error());

  auto Finalized = Info->finalizeFrame(Function, Target);
  if (!Finalized)
    return std::unexpected(Finalized.error());
  if (!Function.frameInfo().isFrozen()) {
    return makeUnexpected(ErrorCode::InternalError,
                          "target frame lowering did not freeze the frame layout");
  }

  auto Prologue = Info->emitPrologue(Function, Target, Scavenger);
  if (!Prologue)
    return std::unexpected(Prologue.error());
  for (const auto &[Block, Terminator] : Epilogues) {
    auto Epilogue = Info->emitEpilogue(*Block, *Terminator, Function, Target, Scavenger);
    if (!Epilogue)
      return std::unexpected(Epilogue.error());
  }

  auto PreFrameIndices = Info->processFunctionBeforeFrameIndexElimination(Function, Target);
  if (!PreFrameIndices)
    return std::unexpected(PreFrameIndices.error());

  auto FrameIndices = eliminateFrameIndices(Function, Target, *Info, Scavenger);
  if (!FrameIndices)
    return std::unexpected(FrameIndices.error());

  MachinePassResult Result;
  Result.Changed = true;
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                    MachinePassChange::Registers, MachinePassChange::Frame};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
