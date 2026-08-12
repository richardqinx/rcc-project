// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/Legalizer.h"

#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &legalizerContract() {
  static const MachinePassContract Contract{
      .Name = "legalizer",
      // LLVM's GlobalISel legalizer operates on SSA form and does not require
      // the optional register-liveness tracking property.
      .Requires = {MachineFunctionProperty::IsSSA},
      .Sets = {MachineFunctionProperty::Legalized},
      .Clears = {},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::Legalized,
  };
  return Contract;
}

struct OpcodeState final {
  MachineInstrStableID Instruction;
  Opcode OpcodeValue;
  std::uint64_t OperandFingerprint = 0;

  friend bool operator==(const OpcodeState &Left, const OpcodeState &Right) noexcept = default;
};

struct OpcodeStateHash final {
  [[nodiscard]] std::size_t operator()(const OpcodeState &State) const noexcept {
    return std::hash<std::uint64_t>{}(State.Instruction.value()) ^
           (std::hash<std::uint32_t>{}(State.OpcodeValue.value()) << 1U) ^
           (std::hash<std::uint64_t>{}(State.OperandFingerprint) << 2U);
  }
};

void combineFingerprint(std::uint64_t &State, std::uint64_t Value) noexcept {
  State ^= Value + 0x9e3779b97f4a7c15ULL + (State << 6U) + (State >> 2U);
}

[[nodiscard]] std::uint64_t operandFingerprint(const MachineInstr &Instruction) noexcept {
  std::uint64_t Result = 0xcbf29ce484222325ULL;
  combineFingerprint(Result, Instruction.flags().rawValue());
  combineFingerprint(Result, Instruction.operandCount());
  for (const MachineOperand &Operand : Instruction.operands()) {
    combineFingerprint(Result, static_cast<std::uint64_t>(Operand.kind()));
    combineFingerprint(Result, Operand.targetFlags());
    combineFingerprint(Result, Operand.registerFlags().rawValue());
    combineFingerprint(Result, Operand.subReg().value());
    if (const Register *Reg = Operand.getIf<Register>())
      combineFingerprint(Result, Reg->opaqueValue());
    else if (const std::int64_t *Immediate = Operand.getIf<std::int64_t>())
      combineFingerprint(Result, static_cast<std::uint64_t>(*Immediate));
    else if (const auto *Integer = Operand.getIf<IntegerImmediateOperand>())
      combineFingerprint(Result, Integer->Value.bitWidth());
    else if (const auto *Floating = Operand.getIf<FloatingImmediateOperand>())
      combineFingerprint(Result, static_cast<std::uint64_t>(Floating->Bits.semantics()));
    else if (const auto *Block = Operand.getIf<BasicBlockOperand>())
      combineFingerprint(Result, Block->Block == nullptr ? 0 : Block->Block->stableID().value());
  }
  return Result;
}

[[nodiscard]] OpcodeState stateOf(const MachineInstr &Instruction) noexcept {
  return {Instruction.stableID(), Instruction.opcode(), operandFingerprint(Instruction)};
}

[[nodiscard]] std::vector<MachineInstr *> layoutInstructions(MachineFunction &Function) {
  std::vector<MachineInstr *> Result;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next())
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next())
      Result.push_back(Instruction);
  return Result;
}

[[nodiscard]] Expected<void> requireProperties(const MachineFunction &Function,
                                               const MachinePassContract &Contract) {
  const MachineFunctionProperties Missing = Contract.Requires.missingFrom(Function.properties());
  if (!Missing.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "pass '" + Contract.Name +
                              "' requires properties: " + Missing.toString());
  }
  return {};
}

[[nodiscard]] Error passFailure(std::string Message,
                                ErrorCode Code = ErrorCode::VerificationError) {
  return Error(Code, std::move(Message));
}

[[nodiscard]] Error prefixedError(const Error &Source, std::string Prefix) {
  Prefix.append(Source.message());
  return Error(Source.code(), std::move(Prefix));
}

[[nodiscard]] std::string_view legalizationActionName(LegalizationAction Action) noexcept {
  switch (Action) {
  case LegalizationAction::Legal:
    return "legal";
  case LegalizationAction::Unsupported:
    return "unsupported";
  case LegalizationAction::Custom:
    return "custom";
  case LegalizationAction::Lower:
    return "lower";
  case LegalizationAction::Libcall:
    return "libcall";
  case LegalizationAction::WidenScalar:
    return "widen-scalar";
  case LegalizationAction::NarrowScalar:
    return "narrow-scalar";
  case LegalizationAction::MoreElements:
    return "more-elements";
  case LegalizationAction::FewerElements:
    return "fewer-elements";
  case LegalizationAction::Bitcast:
    return "bitcast";
  case LegalizationAction::ChangeElementType:
    return "change-element-type";
  case LegalizationAction::ChangeAddressSpace:
    return "change-address-space";
  }
  return "unknown";
}

[[nodiscard]] Expected<void> clearTrackedLivenessFlags(MachineFunction &Function,
                                                       MachinePassResult &Result) {
  if (!Result.Changed || !Function.properties().has(MachineFunctionProperty::TracksLiveness))
    return {};
  bool Changed = false;
  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
      MachineOperand &Operand = *Instruction->operandAt(Index);
      if (!Operand.isRegister())
        continue;
      RegisterOperandFlags Flags = Operand.registerFlags();
      if (!Flags.has(RegisterOperandFlag::Kill) && !Flags.has(RegisterOperandFlag::Dead))
        continue;
      Flags.clear(RegisterOperandFlag::Kill);
      Flags.clear(RegisterOperandFlag::Dead);
      auto Updated = Operand.setRegisterFlags(Flags);
      if (!Updated)
        return std::unexpected(Updated.error());
      Changed = true;
    }
  }
  if (Changed)
    Result.Changes.add(MachinePassChange::Operands);
  return {};
}

} // namespace

const MachinePassContract &Legalizer::contract() const noexcept {
  return legalizerContract();
}

Expected<MachinePassResult> Legalizer::run(MachineFunction &Function,
                                           const TargetDescription &Target,
                                           MachineAnalysisManager &Analyses) {
  if (Info == nullptr)
    return std::unexpected(
        passFailure("legalizer has no target behavior interface", ErrorCode::InvalidArgument));
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(prefixedError(Trial.error(), "legalizer transaction setup failed: "));
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "legalizer diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult> Legalizer::runImplementation(MachineFunction &Function,
                                                         const TargetDescription &Target,
                                                         MachineActionReplay &Actions) {

  MachinePassResult Result;
  std::unordered_set<OpcodeState, OpcodeStateHash> SeenStates;

  std::vector<MachineInstr *> Worklist = layoutInstructions(Function);
  std::unordered_set<MachineInstrStableID> PendingInstructions;
  for (const MachineInstr *Instruction : Worklist)
    PendingInstructions.insert(Instruction->stableID());
  for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
    MachineInstr *Instruction = Worklist[Index];
    if (Instruction != nullptr)
      PendingInstructions.erase(Instruction->stableID());
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;

    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return std::unexpected(
          passFailure("legalizer encountered an unknown instruction descriptor"));
    if (!Descriptor->Flags.has(InstructionFlag::Generic))
      continue;

    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation)
      return std::unexpected(prefixedError(Validation.error(), "legalizer input is invalid: "));

    auto Action = Info->classify(*Instruction, Function, Target);
    if (!Action)
      return std::unexpected(Action.error());
    if (*Action == LegalizationAction::Legal)
      continue;
    if (*Action == LegalizationAction::Unsupported) {
      return std::unexpected(
          passFailure("target legalizer rejected generic instruction '" + Descriptor->Name + "'",
                      ErrorCode::Unsupported));
    }

    const OpcodeState Before = stateOf(*Instruction);
    if (!SeenStates.insert(Before).second)
      return std::unexpected(
          passFailure("target legalizer entered a repeated state for '" + Descriptor->Name + "'",
                      ErrorCode::InternalError));

    auto LoweringValidation = Info->validateLowering(*Instruction, Function, Target, *Action);
    if (!LoweringValidation)
      return std::unexpected(
          prefixedError(LoweringValidation.error(), "legalizer lowering precondition failed: "));
    std::string Rule = "legalize.";
    Rule.append(legalizationActionName(*Action));
    Rule.push_back('.');
    Rule.append(Descriptor->Name);
    auto Accepted = Actions.request(Rule, Function, Instruction->stableID());
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted) {
      return std::unexpected(
          passFailure("required legalizer action was rejected for '" + Descriptor->Name + "'",
                      ErrorCode::VerificationError));
    }
    auto Lowered = Info->lower(*Instruction, Function, Target, *Action);
    if (!Lowered)
      return std::unexpected(Lowered.error());
    const bool InstructionWasErased = Instruction->parent() == nullptr;
    if (!InstructionWasErased) {
      const OpcodeState After = stateOf(*Instruction);
      if (After == Before)
        return std::unexpected(passFailure("target legalizer made no observable progress for '" +
                                               Descriptor->Name + "'",
                                           ErrorCode::InternalError));
    }
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Instructions);

    const std::vector<MachineInstr *> Current = layoutInstructions(Function);
    for (MachineInstr *Candidate : Current) {
      const InstructionDescriptor *CandidateDescriptor =
          Target.instructionDescriptor(Candidate->opcode());
      if (CandidateDescriptor == nullptr)
        return std::unexpected(passFailure("legalizer produced an unknown instruction descriptor"));
      if (CandidateDescriptor->Flags.has(InstructionFlag::Generic) &&
          PendingInstructions.insert(Candidate->stableID()).second) {
        Worklist.push_back(Candidate);
      }
    }
  }

  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return std::unexpected(passFailure("legalizer produced an unknown instruction descriptor"));
    if (!Descriptor->Flags.has(InstructionFlag::Generic))
      continue;
    auto Action = Info->classify(*Instruction, Function, Target);
    if (!Action)
      return std::unexpected(Action.error());
    if (*Action != LegalizationAction::Legal)
      return std::unexpected(
          passFailure("legalizer reached a fixed point with an illegal generic instruction '" +
                          Descriptor->Name + "'",
                      *Action == LegalizationAction::Unsupported ? ErrorCode::Unsupported
                                                                 : ErrorCode::InternalError));
    auto FinalValidation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!FinalValidation)
      return std::unexpected(
          prefixedError(FinalValidation.error(), "legalizer produced invalid MachineIR: "));
  }
  auto Liveness = clearTrackedLivenessFlags(Function, Result);
  if (!Liveness)
    return std::unexpected(
        prefixedError(Liveness.error(), "legalizer liveness-flag invalidation failed: "));
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc
