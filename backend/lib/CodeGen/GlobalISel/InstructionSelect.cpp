// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/InstructionSelect.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstdint>
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

[[nodiscard]] Error passFailure(std::string Message,
                                ErrorCode Code = ErrorCode::VerificationError) {
  return Error(Code, std::move(Message));
}

} // namespace

Expected<void> InstructionSelector::propagateSelectionFlags(MachineInstr &Output,
                                                            MachineInstrFlags InputFlags,
                                                            bool InputMayRaiseFPException,
                                                            const TargetDescription &Target) const {
  const InstructionDescriptor *OutputDescriptor = Target.instructionDescriptor(Output.opcode());
  if (OutputDescriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction selector produced an unknown instruction descriptor");
  }
  if (!InputMayRaiseFPException &&
      OutputDescriptor->Flags.has(InstructionFlag::MayRaiseFPException)) {
    InputFlags.set(MachineInstrFlag::NoFPExcept);
  }
  Output.setFlags(InputFlags);
  return {};
}

Expected<void> InstructionSelector::propagateSelectionFlags(MachineInstr &Output,
                                                            const MachineInstr &Input,
                                                            const TargetDescription &Target) const {
  const InstructionDescriptor *InputDescriptor = Target.instructionDescriptor(Input.opcode());
  if (InputDescriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "instruction selector input has an unknown instruction descriptor");
  }
  return propagateSelectionFlags(Output, Input.flags(),
                                 InputDescriptor->Flags.has(InstructionFlag::MayRaiseFPException),
                                 Target);
}

Expected<void> InstructionSelector::finalizeGeneratedInstruction(
    MachineInstr &Output, MachineFunction &Function, const TargetDescription &Target,
    MachineInstrFlags InputFlags, bool InputMayRaiseFPException) const {
  auto Flags = propagateSelectionFlags(Output, InputFlags, InputMayRaiseFPException, Target);
  if (!Flags)
    return std::unexpected(Flags.error());
  return constrainSelectedInstruction(Output, Function, Target);
}

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
  return runImplementation(Function, Target, Analyses);
}

