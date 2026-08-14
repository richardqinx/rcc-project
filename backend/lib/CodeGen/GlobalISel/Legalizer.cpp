// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/Legalizer.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/CSEInfo.h"
#include "rcc/CodeGen/GlobalISel/CSEMIRBuilder.h"
#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/GlobalISel/GISelValueTracking.h"
#include "rcc/CodeGen/GlobalISel/GISelWorkList.h"
#include "rcc/CodeGen/GlobalISel/LegalizationArtifactCombiner.h"
#include "rcc/CodeGen/GlobalISel/LegalizerHelper.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
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

[[nodiscard]] std::vector<MachineInstr *> layoutInstructions(MachineFunction &Function) {
  std::vector<MachineInstr *> Result;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next())
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next())
      Result.push_back(Instruction);
  return Result;
}

[[nodiscard]] bool isArtifact(const MachineInstr &Instruction) noexcept {
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  default:
    return false;
  case GenericOpcode::G_TRUNC:
  case GenericOpcode::G_ZEXT:
  case GenericOpcode::G_ANYEXT:
  case GenericOpcode::G_SEXT:
  case GenericOpcode::G_MERGE_VALUES:
  case GenericOpcode::G_UNMERGE_VALUES:
  case GenericOpcode::G_CONCAT_VECTORS:
  case GenericOpcode::G_BUILD_VECTOR:
  case GenericOpcode::G_EXTRACT:
  case GenericOpcode::G_INSERT:
    return true;
  }
}

[[nodiscard]] bool isIntrinsicOpcode(Opcode InstructionOpcode) noexcept {
  switch (static_cast<GenericOpcode>(InstructionOpcode.value())) {
  default:
    return false;
  case GenericOpcode::G_INTRINSIC:
  case GenericOpcode::G_INTRINSIC_W_SIDE_EFFECTS:
  case GenericOpcode::G_INTRINSIC_CONVERGENT:
  case GenericOpcode::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS:
    return true;
  }
}

using InstList = GISelWorkList<256>;
using ArtifactList = GISelWorkList<128>;

class LegalizerWorkListManager final : public GISelChangeObserver {
public:
  LegalizerWorkListManager(InstList &Instructions, ArtifactList &Artifacts)
      : Instructions(Instructions), Artifacts(Artifacts) {}

  void erasingInstr(MachineInstr &Instruction) override {
    Instructions.remove(&Instruction);
    Artifacts.remove(&Instruction);
  }

  void createdInstr(MachineInstr &Instruction) override {
    createdOrChangedInstr(Instruction);
  }

  void changingInstr(MachineInstr &) override {}

  void changedInstr(MachineInstr &Instruction) override {
    createdOrChangedInstr(Instruction);
  }

private:
  void createdOrChangedInstr(MachineInstr &Instruction) {
    if (!isGenericOpcode(Instruction.opcode()))
      return;
    if (isArtifact(Instruction))
      Artifacts.insert(&Instruction);
    else
      Instructions.insert(&Instruction);
  }

  InstList &Instructions;
  ArtifactList &Artifacts;
};

