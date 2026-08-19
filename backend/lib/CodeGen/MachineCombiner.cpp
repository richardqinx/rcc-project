// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCombiner.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &combinerContract(MachineCombinerStage Stage) {
  static const MachinePassContract PreLegalizer = [] {
    MachinePassContract Value{
        .Name = "riscv-prelegalizer-combiner",
        .Requires = {MachineFunctionProperty::IsSSA},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::MachineIRInput,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers};
    return Value;
  }();
  static const MachinePassContract SelectedSSA = [] {
    MachinePassContract Value{
        .Name = "machine-combiner-selected-ssa",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::IsSSA},
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
  static const MachinePassContract PreRA = [] {
    MachinePassContract Value{
        .Name = "machine-combiner-pre-ra",
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
        .Name = "machine-combiner-post-ra",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers};
    return Value;
  }();
  static const MachinePassContract PostFrame = [] {
    MachinePassContract Value{
        .Name = "machine-combiner-post-frame",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::PostRAFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Registers};
    return Value;
  }();
  switch (Stage) {
  case MachineCombinerStage::PreLegalizer:
    return PreLegalizer;
  case MachineCombinerStage::SelectedSSA:
    return SelectedSSA;
  case MachineCombinerStage::PreRegisterAllocation:
    return PreRA;
  case MachineCombinerStage::PostRegisterAllocation:
    return PostRA;
  case MachineCombinerStage::PostFrame:
    return PostFrame;
  }
  return PreLegalizer;
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.flags().has(MachineInstrFlag::FrameSetup) ||
         Instruction.flags().has(MachineInstrFlag::FrameDestroy) ||
         Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

[[nodiscard]] bool matchesPredicate(const MachineInstr &Instruction,
                                    const MachineCombinerPredicate &Predicate) {
  const MachineOperand *Operand = Instruction.operandAt(Predicate.Operand);
  if (Operand == nullptr)
    return false;
  switch (Predicate.Kind) {
  case MachineCombinerPredicateKind::ImmediateEquals: {
    const auto *Immediate = Operand->getIf<std::int64_t>();
    return Immediate != nullptr && *Immediate == Predicate.Value;
  }
  case MachineCombinerPredicateKind::RegisterEquals: {
    const MachineOperand *Other = Instruction.operandAt(Predicate.OtherOperand);
    const Register *LeftReg = Operand->getIf<Register>();
    const Register *RightReg = Other == nullptr ? nullptr : Other->getIf<Register>();
    return LeftReg != nullptr && RightReg != nullptr && *LeftReg == *RightReg &&
           Operand->subReg() == Other->subReg();
  }
  case MachineCombinerPredicateKind::PhysicalRegisterEquals: {
    const Register *Reg = Operand->getIf<Register>();
    const std::optional<PhysicalRegister> Physical =
        Reg == nullptr ? std::nullopt : Reg->physicalRegister();
    return Physical && Physical->number() == static_cast<std::uint32_t>(Predicate.Value);
  }
  case MachineCombinerPredicateKind::RegisterIsVirtual: {
    const Register *Reg = Operand->getIf<Register>();
    return Reg != nullptr && Reg->isVirtual();
  }
  case MachineCombinerPredicateKind::RegisterIsPhysical: {
    const Register *Reg = Operand->getIf<Register>();
    return Reg != nullptr && Reg->isPhysical();
  }
  }
  return false;
}

[[nodiscard]] bool matchesPattern(const MachineInstr &Instruction,
                                  const MachineCombinerPattern &Pattern,
                                  MachineCombinerStage Stage) {
  if ((Pattern.Stages & machineCombinerStageMask(Stage)) == 0)
    return false;
  for (const MachineCombinerPredicate &Predicate : Pattern.Predicates) {
    if (!matchesPredicate(Instruction, Predicate))
      return false;
  }
  return true;
}

[[nodiscard]] bool compatibleReplacement(Register Destination, Register Source,
                                         const MachineFunction &Function,
                                         const TargetDescription &Target) {
  if (!Destination.isVirtual() || Source.isNoRegister())
    return false;
  auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(Destination);
  if (!DestinationInfo)
    return false;
  const VirtualRegisterInfo &DestinationValue = DestinationInfo->get();
  if (Source.isPhysical()) {
    const std::optional<PhysicalRegister> Physical = Source.physicalRegister();
    if (!Physical)
      return false;
    if (DestinationValue.RegClass &&
        !Target.registers().registerClassContains(*DestinationValue.RegClass, *Physical)) {
      return false;
    }
    return !DestinationValue.Bank ||
           Target.registers().bankContainsRegister(*DestinationValue.Bank, *Physical);
  }
  auto SourceInfo = Function.registerInfo().virtualRegisterInfo(Source);
  if (!SourceInfo)
    return false;
  const VirtualRegisterInfo &SourceValue = SourceInfo->get();
  if (DestinationValue.Type != SourceValue.Type)
    return false;
  if (DestinationValue.Bank &&
      (!SourceValue.Bank ||
       !Target.registers().registerBanksOverlap(*DestinationValue.Bank, *SourceValue.Bank))) {
    return false;
  }
  if (DestinationValue.RegClass &&
      (!SourceValue.RegClass || !Target.registers().registerClassesOverlap(
                                    *DestinationValue.RegClass, *SourceValue.RegClass))) {
    return false;
  }
  return true;
}

[[nodiscard]] Expected<bool> executeReplacement(MachineInstr &Instruction,
                                                const MachineCombinerAction &Action,
                                                MachineFunction &Function,
                                                const TargetDescription &Target) {
  MachineOperand *Definition = Instruction.operandAt(Action.DefinitionOperand);
  const MachineOperand *SourceOperand = Instruction.operandAt(Action.SourceOperand);
  const Register *Destination = Definition == nullptr ? nullptr : Definition->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || !Definition->isDef() || Definition->isUse() ||
      !SourceOperand->isUse() || Definition->subReg().value() != 0 ||
      SourceOperand->subReg().value() != 0 || Destination->isNoRegister() ||
      Source->isNoRegister()) {
    return false;
  }
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "machine combiner root is detached");

  if (*Destination == *Source) {
    auto Removed = Block->removeInstruction(Instruction);
    if (!Removed)
      return std::unexpected(Removed.error());
    return true;
  }
  if (!Destination->isVirtual() ||
      Function.registerInfo().getUniqueVRegDef(*Destination) != Definition ||
      !compatibleReplacement(*Destination, *Source, Function, Target)) {
    return false;
  }

  std::vector<MachineOperand *> Uses;
  for (MachineOperand &Use : Function.registerInfo().useOperands(*Destination, true))
    Uses.push_back(&Use);
  for (MachineOperand *Use : Uses) {
    auto Replaced = Use->setRegister(*Source);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  auto Removed = Block->removeInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  if (!Function.registerInfo().useEmpty(*Destination, true) ||
      Function.registerInfo().getUniqueVRegDef(*Destination) != nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "machine combiner left references to a replaced result");
  }
  auto Erased = Function.registerInfo().eraseVirtualRegister(*Destination);
  if (!Erased)
    return std::unexpected(Erased.error());
  return true;
}

