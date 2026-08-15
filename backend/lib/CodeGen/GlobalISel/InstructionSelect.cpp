// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/InstructionSelect.h"

#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &instructionSelectContract() {
  static const MachinePassContract Contract{
      .Name = "instruction-select",
      .Requires = {MachineFunctionProperty::Legalized, MachineFunctionProperty::RegBankSelected},
      .Sets = {MachineFunctionProperty::Selected},
      .Clears = {},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::Selected,
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

[[nodiscard]] std::vector<MachineBasicBlock *> stableCFGPostOrder(MachineFunction &Function) {
  std::vector<MachineBasicBlock *> Roots;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next())
    Roots.push_back(Block);
  std::ranges::sort(Roots, {},
                    [](const MachineBasicBlock *Block) { return Block->stableID().value(); });
  if (MachineBasicBlock *Entry = Function.firstBlock(); Entry != nullptr) {
    const auto Position = std::ranges::find(Roots, Entry);
    if (Position != Roots.end())
      std::rotate(Roots.begin(), Position, std::next(Position));
  }

  struct TraversalFrame final {
    MachineBasicBlock *Block = nullptr;
    std::vector<MachineBasicBlock *> Successors;
    std::size_t NextSuccessor = 0;
  };
  const auto MakeFrame = [](MachineBasicBlock &Block) {
    TraversalFrame Frame;
    Frame.Block = &Block;
    for (const MachineSuccessor &Successor : Block.successors()) {
      if (Successor.Block != nullptr)
        Frame.Successors.push_back(Successor.Block);
    }
    std::ranges::sort(Frame.Successors, {}, [](const MachineBasicBlock *Successor) {
      return Successor->stableID().value();
    });
    return Frame;
  };

  std::vector<MachineBasicBlock *> Result;
  std::unordered_set<MachineBasicBlock *> Visited;
  std::vector<TraversalFrame> Stack;
  for (MachineBasicBlock *Root : Roots) {
    if (!Visited.insert(Root).second)
      continue;
    Stack.push_back(MakeFrame(*Root));
    while (!Stack.empty()) {
      TraversalFrame &Frame = Stack.back();
      if (Frame.NextSuccessor < Frame.Successors.size()) {
        MachineBasicBlock *Successor = Frame.Successors[Frame.NextSuccessor++];
        if (Visited.insert(Successor).second)
          Stack.push_back(MakeFrame(*Successor));
        continue;
      }
      Result.push_back(Frame.Block);
      Stack.pop_back();
    }
  }
  return Result;
}

[[nodiscard]] std::vector<MachineInstr *> bottomUpInstructions(MachineFunction &Function) {
  std::vector<MachineInstr *> Result;
  for (MachineBasicBlock *Block : stableCFGPostOrder(Function)) {
    for (MachineInstr *Instruction = Block->lastInstruction(); Instruction != nullptr;
         Instruction = Instruction->previous())
      Result.push_back(Instruction);
  }
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

[[nodiscard]] Expected<void> repairTrackedLiveness(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   MachinePassResult &Result) {
  if (!Result.Changed || !Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    return {};
  }
  auto Rebuilt = rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  if (*Rebuilt)
    Result.Changes.add(MachinePassChange::Operands);
  return {};
}

} // namespace

const MachinePassContract &InstructionSelect::contract() const noexcept {
  return instructionSelectContract();
}