[[nodiscard]] std::vector<MachineBasicBlock *> reversePostOrderBlocks(MachineFunction &Function) {
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

  std::vector<MachineBasicBlock *> PostOrder;
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
      PostOrder.push_back(Frame.Block);
      Stack.pop_back();
    }
  }
  std::ranges::reverse(PostOrder);
  return PostOrder;
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
                                           MachineFunctionAnalysisManager &Analyses) {
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
    return std::unexpected(withErrorContext(Trial.error(), "legalizer transaction setup failed: "));
  MachineActionReplay Actions(Analyses, Trial->InstructionStableIDs);
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, Actions);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  Actions.beginReplay();
  auto Result = runImplementation(Function, Target, Actions);
  if (!Result) {
    return std::unexpected(withErrorContext(
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
  InstList InstructionWorklist;
  ArtifactList ArtifactWorklist;
  std::vector<MachineInstr *> RetryList;

  for (MachineBasicBlock *Block : reversePostOrderBlocks(Function)) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (!isGenericOpcode(Instruction->opcode()))
        continue;
      if (isArtifact(*Instruction))
        ArtifactWorklist.deferredInsert(Instruction);
      else
        InstructionWorklist.deferredInsert(Instruction);
    }
  }
  ArtifactWorklist.finalize();
  InstructionWorklist.finalize();

  LegalizerWorkListManager WorkListObserver(InstructionWorklist, ArtifactWorklist);
  GISelCSEInfo CSEInfo;
  CSEInfo.setCSEConfig(getStandardCSEConfigForOpt(Optimization));
  auto CSEAnalysis = CSEInfo.analyze(Function);
  if (!CSEAnalysis)
    return std::unexpected(
        withErrorContext(CSEAnalysis.error(), "GlobalISel CSE analysis failed: "));
  std::array<GISelChangeObserver *, 2> AuxiliaryObservers{&WorkListObserver, &CSEInfo};
  GISelObserverWrapper Observer{std::span<GISelChangeObserver *const>(AuxiliaryObservers)};
  MachineFunctionDelegateInstaller ObserverInstaller(Function, Observer);
  CSEMIRBuilder Builder(Function, Target);
  Builder.setCSEInfo(&CSEInfo);
  const LibcallLoweringInfo *ActiveLibcalls =
      Libcalls.CallLowering == nullptr ? nullptr : &Libcalls;
  LegalizerHelper Helper(Function, *Info, Observer, Builder, ActiveLibcalls);
  GISelValueTracking ValueTracking(Function, Target);
  LegalizationArtifactCombiner ArtifactCombiner(Builder, Function.registerInfo(), *Info,
                                                &ValueTracking);

  do {
    while (!InstructionWorklist.empty()) {
      MachineInstr *Instruction = InstructionWorklist.popBack();
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

      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return std::unexpected(
            passFailure("legalizer encountered an unknown instruction descriptor"));
      if (!Descriptor->Flags.has(InstructionFlag::Generic))
        continue;

      auto Validation = Target.validateInstruction(*Instruction, Function.registerInfo());
      if (!Validation) {
        return std::unexpected(
            withErrorContext(Validation.error(), "legalizer input is invalid: "));
      }

      if (isIntrinsicOpcode(Instruction->opcode())) {
        auto Accepted = Actions.request("legalize.intrinsic", Function, Instruction->stableID());
        if (!Accepted)
          return std::unexpected(Accepted.error());
        if (!*Accepted)
          return std::unexpected(passFailure("required intrinsic legalization action was rejected",
                                             ErrorCode::VerificationError));
        auto IntrinsicResult = Info->legalizeIntrinsic(Helper, *Instruction);
        if (!IntrinsicResult)
          return std::unexpected(IntrinsicResult.error());
        if (!*IntrinsicResult) {
          const std::string_view Name = Descriptor->Name;
          return std::unexpected(passFailure("target legalizer rejected intrinsic instruction '" +
                                                 std::string(Name) + "'",
                                             ErrorCode::Unsupported));
        }
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Instructions);
        continue;
      }

      auto Decision = Info->getDecision(*Instruction, Function, Target);
      if (!Decision)
        return std::unexpected(Decision.error());
      if (Decision->Action == LegalizationAction::Legal)
        continue;
      if (Decision->Action == LegalizationAction::Unsupported) {
        if (isArtifact(*Instruction)) {
          RetryList.push_back(Instruction);
          continue;
        }
        return std::unexpected(
            passFailure("target legalizer rejected generic instruction '" + Descriptor->Name + "'",
                        ErrorCode::Unsupported));
      }

      auto LoweringValidation = Info->validateLowering(*Instruction, Function, Target, *Decision);
      if (!LoweringValidation) {
        return std::unexpected(withErrorContext(LoweringValidation.error(),
                                                "legalizer lowering precondition failed: "));
      }
      std::string Rule = "legalize.";
      Rule.append(legalizationActionName(Decision->Action));
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
      Expected<void> Lowered = Helper.legalize(*Instruction, *Decision);
      if (!Lowered) {
        if (Lowered.error().code() == ErrorCode::Unsupported && Instruction->parent() != nullptr &&
            isArtifact(*Instruction)) {
          RetryList.push_back(Instruction);
          continue;
        }
        return std::unexpected(Lowered.error());
      }
      Result.Changed = true;
      Result.Changes.add(MachinePassChange::Instructions);
    }

    if (!RetryList.empty()) {
      if (!ArtifactWorklist.empty()) {
        for (MachineInstr *Instruction : RetryList)
          ArtifactWorklist.insert(Instruction);
        RetryList.clear();
      } else {
        MachineInstr *Instruction = RetryList.front();
        const InstructionDescriptor *Descriptor =
            Instruction == nullptr ? nullptr : Target.instructionDescriptor(Instruction->opcode());
        return std::unexpected(passFailure(
            "target legalizer rejected generic artifact '" +
                std::string(Descriptor == nullptr ? "<unknown>" : Descriptor->Name) + "'",
            ErrorCode::Unsupported));
      }
    }

    while (!ArtifactWorklist.empty()) {
      MachineInstr *Instruction = ArtifactWorklist.popBack();
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
      std::vector<MachineInstr *> DeadInstructions;
      auto Combined =
          ArtifactCombiner.tryCombineInstruction(*Instruction, DeadInstructions, Observer);
      if (!Combined)
        return std::unexpected(Combined.error());
      if (*Combined) {
        auto Removed = eraseInstrs(DeadInstructions, Function.registerInfo(), Target);
        if (!Removed)
          return std::unexpected(Removed.error());
        Result.Changed = true;
        Result.Changes.add(MachinePassChange::Instructions);
        continue;
      }
      InstructionWorklist.insert(Instruction);
    }
  } while (!InstructionWorklist.empty());

  auto CSEValidation = CSEInfo.verify();
  if (!CSEValidation) {
    return std::unexpected(
        withErrorContext(CSEValidation.error(), "GlobalISel CSE verification failed: "));
  }

  for (MachineInstr *Instruction : layoutInstructions(Function)) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return std::unexpected(passFailure("legalizer produced an unknown instruction descriptor"));
    if (!Descriptor->Flags.has(InstructionFlag::Generic))
      continue;
    auto Decision = Info->getDecision(*Instruction, Function, Target);
    if (!Decision)
      return std::unexpected(Decision.error());
    if (Decision->Action != LegalizationAction::Legal &&
        Decision->Action != LegalizationAction::Custom)
      return std::unexpected(passFailure(
          "legalizer reached a fixed point with an illegal generic instruction '" +
              Descriptor->Name + "'",
          Decision->Action == LegalizationAction::Unsupported ? ErrorCode::Unsupported
                                                              : ErrorCode::InternalError));
    auto FinalValidation = Target.validateInstruction(*Instruction, Function.registerInfo());
    if (!FinalValidation)
      return std::unexpected(
          withErrorContext(FinalValidation.error(), "legalizer produced invalid MachineIR: "));
  }
  auto Liveness = clearTrackedLivenessFlags(Function, Result);
  if (!Liveness)
    return std::unexpected(
        withErrorContext(Liveness.error(), "legalizer liveness-flag invalidation failed: "));
  Result.Preserved = Result.Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  return Result;
}

} // namespace rcc