[[nodiscard]] Expected<bool> executePattern(MachineInstr &Instruction,
                                            const MachineCombinerPattern &Pattern,
                                            MachineFunction &Function,
                                            const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "machine combiner root has no descriptor");
  const InstructionFlags Flags = Descriptor->Flags;
  if (Flags.has(InstructionFlag::Terminator) || Flags.has(InstructionFlag::Call) ||
      Flags.has(InstructionFlag::Barrier) || Flags.has(InstructionFlag::MayLoad) ||
      Flags.has(InstructionFlag::MayStore) || Flags.has(InstructionFlag::HasSideEffects) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) || !Instruction.memOperands().empty() ||
      hasProtectedAttachment(Instruction)) {
    return false;
  }
  switch (Pattern.Action.Kind) {
  case MachineCombinerActionKind::ReplaceDefWithOperandAndErase:
    return executeReplacement(Instruction, Pattern.Action, Function, Target);
  }
  return false;
}

} // namespace

const MachinePassContract &MachineCombinerPass::contract() const noexcept {
  return combinerContract(Stage);
}

Expected<MachinePassResult> MachineCombinerPass::run(MachineFunction &Function,
                                                     const TargetDescription &Target,
                                                     MachineFunctionAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine combiner input does not satisfy its stage contract");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "machine combiner transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "machine combiner diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult>
MachineCombinerPass::runImplementation(MachineFunction &Function, const TargetDescription &Target,
                                       MachineActionReplay &Actions) const {
  if (Options.MaximumIterations == 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine combiner iteration budget must be nonzero");
  MachinePassResult Result;
  std::size_t Rewrites = 0;
  for (std::size_t Iteration = 0; Iteration < Options.MaximumIterations; ++Iteration) {
    bool IterationChanged = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
        MachineInstr *Next = Instruction->next();
        for (const MachineCombinerPattern &Pattern : Info->patterns(Instruction->opcode())) {
          if (!matchesPattern(*Instruction, Pattern, Stage))
            continue;
          if (Rewrites == Options.MaximumRewrites)
            break;
          std::string Rule = "combine.";
          Rule.append(Pattern.Name);
          auto Accepted = Actions.request(Rule, Function, Instruction->stableID());
          if (!Accepted)
            return std::unexpected(Accepted.error());
          if (!*Accepted)
            continue;
          auto Executed = executePattern(*Instruction, Pattern, Function, Target);
          if (!Executed)
            return std::unexpected(Executed.error());
          if (!*Executed)
            continue;
          ++Rewrites;
          IterationChanged = true;
          Result.Changed = true;
          break;
        }
        Instruction = Next;
      }
    }
    if (!IterationChanged || Rewrites == Options.MaximumRewrites)
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
