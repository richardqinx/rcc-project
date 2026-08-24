// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePeepholeOptimizer.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <optional>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &peepholeOptimizerContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "peephole-opt",
        .Requires = {MachineFunctionProperty::IsSSA},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers};
    return Value;
  }();
  return Contract;
}

} // namespace

const MachinePassContract &MachinePeepholeOptimizerPass::contract() const noexcept {
  return peepholeOptimizerContract();
}

Expected<MachinePassResult> MachinePeepholeOptimizerPass::run(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineFunctionAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine peephole optimizer requires SSA MachineIR");
  }

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "machine peephole transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "machine peephole diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachinePeepholeOptimizerPass::runImplementation(MachineFunction &Function,
                                                const TargetDescription &Target) const {
  MachinePassResult Result;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
      MachineInstr *Next = Instruction->next();
      if (!Instruction->isDebugInstr()) {
        auto Changed = Info->optimizeCondBranch(*Instruction, Function, Target);
        if (!Changed)
          return std::unexpected(Changed.error());
        Result.Changed |= *Changed;
      }
      Instruction = Next;
    }
  }
  if (Result.Changed) {
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                      MachinePassChange::Registers};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
