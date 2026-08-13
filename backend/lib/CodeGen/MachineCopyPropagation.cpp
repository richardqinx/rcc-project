// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCopyPropagation.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &copyPropagationContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-cp",
        .Requires = {MachineFunctionProperty::NoVRegs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = std::nullopt,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value() ||
         Instruction.flags().has(MachineInstrFlag::BundledWithPred) ||
         Instruction.flags().has(MachineInstrFlag::BundledWithSucc);
}

struct CopyOperands final {
  MachineOperand *DestinationOperand = nullptr;
  MachineOperand *SourceOperand = nullptr;
  Register Destination;
  Register Source;
};

[[nodiscard]] std::optional<CopyOperands> copyOperands(MachineInstr &Instruction,
                                                       const TargetInstrInfo &Info,
                                                       bool UseTargetCopyInstructions) {
  if (hasProtectedAttachment(Instruction))
    return std::nullopt;
  MachineOperand *DestinationOperand = nullptr;
  MachineOperand *SourceOperand = nullptr;
  if (UseTargetCopyInstructions) {
    const std::optional<MachineCopyOperands> TargetCopy = Info.getCopyOperands(Instruction);
    if (!TargetCopy)
      return std::nullopt;
    DestinationOperand = TargetCopy->Destination;
    SourceOperand = TargetCopy->Source;
  } else {
    if (Instruction.opcode() != opcode(GenericOpcode::COPY) || Instruction.operandCount() < 2)
      return std::nullopt;
    DestinationOperand = Instruction.operandAt(0);
    SourceOperand = Instruction.operandAt(1);
  }
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || !DestinationOperand->isDef() ||
      !SourceOperand->isUse() || DestinationOperand->isImplicit() || SourceOperand->isImplicit()) {
    return std::nullopt;
  }
  return CopyOperands{DestinationOperand, SourceOperand, *Destination, *Source};
}

[[nodiscard]] bool shareRegisterClass(PhysicalRegister Left, PhysicalRegister Right,
                                      const TargetRegisterInfo &Registers) noexcept {
  return std::ranges::any_of(Registers.registerClasses(), [&](const RegisterClassDesc &Class) {
    return Registers.registerClassContains(Class.ID, Left) &&
           Registers.registerClassContains(Class.ID, Right);
  });
}

[[nodiscard]] bool clobbersRegister(const MachineInstr &Instruction, PhysicalRegister Reg,
                                    const TargetRegisterInfo &Registers) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (const auto *Mask = Operand.getIf<RegisterMaskOperand>()) {
      if (Mask->Mask && Mask->Mask->clobbers(Reg))
        return true;
      continue;
    }
    const Register *OperandReg = Operand.getIf<Register>();
    const std::optional<PhysicalRegister> Physical =
        OperandReg == nullptr ? std::nullopt : OperandReg->physicalRegister();
    if (Physical && Operand.isDef() && Registers.regsOverlap(*Physical, Reg))
      return true;
  }
  return false;
}

[[nodiscard]] bool canReplacePhysicalUse(const MachineInstr &Instruction, std::size_t OperandIndex,
                                         PhysicalRegister Source, const MachineFunction &Function,
                                         const TargetDescription &Target,
                                         const TargetInstrInfo &Info) noexcept {
  const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Operand == nullptr || Descriptor == nullptr || Operand->isImplicit() ||
      Operand->tiedOperandIndex() || Operand->subReg().value() != 0 ||
      OperandIndex >= Descriptor->Operands.size()) {
    return false;
  }
  const OperandDescriptor &Constraint = Descriptor->Operands[OperandIndex];
  if (Constraint.RegClass &&
      !Target.registers().registerClassContains(*Constraint.RegClass, Source)) {
    return false;
  }
  if (Constraint.RegBank && !Target.registers().bankContainsRegister(*Constraint.RegBank, Source)) {
    return false;
  }
  if (Instruction.opcode() == opcode(GenericOpcode::COPY)) {
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return false;
    auto Valid =
        Info.validateCopy(*Destination, DestinationOperand->subReg(),
                          Register::fromPhysical(Source), Operand->subReg(), Function, Target);
    if (!Valid)
      return false;
  }
  return true;
}

[[nodiscard]] bool valueUsedAfter(const MachineInstr &Copy, PhysicalRegister Destination,
                                  const TargetDescription &Target) noexcept {
  for (const MachineInstr *Instruction = Copy.next(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    for (const MachineOperand &Operand : Instruction->operands()) {
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>()) {
        if (Mask->Mask && Mask->Mask->clobbers(Destination))
          return false;
        continue;
      }
      const Register *Reg = Operand.getIf<Register>();
      const std::optional<PhysicalRegister> Physical =
          Reg == nullptr ? std::nullopt : Reg->physicalRegister();
      if (!Physical || !Target.registers().regsOverlap(*Physical, Destination))
        continue;
      if (Operand.isUse())
        return true;
      if (Operand.isDef())
        return false;
    }
  }
  const MachineBasicBlock *Block = Copy.parent();
  if (Block == nullptr)
    return true;
  if (!Block->successors().empty() &&
      !Copy.function().properties().has(MachineFunctionProperty::TracksLiveness)) {
    return true;
  }
  return std::ranges::any_of(Block->successors(), [&](const MachineSuccessor &Successor) {
    return Successor.Block != nullptr &&
           std::ranges::any_of(Successor.Block->liveIns(), [&](const MachineBasicBlockLiveIn &In) {
             return Target.registers().regsOverlap(In.PhysReg, Destination);
           });
  });
}

[[nodiscard]] Expected<bool> propagatePhysicalCopy(MachineInstr &Copy, const CopyOperands &Operands,
                                                   MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   const TargetInstrInfo &Info) {
  const std::optional<PhysicalRegister> Destination = Operands.Destination.physicalRegister();
  const std::optional<PhysicalRegister> Source = Operands.Source.physicalRegister();
  if (!Destination || !Source || Operands.DestinationOperand->subReg().value() != 0 ||
      Operands.SourceOperand->subReg().value() != 0) {
    return false;
  }
  auto ValidCopy =
      Info.validateCopy(Operands.Destination, {}, Operands.Source, {}, Function, Target);
  if (!ValidCopy) {
    if (ValidCopy.error().code() == ErrorCode::Unsupported)
      return false;
    return std::unexpected(ValidCopy.error());
  }
  MachineBasicBlock *Block = Copy.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "physical COPY propagation candidate is detached");
  const bool IsGenericCopy = Copy.opcode() == opcode(GenericOpcode::COPY);
  if (*Destination == *Source && IsGenericCopy) {
    auto Removed = Block->removeInstruction(Copy);
    if (!Removed)
      return std::unexpected(Removed.error());
    return true;
  }
  if (!shareRegisterClass(*Destination, *Source, Target.registers()))
    return false;

  bool Changed = false;
  for (MachineInstr *Instruction = Copy.next(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (clobbersRegister(*Instruction, *Destination, Target.registers()) ||
        clobbersRegister(*Instruction, *Source, Target.registers())) {
      break;
    }
    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      MachineOperand *Operand = Instruction->operandAt(Index);
      const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
      const std::optional<PhysicalRegister> Physical =
          Reg == nullptr ? std::nullopt : Reg->physicalRegister();
      if (!Physical || *Physical != *Destination || !Operand->isUse() || Operand->isDebugUse() ||
          !Operand->registerFlags().has(RegisterOperandFlag::Renamable) ||
          !canReplacePhysicalUse(*Instruction, Index, *Source, Function, Target, Info)) {
        continue;
      }
      auto Replaced = Operand->setRegister(Operands.Source);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      RegisterOperandFlags Flags = Operand->registerFlags();
      Flags.clear(RegisterOperandFlag::Kill);
      if (!Operands.SourceOperand->registerFlags().has(RegisterOperandFlag::Renamable))
        Flags.clear(RegisterOperandFlag::Renamable);
      if (Operands.SourceOperand->registerFlags().has(RegisterOperandFlag::Undef))
        Flags.set(RegisterOperandFlag::Undef);
      else
        Flags.clear(RegisterOperandFlag::Undef);
      auto SetFlags = Operand->setRegisterFlags(Flags);
      if (!SetFlags)
        return std::unexpected(SetFlags.error());
      Changed = true;
    }
  }
  if (IsGenericCopy &&
      Operands.DestinationOperand->registerFlags().has(RegisterOperandFlag::Renamable) &&
      !valueUsedAfter(Copy, *Destination, Target)) {
    auto Removed = Block->removeInstruction(Copy);
    if (!Removed)
      return std::unexpected(Removed.error());
    Changed = true;
  }
  return Changed;
}

} // namespace

const MachinePassContract &MachineCopyPropagationPass::contract() const noexcept {
  return copyPropagationContract();
}

Expected<MachinePassResult>
MachineCopyPropagationPass::run(MachineFunction &Function, const TargetDescription &Target,
                                MachineFunctionAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine COPY propagation has unmet stage properties");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "COPY propagation transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "COPY propagation diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult>
MachineCopyPropagationPass::runImplementation(MachineFunction &Function,
                                              const TargetDescription &Target,
                                              MachineActionReplay &Actions) const {
  MachinePassResult Result;
  std::size_t PropagatedCopies = 0;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction();
         Instruction != nullptr && PropagatedCopies < Options.MaximumPropagatedCopies;) {
      MachineInstr *Next = Instruction->next();
      auto Simplified = Info->simplifyInstruction(*Instruction, Function, Target);
      if (!Simplified)
        return std::unexpected(Simplified.error());
      if (*Simplified)
        Result.Changed = true;
      const std::optional<CopyOperands> Operands =
          copyOperands(*Instruction, *Info, Options.UseTargetCopyInstructions);
      if (!Operands) {
        Instruction = Next;
        continue;
      }
      auto Accepted = Actions.request("propagate-copy", Function, Instruction->stableID());
      if (!Accepted)
        return std::unexpected(Accepted.error());
      if (!*Accepted) {
        Instruction = Next;
        continue;
      }
      Expected<bool> Changed =
          propagatePhysicalCopy(*Instruction, *Operands, Function, Target, *Info);
      if (!Changed)
        return std::unexpected(Changed.error());
      if (*Changed) {
        ++PropagatedCopies;
        Result.Changed = true;
      }
      Instruction = Next;
    }
  }

  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