Expected<MachinePassResult> InstructionSelect::run(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   MachineFunctionAnalysisManager &Analyses) {
  if (Function.properties().has(MachineFunctionProperty::FailedISel))
    return MachinePassResult{
        .Changed = false,
        .Changes = {},
        .Preserved = PreservedAnalyses::all(),
    };
  if (Info == nullptr)
    return std::unexpected(passFailure("instruction select has no target behavior interface",
                                       ErrorCode::InvalidArgument));
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "instruction-selection transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(),
        "instruction selection diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult> InstructionSelect::runImplementation(MachineFunction &Function,
                                                                 const TargetDescription &Target,
                                                                 MachineActionReplay &Actions) {

  MachinePassResult Result;
  std::vector<MachineInstr *> Worklist;
  std::unordered_set<MachineInstrStableID> DiscoveredInstructions;
  std::unordered_set<OpcodeState, OpcodeStateHash> SeenStates;
  for (MachineInstr *Instruction : bottomUpInstructions(Function)) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return std::unexpected(
          passFailure("instruction selector encountered an unknown instruction descriptor"));
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation)
      return std::unexpected(
          withErrorContext(Validation.error(), "instruction selector input is invalid: "));
    if (!Descriptor->Flags.has(InstructionFlag::Generic))
      continue;
    auto Match = Info->canSelect(*Instruction, Function, Target);
    if (!Match)
      return std::unexpected(Match.error());
    Worklist.push_back(Instruction);
    DiscoveredInstructions.insert(Instruction->stableID());
  }

  for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
    MachineInstr *Instruction = Worklist[Index];
    if (Instruction->parent() == nullptr)
      return std::unexpected(passFailure("instruction selector lost its input instruction"));
    const InstructionDescriptor *CurrentDescriptor =
        Target.instructionDescriptor(Instruction->opcode());
    if (CurrentDescriptor == nullptr)
      return std::unexpected(
          passFailure("instruction selector encountered an unknown instruction descriptor"));
    if (!CurrentDescriptor->Flags.has(InstructionFlag::Generic))
      continue;
    const OpcodeState Before = stateOf(*Instruction);
    if (!SeenStates.insert(Before).second) {
      return std::unexpected(passFailure("instruction selector entered a repeated state for '" +
                                             CurrentDescriptor->Name + "'",
                                         ErrorCode::InternalError));
    }
    auto Match = Info->canSelect(*Instruction, Function, Target);
    if (!Match)
      return std::unexpected(Match.error());
    std::string Rule = "select.";
    Rule.append(CurrentDescriptor->Name);
    auto Accepted = Actions.request(Rule, Function, Instruction->stableID());
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted) {
      return std::unexpected(passFailure("required selection action was rejected for '" +
                                             CurrentDescriptor->Name + "'",
                                         ErrorCode::VerificationError));
    }
    auto Selection = Info->select(*Instruction, Function, Target);
    if (!Selection)
      return std::unexpected(Selection.error());
    const InstructionDescriptor *Selected = Target.instructionDescriptor(Instruction->opcode());
    if (Selected == nullptr)
      return std::unexpected(
          passFailure("instruction selector produced an unknown instruction descriptor"));
    auto Constraint = Info->constrainSelectedInstruction(*Instruction, Function, Target);
    if (!Constraint) {
      return std::unexpected(withErrorContext(
          Constraint.error(), "instruction selector could not constrain selected registers: "));
    }
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation)
      return std::unexpected(withErrorContext(Validation.error(),
                                              "instruction selector produced invalid MachineIR: "));
    const OpcodeState After = stateOf(*Instruction);
    if (After == Before)
      return std::unexpected(
          passFailure("instruction selector reported success without observable progress",
                      ErrorCode::InternalError));
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Instructions);
    if (Selected->Flags.has(InstructionFlag::Generic))
      Worklist.push_back(Instruction);
    for (MachineInstr *Candidate : bottomUpInstructions(Function)) {
      const InstructionDescriptor *CandidateDescriptor =
          Target.instructionDescriptor(Candidate->opcode());
      if (CandidateDescriptor == nullptr)
        return std::unexpected(
            passFailure("instruction selector produced an unknown instruction descriptor"));
      if (!CandidateDescriptor->Flags.has(InstructionFlag::Generic) ||
          !DiscoveredInstructions.insert(Candidate->stableID()).second) {
        continue;
      }
      auto CandidateMatch = Info->canSelect(*Candidate, Function, Target);
      if (!CandidateMatch)
        return std::unexpected(CandidateMatch.error());
      Worklist.push_back(Candidate);
    }
  }
  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return std::unexpected(
          passFailure("instruction selector produced an unknown instruction descriptor"));
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation)
      return std::unexpected(withErrorContext(Validation.error(),
                                              "instruction selector produced invalid MachineIR: "));
    if (Descriptor->Flags.has(InstructionFlag::Generic))
      return std::unexpected(
          passFailure("instruction selector left a generic opcode in the function"));
  }
  auto RegisterValidation = Target.validateRegisterInfo(Function.registerInfo());
  if (!RegisterValidation)
    return std::unexpected(withErrorContext(RegisterValidation.error(),
                                            "instruction selection produced invalid registers: "));
  auto Liveness = repairTrackedLiveness(Function, Target, Result);
  if (!Liveness) {
    return std::unexpected(
        withErrorContext(Liveness.error(), "instruction-selection liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}
} // namespace rcc
