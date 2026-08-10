// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePipelinePasses.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
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

[[nodiscard]] const MachinePassContract &legalizerContract() {
  static const MachinePassContract Contract{
      .Name = "Legalizer",
      .Requires = {MachineFunctionProperty::IsSSA, MachineFunctionProperty::TracksLiveness},
      .Sets = {MachineFunctionProperty::Legalized},
      .Clears = {},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::Legalized,
  };
  return Contract;
}

[[nodiscard]] const MachinePassContract &bankSelectContract() {
  static const MachinePassContract Contract{
      .Name = "RegisterBankSelect",
      .Requires = {MachineFunctionProperty::Legalized},
      .Sets = {MachineFunctionProperty::RegBankSelected},
      .Clears = {},
      .RequiredAnalyses = {},
      .VerificationStage = PipelineStage::RegBankSelected,
  };
  return Contract;
}

[[nodiscard]] const MachinePassContract &instructionSelectContract() {
  static const MachinePassContract Contract{
      .Name = "InstructionSelect",
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

[[nodiscard]] std::uint64_t
registerConfigurationFingerprint(const MachineRegisterInfo &RegInfo) noexcept {
  std::uint64_t Result = 0xcbf29ce484222325ULL;
  for (const VirtualRegisterInfo &Info : RegInfo.virtualRegisters()) {
    combineFingerprint(Result, Info.IsTombstone ? 1U : 0U);
    combineFingerprint(Result, Info.Bank ? Info.Bank->value() : 0U);
    combineFingerprint(Result, Info.RegClass ? Info.RegClass->value() : 0U);
  }
  return Result;
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

[[nodiscard]] Expected<void> restoreRegisterConfiguration(
    MachineRegisterInfo &RegInfo,
    const std::vector<std::pair<Register, std::pair<std::optional<RegisterBankID>,
                                                    std::optional<RegisterClassID>>>> &Snapshot) {
  for (const auto &[Reg, Configuration] : Snapshot) {
    auto ClearBank = RegInfo.clearRegisterBank(Reg);
    if (!ClearBank)
      return std::unexpected(ClearBank.error());
    if (Configuration.first) {
      auto SetBank = RegInfo.setRegisterBank(Reg, *Configuration.first);
      if (!SetBank)
        return std::unexpected(SetBank.error());
    }
    auto ClearClass = RegInfo.clearRegisterClass(Reg);
    if (!ClearClass)
      return std::unexpected(ClearClass.error());
    if (Configuration.second) {
      auto SetClass = RegInfo.setRegisterClass(Reg, *Configuration.second);
      if (!SetClass)
        return std::unexpected(SetClass.error());
    }
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

const MachinePassContract &LegalizerPass::contract() const noexcept {
  return legalizerContract();
}

Expected<MachinePassResult> LegalizerPass::run(MachineFunction &Function,
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

Expected<MachinePassResult> LegalizerPass::runImplementation(MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             MachineActionReplay &Actions) {

  MachinePassResult Result;
  std::unordered_set<OpcodeState, OpcodeStateHash> SeenStates;

  std::vector<MachineInstr *> Worklist = layoutInstructions(Function);
  std::unordered_set<MachineInstrStableID> DiscoveredInstructions;
  for (const MachineInstr *Instruction : Worklist)
    DiscoveredInstructions.insert(Instruction->stableID());
  for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
    MachineInstr *Instruction = Worklist[Index];
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
    if (Instruction->parent() == nullptr)
      return std::unexpected(passFailure("legalizer detached the instruction it was lowering"));
    const OpcodeState After = stateOf(*Instruction);
    if (After == Before)
      return std::unexpected(
          passFailure("target legalizer made no observable progress for '" + Descriptor->Name + "'",
                      ErrorCode::InternalError));
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Instructions);

    const InstructionDescriptor *LoweredDescriptor =
        Target.instructionDescriptor(Instruction->opcode());
    if (LoweredDescriptor == nullptr)
      return std::unexpected(passFailure("legalizer produced an unknown instruction descriptor"));
    if (LoweredDescriptor->Flags.has(InstructionFlag::Generic))
      Worklist.push_back(Instruction);
    const std::vector<MachineInstr *> Current = layoutInstructions(Function);
    for (MachineInstr *Candidate : Current) {
      if (DiscoveredInstructions.insert(Candidate->stableID()).second)
        Worklist.push_back(Candidate);
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
  auto Liveness = repairTrackedLiveness(Function, Target, Result);
  if (!Liveness)
    return std::unexpected(prefixedError(Liveness.error(), "legalizer liveness repair failed: "));
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

const MachinePassContract &RegisterBankSelectPass::contract() const noexcept {
  return bankSelectContract();
}

Expected<MachinePassResult> RegisterBankSelectPass::run(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineAnalysisManager &) {
  if (Info == nullptr)
    return std::unexpected(passFailure("register-bank select has no target behavior interface",
                                       ErrorCode::InvalidArgument));
  auto Required = requireProperties(Function, contract());
  if (!Required)
    return std::unexpected(Required.error());

  MachinePassResult Result;
  MachineRegisterInfo &RegInfo = Function.registerInfo();
  const std::uint64_t RegisterConfigurationBefore = registerConfigurationFingerprint(RegInfo);
  using RegisterConfiguration =
      std::pair<std::optional<RegisterBankID>, std::optional<RegisterClassID>>;
  std::vector<std::pair<Register, RegisterConfiguration>> Snapshot;
  std::vector<std::pair<Register, RegisterBankID>> PlannedBanks;
  for (std::size_t Index = 0; Index < RegInfo.virtualRegisters().size(); ++Index) {
    auto Reg = Register::makeVirtual(Index);
    if (!Reg)
      return std::unexpected(Reg.error());
    const VirtualRegisterInfo &StoredInfo = RegInfo.virtualRegisters()[Index];
    if (StoredInfo.IsTombstone)
      continue;
    Snapshot.emplace_back(*Reg, RegisterConfiguration{StoredInfo.Bank, StoredInfo.RegClass});
    auto InfoResult = RegInfo.virtualRegisterInfo(*Reg);
    if (!InfoResult)
      return std::unexpected(InfoResult.error());
    const VirtualRegisterInfo &InfoValue = InfoResult->get();
    const MachineInstr *Context = nullptr;
    for (const MachineOperand &Operand : RegInfo.regOperands(*Reg)) {
      Context = Operand.parent();
      if (Context != nullptr)
        break;
    }
    auto Bank = Info->selectBank(Function, *Reg, InfoValue, Context, Target);
    if (!Bank)
      return std::unexpected(Bank.error());
    if (Target.registers().registerBank(*Bank) == nullptr)
      return std::unexpected(passFailure("target register-bank selector returned an unknown bank"));
    if (InfoValue.Bank && *InfoValue.Bank != *Bank)
      return std::unexpected(
          passFailure("target register-bank selector conflicts with an existing bank"));
    if (InfoValue.RegClass && !Target.registers().bankCoversClass(*Bank, *InfoValue.RegClass))
      return std::unexpected(
          passFailure("existing register class is not covered by selected bank"));
    PlannedBanks.emplace_back(*Reg, *Bank);
  }

  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    auto Validation = Info->validateInstruction(*Instruction, Function, Target);
    if (!Validation)
      return std::unexpected(
          prefixedError(Validation.error(), "register-bank mapping precondition failed: "));
  }

  for (const auto &[Reg, Bank] : PlannedBanks) {
    const auto InfoResult = RegInfo.virtualRegisterInfo(Reg);
    if (!InfoResult)
      return std::unexpected(InfoResult.error());
    if (!InfoResult->get().Bank) {
      auto SetBank = RegInfo.setRegisterBank(Reg, Bank);
      if (!SetBank)
        return std::unexpected(SetBank.error());
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Registers);
    }
  }

  const auto Restore = [&]() -> Expected<void> {
    return restoreRegisterConfiguration(RegInfo, Snapshot);
  };
  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    auto Constraint = Info->constrainInstruction(*Instruction, Function, Target);
    if (!Constraint) {
      auto Restored = Restore();
      if (!Restored)
        return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
      return std::unexpected(Constraint.error());
    }
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation) {
      auto Restored = Restore();
      if (!Restored)
        return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
      return std::unexpected(prefixedError(Validation.error(),
                                           "register-bank selection produced invalid MachineIR: "));
    }
  }
  auto RegisterValidation = Target.validateRegisterInfo(RegInfo);
  if (!RegisterValidation) {
    auto Restored = Restore();
    if (!Restored)
      return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
    return std::unexpected(prefixedError(RegisterValidation.error(),
                                         "register-bank selection produced invalid registers: "));
  }

  if (registerConfigurationFingerprint(RegInfo) != RegisterConfigurationBefore) {
    Result.Changed = true;
    Result.Changes.add(MachinePassChange::Registers);
  }

  auto Liveness = repairTrackedLiveness(Function, Target, Result);
  if (!Liveness) {
    auto Restored = Restore();
    if (!Restored)
      return std::unexpected(prefixedError(Restored.error(), "register-bank rollback failed: "));
    return std::unexpected(
        prefixedError(Liveness.error(), "register-bank liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

const MachinePassContract &InstructionSelectPass::contract() const noexcept {
  return instructionSelectContract();
}

Expected<MachinePassResult> InstructionSelectPass::run(MachineFunction &Function,
                                                       const TargetDescription &Target,
                                                       MachineAnalysisManager &Analyses) {
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
        prefixedError(Trial.error(), "instruction-selection transaction setup failed: "));
  }
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(
        prefixedError(Result.error(),
                      "instruction selection diverged after a successful transaction preflight: "));
  }
  auto Replayed = Actions.finishReplay();
  if (!Replayed)
    return std::unexpected(Replayed.error());
  return Result;
}

Expected<MachinePassResult>
InstructionSelectPass::runImplementation(MachineFunction &Function, const TargetDescription &Target,
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
          prefixedError(Validation.error(), "instruction selector input is invalid: "));
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
    auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!Validation)
      return std::unexpected(
          prefixedError(Validation.error(), "instruction selector produced invalid MachineIR: "));
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
      return std::unexpected(
          prefixedError(Validation.error(), "instruction selector produced invalid MachineIR: "));
    if (Descriptor->Flags.has(InstructionFlag::Generic))
      return std::unexpected(
          passFailure("instruction selector left a generic opcode in the function"));
  }
  auto RegisterValidation = Target.validateRegisterInfo(Function.registerInfo());
  if (!RegisterValidation)
    return std::unexpected(prefixedError(RegisterValidation.error(),
                                         "instruction selection produced invalid registers: "));
  auto Liveness = repairTrackedLiveness(Function, Target, Result);
  if (!Liveness) {
    return std::unexpected(
        prefixedError(Liveness.error(), "instruction-selection liveness repair failed: "));
  }
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc
