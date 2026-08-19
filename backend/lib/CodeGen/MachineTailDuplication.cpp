// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineTailDuplication.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &tailDuplicationContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-tail-duplication",
        .Requires = {},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::FrameFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::CFG, MachinePassChange::BlockLayout};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

[[nodiscard]] bool isJumpTableTarget(const MachineBasicBlock &Block,
                                     const MachineFunction &Function) noexcept {
  for (const MachineJumpTableEntry &Entry : Function.jumpTableInfo().tables()) {
    const MachineJumpTableDesc *Description = Entry.description();
    if (Description != nullptr &&
        std::ranges::find(Description->Targets, &Block) != Description->Targets.end()) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool hasProtectedIdentity(const MachineBasicBlock &Block,
                                        const MachineFunction &Function) noexcept {
  const MachineBasicBlockAttributes &Attributes = Block.attributes();
  return Attributes.rawFlags() != 0 || !Attributes.sectionID().isDefault() ||
         Attributes.profileID().has_value() || isJumpTableTarget(Block, Function);
}

[[nodiscard]] Expected<bool> isDuplicableInstruction(const MachineInstr &Instruction,
                                                     const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tail duplication encountered an unknown instruction");
  }
  const InstructionFlags Flags = Descriptor->Flags;
  if (Flags.has(InstructionFlag::NotDuplicable) || Flags.has(InstructionFlag::Call) ||
      Flags.has(InstructionFlag::Return) || Flags.has(InstructionFlag::Barrier) ||
      Flags.has(InstructionFlag::Meta) || Flags.has(InstructionFlag::Phi) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) || Flags.has(InstructionFlag::Convergent) ||
      Instruction.flags().has(MachineInstrFlag::NoMerge) ||
      Instruction.flags().has(MachineInstrFlag::BundledWithPred) ||
      Instruction.flags().has(MachineInstrFlag::BundledWithSucc) ||
      hasProtectedAttachment(Instruction)) {
    return false;
  }
  return std::ranges::none_of(Instruction.operands(), [](const MachineOperand &Operand) {
    return Operand.kind() == MachineOperand::Kind::BasicBlock ||
           Operand.kind() == MachineOperand::Kind::DebugInstrRef;
  });
}

[[nodiscard]] Expected<MachineInstr *>
cloneInstructionBefore(MachineInstr &Source, MachineBasicBlock &Destination, MachineInstr *Before) {
  MachineFunction &Function = Destination.function();
  auto Created = Function.createInstruction(Source.opcode());
  if (!Created)
    return std::unexpected(Created.error());
  MachineInstr *Clone = *Created;
  Clone->reserveOperands(Source.operandCount());
  for (const MachineOperand &Operand : Source.operands()) {
    auto Added = Clone->addOperand(Operand.cloneDetached());
    if (!Added)
      return std::unexpected(Added.error());
  }
  for (std::size_t Index = 0; Index < Source.operandCount(); ++Index) {
    const MachineOperand *Operand = Source.operandAt(Index);
    const std::optional<std::size_t> Tied =
        Operand == nullptr ? std::nullopt : Operand->tiedOperandIndex();
    if (!Tied || Index > *Tied)
      continue;
    const std::size_t DefIndex = Operand->isDef() ? Index : *Tied;
    const std::size_t UseIndex = Operand->isDef() ? *Tied : Index;
    auto TiedResult = Clone->tieOperands(DefIndex, UseIndex);
    if (!TiedResult)
      return std::unexpected(TiedResult.error());
  }
  for (const MachineMemOperand *Memory : Source.memOperands()) {
    if (Memory == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "tail duplication encountered a null memory operand");
    auto Added = Clone->addMemOperand(*Memory);
    if (!Added)
      return std::unexpected(Added.error());
  }
  Clone->setFlags(Source.flags());
  if (Source.sourceLocation())
    Clone->setSourceLocation(*Source.sourceLocation());
  if (Source.debugLocation()) {
    auto Debug = Clone->setDebugLocation(*Source.debugLocation());
    if (!Debug)
      return std::unexpected(Debug.error());
  }
  if (Source.origin())
    Clone->setOrigin(*Source.origin());
  Expected<void> Inserted = Before == nullptr
                                ? Destination.appendInstruction(*Clone)
                                : Destination.insertInstructionBefore(*Before, *Clone);
  if (!Inserted)
    return std::unexpected(Inserted.error());
  return Clone;
}

struct TailCandidate final {
  MachineBasicBlock *Successor = nullptr;
  MachineInstr *Branch = nullptr;
  std::vector<MachineInstr *> Body;
};

