// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/ReachingDefPrinter.h"

#include "rcc/CodeGen/MIRPrinter.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/ReachingDefAnalysis.h"

#include <iostream>
#include <optional>

namespace rcc::mir {

const MachinePassContract &ReachingDefPrinterPass::contract() const noexcept {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = std::string(name()),
        .Requires = {MachineFunctionProperty::NoVRegs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<ReachingDefAnalysis>()},
        .VerificationStage = std::nullopt,
    };
    Value.DeclaredChanges = {};
    return Value;
  }();
  return Contract;
}

Expected<MachinePassResult> ReachingDefPrinterPass::run(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineFunctionAnalysisManager &Analyses) {
  auto ReachingDefinitions = Analyses.getResult<ReachingDefAnalysis>(Function, Target);
  if (!ReachingDefinitions)
    return std::unexpected(ReachingDefinitions.error());
  auto Context = createMachineFunctionPrintContext(Function);
  if (!Context)
    return std::unexpected(Context.error());

  std::cerr << "Reaching definitions for for machine function: " << Function.name() << '\n';
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    std::cerr << "%bb." << Block->denseNumber().value_or(0) << ":\n";
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const MachineOperand &Operand : Instruction->operands()) {
        if (Operand.isRegister()) {
          const std::optional<Register> Reg = Operand.registerValue();
          if (Operand.isDef() || !Reg || Reg->isNoRegister())
            continue;
        } else if (Operand.kind() != MachineOperand::Kind::FrameIndex) {
          continue;
        }

        auto PrintedOperand = printMachineOperand(Operand, Target, *Context);
        if (!PrintedOperand)
          return std::unexpected(PrintedOperand.error());
        std::cerr << *PrintedOperand << ":{ ";
        for (const MachineInstr *Definition :
             (*ReachingDefinitions)->reachingDefinitions(Operand)) {
          std::cerr << (*ReachingDefinitions)->instructionNumber(*Definition) << ' ';
        }
        std::cerr << "}\n";
      }

      auto PrintedInstruction = printMachineInstruction(*Instruction, Target, *Context);
      if (!PrintedInstruction)
        return std::unexpected(PrintedInstruction.error());
      std::cerr << (*ReachingDefinitions)->instructionNumber(*Instruction) << ": "
                << *PrintedInstruction << "\n\n";
    }
  }

  MachinePassResult Result;
  Result.Preserved = PreservedAnalyses::all();
  return Result;
}

} // namespace rcc::mir
