// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCopyPropagation.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &
copyPropagationContract(MachineCopyPropagationStage Stage) {
  static const MachinePassContract PreRA = [] {
    MachinePassContract Value{
        .Name = "pre-ra-machine-copy-propagation",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::TiedOpsRewritten,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers};
    return Value;
  }();
  static const MachinePassContract PostRA = [] {
    MachinePassContract Value{
        .Name = "post-ra-machine-copy-propagation",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::TracksLiveness},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Stage == MachineCopyPropagationStage::PreRegisterAllocation ? PreRA : PostRA;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
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

[[nodiscard]] std::optional<CopyOperands> copyOperands(MachineInstr &Instruction) {
  if (Instruction.opcode() != opcode(GenericOpcode::COPY) || Instruction.operandCount() != 2 ||
      hasProtectedAttachment(Instruction)) {
    return std::nullopt;
  }
  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  MachineOperand *SourceOperand = Instruction.operandAt(1);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || !DestinationOperand->isDef() ||
      !SourceOperand->isUse() || DestinationOperand->isImplicit() || SourceOperand->isImplicit()) {
    return std::nullopt;
  }
  return CopyOperands{DestinationOperand, SourceOperand, *Destination, *Source};
}

[[nodiscard]] Expected<bool> compatibleVirtualRegisters(Register Destination, Register Source,
                                                        const MachineFunction &Function,
                                                        const TargetDescription &Target) {
  if (!Destination.isVirtual() || !Source.isVirtual())
    return false;
  auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(Destination);
  auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!DestinationInfo || !SourceInfo)
    return std::unexpected(!DestinationInfo ? DestinationInfo.error() : SourceInfo.error());
  const VirtualRegisterInfo &DestinationValue = DestinationInfo->get();
  const VirtualRegisterInfo &SourceValue = SourceInfo->get();
  if (DestinationValue.Type != SourceValue.Type)
    return false;
  if (DestinationValue.Bank && SourceValue.Bank &&
      !Target.registers().registerBanksOverlap(*DestinationValue.Bank, *SourceValue.Bank)) {
    return false;
  }
  if (DestinationValue.RegClass && SourceValue.RegClass &&
      !Target.registers().isSubclass(*SourceValue.RegClass, *DestinationValue.RegClass)) {
    return false;
  }
  return true;
}

[[nodiscard]] Expected<bool> propagateVirtualCopy(MachineInstr &Copy, const CopyOperands &Operands,
                                                  MachineFunction &Function,
                                                  const TargetDescription &Target,
                                                  const TargetInstrInfo &Info) {
  if (!Operands.Destination.isVirtual() || !Operands.Source.isVirtual())
    return false;
  if (Function.registerInfo().getUniqueVRegDef(Operands.Destination) !=
      Operands.DestinationOperand) {
    return false;
  }
  auto Compatible =
      compatibleVirtualRegisters(Operands.Destination, Operands.Source, Function, Target);
  if (!Compatible)
    return std::unexpected(Compatible.error());
  if (!*Compatible)
    return false;
  auto ValidCopy =
      Info.validateCopy(Operands.Destination, Operands.DestinationOperand->subReg(),
                        Operands.Source, Operands.SourceOperand->subReg(), Function, Target);
  if (!ValidCopy) {
    if (ValidCopy.error().code() == ErrorCode::Unsupported)
      return false;
    return std::unexpected(ValidCopy.error());
  }

  std::vector<MachineOperand *> Uses;
  for (MachineOperand &Use : Function.registerInfo().useOperands(Operands.Destination, true)) {
    if (&Use == Operands.SourceOperand)
      continue;
    if (Use.tiedOperandIndex() || Use.isImplicit() ||
        Use.subReg() != Operands.DestinationOperand->subReg()) {
      return false;
    }
    Uses.push_back(&Use);
  }
  for (MachineOperand *Use : Uses) {
    auto Replaced = Use->setRegister(Operands.Source);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  MachineBasicBlock *Block = Copy.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "virtual COPY propagation candidate is detached");
  auto Removed = Block->removeInstruction(Copy);
  if (!Removed)
    return std::unexpected(Removed.error());
  if (!Function.registerInfo().useEmpty(Operands.Destination, true) ||
      Function.registerInfo().getUniqueVRegDef(Operands.Destination) != nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "virtual COPY propagation left destination references");
  }
  auto Erased = Function.registerInfo().eraseVirtualRegister(Operands.Destination);
  if (!Erased)
    return std::unexpected(Erased.error());
  return true;
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
                                         PhysicalRegister Source,
                                         const TargetDescription &Target) noexcept {
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
  return true;
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
  if (*Destination == *Source) {
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
      if (!Physical || !Operand->isUse() || Operand->isDebugUse() ||
          !Target.registers().regsOverlap(*Physical, *Destination) ||
          !canReplacePhysicalUse(*Instruction, Index, *Source, Target)) {
        continue;
      }
      auto Replaced = Operand->setRegister(Operands.Source);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      Changed = true;
    }
  }
  return Changed;
}

} // namespace

const MachinePassContract &MachineCopyPropagationPass::contract() const noexcept {
  return copyPropagationContract(Stage);
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
        prefixedError(Trial.error(), "COPY propagation transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(prefixedError(
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
      const std::optional<CopyOperands> Operands = copyOperands(*Instruction);
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
          Stage == MachineCopyPropagationStage::PreRegisterAllocation
              ? propagateVirtualCopy(*Instruction, *Operands, Function, Target, *Info)
              : propagatePhysicalCopy(*Instruction, *Operands, Function, Target, *Info);
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
  if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());
  }
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
  if (Stage == MachineCopyPropagationStage::PreRegisterAllocation)
    Result.Changes.add(MachinePassChange::Registers);
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