[[nodiscard]] Expected<std::optional<TailCandidate>>
analyzeTail(MachineBasicBlock &Tail, MachineFunction &Function, const TargetDescription &Target,
            const TargetInstrInfo &Info, std::size_t MaximumInstructions) {
  if (&Tail == Function.firstBlock() || Tail.predecessors().size() < 2 ||
      Tail.successors().size() != 1 || Tail.successors().front().Block == nullptr ||
      hasProtectedIdentity(Tail, Function)) {
    return std::nullopt;
  }
  auto Branch = Info.analyzeBranch(Tail, Function, Target);
  if (!Branch)
    return std::unexpected(Branch.error());
  MachineBasicBlock *Successor = Tail.successors().front().Block;
  if (Branch->HasUnanalyzableTerminator || Branch->Conditional != nullptr ||
      Branch->Unconditional == nullptr || Branch->TrueTarget != Successor ||
      Branch->Unconditional != Tail.lastInstruction() ||
      hasProtectedAttachment(*Branch->Unconditional)) {
    return std::nullopt;
  }

  TailCandidate Candidate{.Successor = Successor, .Branch = Branch->Unconditional, .Body = {}};
  for (MachineInstr *Instruction = Tail.firstInstruction(); Instruction != Candidate.Branch;
       Instruction = Instruction->next()) {
    if (Instruction == nullptr)
      return makeUnexpected(ErrorCode::CFGOptimizationFailed,
                            "tail branch is not reachable from its block body");
    auto Duplicable = isDuplicableInstruction(*Instruction, Target);
    if (!Duplicable)
      return std::unexpected(Duplicable.error());
    if (!*Duplicable)
      return std::nullopt;
    Candidate.Body.push_back(Instruction);
    if (Candidate.Body.size() > MaximumInstructions)
      return std::nullopt;
  }
  return Candidate;
}

[[nodiscard]] Expected<bool>
duplicateTailIntoPredecessor(const TailCandidate &Candidate, MachineBasicBlock &Tail,
                             MachineBasicBlock &Predecessor, MachineFunction &Function,
                             const TargetDescription &Target, const TargetInstrInfo &Info) {
  if (Predecessor.successors().size() != 1 || Predecessor.successors().front().Block != &Tail) {
    return false;
  }
  auto Branch = Info.analyzeBranch(Predecessor, Function, Target);
  if (!Branch)
    return std::unexpected(Branch.error());
  if (Branch->HasUnanalyzableTerminator || Branch->Conditional != nullptr)
    return false;
  MachineInstr *OldBranch = Branch->Unconditional;
  if (OldBranch != nullptr) {
    if (Branch->TrueTarget != &Tail || OldBranch != Predecessor.lastInstruction() ||
        hasProtectedAttachment(*OldBranch)) {
      return false;
    }
  } else if (Predecessor.next() != &Tail) {
    return false;
  }

  for (MachineInstr *Instruction : Candidate.Body) {
    auto Cloned = cloneInstructionBefore(*Instruction, Predecessor, OldBranch);
    if (!Cloned)
      return std::unexpected(Cloned.error());
  }
  if (OldBranch != nullptr) {
    auto Removed = Info.removeBranch(*OldBranch, Function, Target);
    if (!Removed)
      return std::unexpected(Removed.error());
  }
  auto NewBranch = Info.insertBranch(Predecessor, *Candidate.Successor, Function, Target);
  if (!NewBranch)
    return std::unexpected(NewBranch.error());
  MachineCFGUpdater CFG = Function.cfgUpdater();
  auto Redirected = CFG.replaceSuccessor(Predecessor, Tail, *Candidate.Successor);
  if (!Redirected)
    return std::unexpected(Redirected.error());
  return true;
}

} // namespace

const MachinePassContract &MachineTailDuplicationPass::contract() const noexcept {
  return tailDuplicationContract();
}

Expected<MachinePassResult> MachineTailDuplicationPass::run(MachineFunction &Function,
                                                            const TargetDescription &Target,
                                                            MachineFunctionAnalysisManager &) {
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "tail duplication transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "tail duplication diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachineTailDuplicationPass::runImplementation(MachineFunction &Function,
                                              const TargetDescription &Target) const {
  MachinePassResult Result;
  std::size_t DuplicatedInstructions = 0;
  for (MachineBasicBlock *Tail = Function.firstBlock(); Tail != nullptr;) {
    MachineBasicBlock *Next = Tail->next();
    auto Candidate = analyzeTail(*Tail, Function, Target, *Info, Options.MaximumTailInstructions);
    if (!Candidate)
      return std::unexpected(Candidate.error());
    if (!Candidate->has_value()) {
      Tail = Next;
      continue;
    }
    const auto &TailCandidate = Candidate->value();
    const std::size_t Cost = TailCandidate.Body.size() + 1U;
    const std::vector<MachineBasicBlock *> Predecessors(Tail->predecessors().begin(),
                                                        Tail->predecessors().end());
    for (MachineBasicBlock *Predecessor : Predecessors) {
      if (Predecessor == nullptr ||
          DuplicatedInstructions > Options.MaximumDuplicatedInstructions ||
          Cost > Options.MaximumDuplicatedInstructions - DuplicatedInstructions) {
        break;
      }
      auto Duplicated =
          duplicateTailIntoPredecessor(TailCandidate, *Tail, *Predecessor, Function, Target, *Info);
      if (!Duplicated)
        return std::unexpected(Duplicated.error());
      if (!*Duplicated)
        continue;
      DuplicatedInstructions += Cost;
      Result.Changed = true;
    }
    if (Tail->predecessors().empty()) {
      MachineCFGUpdater CFG = Function.cfgUpdater();
      auto Deleted = CFG.deleteBlock(*Tail);
      if (!Deleted)
        return std::unexpected(Deleted.error());
      Result.Changed = true;
    }
    Tail = Next;
  }

  if (!Result.Changed) {
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  auto Rebuilt = rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                    MachinePassChange::CFG, MachinePassChange::BlockLayout};
  Result.Preserved = PreservedAnalyses::none();
  return Result;
}

} // namespace rcc
