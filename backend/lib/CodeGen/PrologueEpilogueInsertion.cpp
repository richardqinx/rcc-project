// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PrologueEpilogueInsertion.h"

#include "rcc/CodeGen/FrameIndexElimination.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/RegisterScavenger.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {} // namespace

const MachinePassContract &PrologueEpilogueInsertionPass::contract() const noexcept {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "prologepilog",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs},
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
  const MachineFunctionProperties Missing = contract().Requires.missingFrom(Function.properties());
  if (!Missing.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "PrologueEpilogueInsertion requires properties: " + Missing.toString());
  }

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(withErrorContext(Trial.error(), "frame transaction setup failed: "));
  auto TrialRun = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialRun)
    return std::unexpected(
        withErrorContext(TrialRun.error(), "frame transaction rejected input: "));
  auto Transition = Trial->ClonedFunction->properties().applyTransition(
      contract().Requires, contract().Sets, contract().Clears);
  if (!Transition) {
    return std::unexpected(
        withErrorContext(Transition.error(), "frame transaction property update failed: "));
  }
  auto Verified =
      MachineVerifier::verify(*Trial->ClonedFunction, Target,
                              MachineVerifierOptions{.Mode = MachineVerifierMode::Generic,
                                                     .Stage = contract().VerificationStage,
                                                     .VerifyProperties = true});
  if (!Verified) {
    return std::unexpected(
        withErrorContext(Verified.error(), "frame transaction postcondition failed: "));
  }

  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "frame lowering diverged after successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
PrologueEpilogueInsertionPass::runImplementation(MachineFunction &Function,
                                                 const TargetDescription &Target) const {
  auto Prepared = Info->prepareFrame(Function, Target);
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

  if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());
  }

  MachinePassResult Result;
  Result.Changed = true;
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                    MachinePassChange::Registers, MachinePassChange::Frame};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
