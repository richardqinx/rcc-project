// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/Combiner.h"

#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"

#include <string>

namespace rcc {

const MachinePassContract &GlobalISelCombinerPass::contract() const noexcept {
  return Contract;
}

Expected<MachinePassResult> GlobalISelCombinerPass::run(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineFunctionAnalysisManager &Analyses) {
  return runImplementation(Function, Target, Analyses);
}

Expected<MachinePassResult>
GlobalISelCombinerPass::runImplementation(MachineFunction &Function,
                                          const TargetDescription &Target,
                                          MachineFunctionAnalysisManager &Analyses) const {
  if (MaximumRewrites == 0 || MaximumIterations == 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "GlobalISel combiner iteration limits must be nonzero");
  MachinePassResult Result;
  std::size_t Rewrites = 0;
  for (std::size_t Iteration = 0; Iteration < MaximumIterations; ++Iteration) {
    bool IterationChanged = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
        MachineInstr *Next = Instruction->next();
        auto Matches = Executor->match(*Table, *Instruction, Function, Target);
        if (!Matches)
          return std::unexpected(Matches.error());
        if (*Matches && Rewrites < MaximumRewrites) {
          auto Accepted =
              Analyses.requestAction("gisel-combiner", Function, Instruction->stableID());
          if (!Accepted)
            return std::unexpected(Accepted.error());
          if (*Accepted) {
            auto Combined = Executor->combine(*Table, *Instruction, Function, Target);
            if (!Combined)
              return std::unexpected(Combined.error());
            if (*Combined) {
              ++Rewrites;
              IterationChanged = true;
              Result.Changed = true;
            }
          }
        }
        Instruction = Next;
      }
    }
    if (!IterationChanged || Rewrites == MaximumRewrites)
      break;
  }

  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                    MachinePassChange::Registers};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
