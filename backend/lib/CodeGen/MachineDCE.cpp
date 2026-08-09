// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineDCE.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &dceContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-dce",
        .Requires = {MachineFunctionProperty::Selected},
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

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.flags().has(MachineInstrFlag::FrameSetup) ||
         Instruction.flags().has(MachineInstrFlag::FrameDestroy) ||
         Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

[[nodiscard]] Expected<std::optional<std::vector<Register>>>
deadDefinitions(MachineInstr &Instruction, MachineRegisterInfo &Registers,
                const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine DCE encountered an instruction without a descriptor");
  }
  const InstructionFlags Flags = Descriptor->Flags;
  if (Flags.has(InstructionFlag::Terminator) || Flags.has(InstructionFlag::Branch) ||
      Flags.has(InstructionFlag::Call) || Flags.has(InstructionFlag::Return) ||
      Flags.has(InstructionFlag::Barrier) || Flags.has(InstructionFlag::MayStore) ||
      Flags.has(InstructionFlag::HasSideEffects) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) || hasProtectedAttachment(Instruction)) {
    return std::nullopt;
  }

  std::vector<Register> VirtualDefinitions;
  bool HasDefinition = false;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Operand.isDef())
      continue;
    HasDefinition = true;
    if (Operand.isUse() || Operand.tiedOperandIndex())
      return std::nullopt;
    if (Reg->isVirtual()) {
      if (!Registers.useEmpty(*Reg, false))
        return std::nullopt;
      VirtualDefinitions.push_back(*Reg);
      continue;
    }
    if (!Reg->isPhysical() || !Operand.registerFlags().has(RegisterOperandFlag::Dead)) {
      return std::nullopt;
    }
  }
  if (!HasDefinition)
    return std::nullopt;
  return VirtualDefinitions;
}

[[nodiscard]] Expected<void> clearDebugUses(MachineRegisterInfo &Registers, Register Reg) {
  std::vector<MachineOperand *> Uses;
  for (MachineOperand &Operand : Registers.useOperands(Reg, true)) {
    if (Operand.isDebugUse())
      Uses.push_back(&Operand);
  }
  for (MachineOperand *Operand : Uses) {
    auto Cleared = Operand->setRegister(Register{});
    if (!Cleared)
      return std::unexpected(Cleared.error());
  }
  return {};
}

} // namespace

const MachinePassContract &MachineDCEPass::contract() const noexcept {
  return dceContract();
}

Expected<MachinePassResult> MachineDCEPass::run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine DCE requires a selected machine function");
  }

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(prefixedError(Trial.error(), "machine DCE transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "machine DCE diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult> MachineDCEPass::runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target) {
  MachinePassResult Result;
  MachineRegisterInfo &Registers = Function.registerInfo();

  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
        MachineInstr *Next = Instruction->next();
        auto Definitions = deadDefinitions(*Instruction, Registers, Target);
        if (!Definitions)
          return std::unexpected(Definitions.error());
        if (!*Definitions) {
          Instruction = Next;
          continue;
        }
        std::vector<Register> DefinitionList =
            std::move(*Definitions).value_or(std::vector<Register>{});

        for (const Register Reg : DefinitionList) {
          auto Cleared = clearDebugUses(Registers, Reg);
          if (!Cleared)
            return std::unexpected(Cleared.error());
        }
        auto Removed = Block->removeInstruction(*Instruction);
        if (!Removed)
          return std::unexpected(Removed.error());
        for (const Register Reg : DefinitionList) {
          if (!Registers.useEmpty(Reg, true) || Registers.getUniqueVRegDef(Reg) != nullptr) {
            return makeUnexpected(ErrorCode::InternalError,
                                  "machine DCE left references to a removed definition");
          }
          auto Erased = Registers.eraseVirtualRegister(Reg);
          if (!Erased)
            return std::unexpected(Erased.error());
        }
        LocalChange = true;
        Result.Changed = true;
        Instruction = Next;
      }
    }
  }

  if (Result.Changed) {
    if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
      auto Rebuilt = rebuildMachineLiveness(Function, Target);
      if (!Rebuilt)
        return std::unexpected(Rebuilt.error());
    }
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                      MachinePassChange::Registers};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