Expected<MachinePassResult>
InstructionSelect::runImplementation(MachineFunction &Function, const TargetDescription &Target,
                                     MachineFunctionAnalysisManager &Analyses) {

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
    Worklist.push_back(Instruction);
    DiscoveredInstructions.insert(Instruction->stableID());
  }
  const auto DiscoverNewInstructions = [&]() -> Expected<void> {
    for (MachineInstr *Candidate : bottomUpInstructions(Function)) {
      const InstructionDescriptor *CandidateDescriptor =
          Target.instructionDescriptor(Candidate->opcode());
      if (CandidateDescriptor == nullptr) {
        return std::unexpected(
            passFailure("instruction selector produced an unknown instruction descriptor"));
      }
      if (DiscoveredInstructions.insert(Candidate->stableID()).second)
        Worklist.push_back(Candidate);
    }
    return {};
  };

  for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
    MachineInstr *Instruction = Worklist[Index];
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;
    if (isTriviallyDead(*Instruction, Function.registerInfo(), Target)) {
      auto Removed = eraseInstr(*Instruction, Function.registerInfo(), Target);
      if (!Removed)
        return std::unexpected(Removed.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
      continue;
    }
    const InstructionDescriptor *CurrentDescriptor =
        Target.instructionDescriptor(Instruction->opcode());
    if (CurrentDescriptor == nullptr)
      return std::unexpected(
          passFailure("instruction selector encountered an unknown instruction descriptor"));
    const bool IsGeneric = CurrentDescriptor->Flags.has(InstructionFlag::Generic);
    const bool IsCopy = Instruction->opcode() == opcode(GenericOpcode::COPY);
    if (!IsGeneric && !IsCopy)
      continue;
    const OpcodeState Before = stateOf(*Instruction);
    if (!SeenStates.insert(Before).second) {
      return std::unexpected(passFailure("instruction selector entered a repeated state for '" +
                                             CurrentDescriptor->Name + "'",
                                         ErrorCode::InternalError));
    }
    std::string Rule = "select.";
    Rule.append(CurrentDescriptor->Name);
    auto Accepted = Analyses.requestAction(Rule, Function, Instruction->stableID());
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted) {
      return std::unexpected(passFailure("required selection action was rejected for '" +
                                             CurrentDescriptor->Name + "'",
                                         ErrorCode::VerificationError));
    }
    const MachineInstrFlags InputFlags = Instruction->flags();
    const bool InputMayRaiseFPException =
        CurrentDescriptor->Flags.has(InstructionFlag::MayRaiseFPException);
    auto Selection = Info->select(*Instruction, Function, Target);
    if (!Selection)
      return std::unexpected(Selection.error());
    if (Instruction->parent() == nullptr) {
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
      auto Discovered = DiscoverNewInstructions();
      if (!Discovered)
        return std::unexpected(Discovered.error());
      continue;
    }
    const InstructionDescriptor *Selected = Target.instructionDescriptor(Instruction->opcode());
    if (Selected == nullptr)
      return std::unexpected(
          passFailure("instruction selector produced an unknown instruction descriptor"));
    if (IsGeneric) {
      auto FlagPropagation =
          Info->propagateSelectionFlags(*Instruction, InputFlags, InputMayRaiseFPException, Target);
      if (!FlagPropagation)
        return std::unexpected(FlagPropagation.error());
      auto Constraint = Info->constrainSelectedInstruction(*Instruction, Function, Target);
      if (!Constraint) {
        return std::unexpected(withErrorContext(
            Constraint.error(), "instruction selector could not constrain selected registers: "));
      }
    }
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation)
      return std::unexpected(withErrorContext(Validation.error(),
                                              "instruction selector produced invalid MachineIR: "));
    const OpcodeState After = stateOf(*Instruction);
    if (IsGeneric && After == Before)
      return std::unexpected(
          passFailure("instruction selector reported success without observable progress",
                      ErrorCode::InternalError));
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Instructions);
    if (Selected->Flags.has(InstructionFlag::Generic))
      Worklist.push_back(Instruction);
    auto Discovered = DiscoverNewInstructions();
    if (!Discovered)
      return std::unexpected(Discovered.error());
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
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->lastInstruction(); Instruction != nullptr;) {
      MachineInstr *Previous = Instruction->previous();
      if (Instruction->opcode() != opcode(GenericOpcode::COPY)) {
        Instruction = Previous;
        continue;
      }
      const MachineOperand *SourceOperand = Instruction->operandAt(1);
      const MachineOperand *DestinationOperand = Instruction->operandAt(0);
      const Register *Source =
          SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
      const Register *Destination =
          DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
      if (Source == nullptr || Destination == nullptr || !Source->isVirtual() ||
          !Destination->isVirtual() || SourceOperand->subReg().value() != 0) {
        Instruction = Previous;
        continue;
      }
      auto SourceInfo = Function.registerInfo().virtualRegisterInfo(*Source);
      if (!SourceInfo)
        return std::unexpected(SourceInfo.error());
      auto DestinationInfo = Function.registerInfo().virtualRegisterInfo(*Destination);
      if (!DestinationInfo)
        return std::unexpected(DestinationInfo.error());
      if (!SourceInfo->get().RegClass ||
          SourceInfo->get().RegClass != DestinationInfo->get().RegClass) {
        Instruction = Previous;
        continue;
      }
      auto Replaced = Function.registerInfo().replaceRegWith(*Destination, *Source);
      if (!Replaced)
        return std::unexpected(Replaced.error());
      auto Removed = Instruction->eraseFromParent();
      if (!Removed)
        return std::unexpected(Removed.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
      Instruction = Previous;
    }
  }
  auto RegisterValidation = Target.validateRegisterInfo(Function.registerInfo());
  if (!RegisterValidation)
    return std::unexpected(withErrorContext(RegisterValidation.error(),
                                            "instruction selection produced invalid registers: "));
  Function.registerInfo().clearAllVirtualRegisterTypes();
  Result.Changed = true;
  Result.Changes.add(MachinePassChange::Registers);
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}
} // namespace rcc
