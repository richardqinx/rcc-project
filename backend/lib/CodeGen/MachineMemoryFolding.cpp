// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineMemoryFolding.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMemoryFolding.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &memoryFoldingContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-memory-folding",
        .Requires = {MachineFunctionProperty::IsSSA, MachineFunctionProperty::Selected},
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

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value() ||
         Instruction.flags().has(MachineInstrFlag::BundledWithPred) ||
         Instruction.flags().has(MachineInstrFlag::BundledWithSucc);
}

struct FoldCandidate final {
  MachineInstr *MemoryInstruction = nullptr;
  MachineInstr *Consumer = nullptr;
  Register DefinedRegister;
  std::size_t ConsumerOperand = 0;
};

[[nodiscard]] std::optional<FoldCandidate> findCandidate(MachineInstr &Instruction,
                                                         MachineFunction &Function,
                                                         const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr || !Descriptor->Flags.has(InstructionFlag::MayLoad) ||
      Descriptor->Flags.has(InstructionFlag::MayStore) ||
      Descriptor->Flags.has(InstructionFlag::Call) ||
      Descriptor->Flags.has(InstructionFlag::Barrier) ||
      Descriptor->Flags.has(InstructionFlag::UnmodeledSideEffects) ||
      hasProtectedAttachment(Instruction) || Instruction.memOperands().empty()) {
    return std::nullopt;
  }
  for (const MachineMemOperand *Memory : Instruction.memOperands()) {
    if (Memory == nullptr || Memory->isStore() || Memory->isAtomic() || Memory->isVolatile()) {
      return std::nullopt;
    }
  }

  MachineOperand *Definition = nullptr;
  Register DefinedRegister;
  for (std::size_t Index = 0; Index < Instruction.operandCount(); ++Index) {
    MachineOperand *Operand = Instruction.operandAt(Index);
    const Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Reg == nullptr || !Operand->isDef())
      continue;
    if (Definition != nullptr || !Reg->isVirtual())
      return std::nullopt;
    Definition = Operand;
    DefinedRegister = *Reg;
  }
  if (Definition == nullptr || !Function.registerInfo().hasOneUse(DefinedRegister, true)) {
    return std::nullopt;
  }
  MachineOperand *Use = nullptr;
  for (MachineOperand &Candidate : Function.registerInfo().useOperands(DefinedRegister, false)) {
    Use = &Candidate;
    break;
  }
  MachineInstr *Consumer = Use == nullptr ? nullptr : Use->parent();
  if (Consumer == nullptr || Consumer->parent() != Instruction.parent() ||
      Instruction.next() != Consumer || hasProtectedAttachment(*Consumer) || Use->isImplicit() ||
      Use->tiedOperandIndex()) {
    return std::nullopt;
  }
  for (std::size_t Index = 0; Index < Consumer->operandCount(); ++Index) {
    if (Consumer->operandAt(Index) == Use)
      return FoldCandidate{&Instruction, Consumer, DefinedRegister, Index};
  }
  return std::nullopt;
}

[[nodiscard]] bool containsMemoryDescription(const MachineInstr &Instruction,
                                             const MachineMemOperandDesc &Description) noexcept {
  return std::ranges::any_of(Instruction.memOperands(),
                             [&Description](const MachineMemOperand *Memory) {
                               return Memory != nullptr && Memory->description() == Description;
                             });
}

[[nodiscard]] Expected<bool> applyFold(FoldCandidate Candidate, MachineFunction &Function,
                                       const TargetDescription &Target,
                                       const TargetMemoryFoldingInfo &Info) {
  auto CanFold = Info.canFoldMemoryOperand(*Candidate.Consumer, Candidate.ConsumerOperand,
                                           *Candidate.MemoryInstruction, Function, Target);
  if (!CanFold)
    return std::unexpected(CanFold.error());
  if (!*CanFold)
    return false;
  std::vector<MachineMemOperandDesc> MemoryDescriptions;
  for (const MachineMemOperand *Memory : Candidate.MemoryInstruction->memOperands()) {
    if (Memory == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "memory folding encountered a null memory operand");
    }
    MemoryDescriptions.push_back(Memory->description());
  }
  auto Folded = Info.foldMemoryOperand(*Candidate.Consumer, Candidate.ConsumerOperand,
                                       *Candidate.MemoryInstruction, Function, Target);
  if (!Folded)
    return std::unexpected(Folded.error());
  if (*Folded == nullptr || (*Folded)->parent() == nullptr ||
      Candidate.MemoryInstruction->parent() != nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target memory fold did not replace the consumer and erase the producer");
  }
  for (const MachineMemOperandDesc &Description : MemoryDescriptions) {
    if (!containsMemoryDescription(**Folded, Description)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "target memory fold did not migrate every memory operand");
    }
  }
  auto FoldedSize = Info.getFoldedMemorySize(**Folded, Function, Target);
  if (!FoldedSize)
    return std::unexpected(FoldedSize.error());
  if (!FoldedSize->has_value() || !FoldedSize->value().isKnown()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target folded instruction has no known folded memory size");
  }
  if (MemoryDescriptions.size() == 1 && MemoryDescriptions.front().Size.isKnown() &&
      FoldedSize->value() != MemoryDescriptions.front().Size) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target folded memory size disagrees with the migrated MMO");
  }
  if (!Function.registerInfo().useEmpty(Candidate.DefinedRegister, true) ||
      Function.registerInfo().getUniqueVRegDef(Candidate.DefinedRegister) != nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target memory fold retained the load result register");
  }
  auto Erased = Function.registerInfo().eraseVirtualRegister(Candidate.DefinedRegister);
  if (!Erased)
    return std::unexpected(Erased.error());
  return true;
}

} // namespace

const MachinePassContract &MachineMemoryFoldingPass::contract() const noexcept {
  return memoryFoldingContract();
}

Expected<MachinePassResult> MachineMemoryFoldingPass::run(MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          MachineFunctionAnalysisManager &) {
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "memory folding transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "memory folding diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachineMemoryFoldingPass::runImplementation(MachineFunction &Function,
                                            const TargetDescription &Target) const {
  MachinePassResult Result;
  std::size_t FoldCount = 0;
  bool LocalChange = true;
  while (LocalChange && FoldCount < Options.MaximumFolds) {
    LocalChange = false;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr && !LocalChange;
         Block = Block->next()) {
      for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
           Instruction = Instruction->next()) {
        std::optional<FoldCandidate> Candidate = findCandidate(*Instruction, Function, Target);
        if (!Candidate)
          continue;
        auto Folded = applyFold(*Candidate, Function, Target, *Info);
        if (!Folded)
          return std::unexpected(Folded.error());
        if (!*Folded)
          continue;
        ++FoldCount;
        Result.Changed = true;
        LocalChange = true;
        break;
      }
    }
  }
  if (Result.Changed) {
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                      MachinePassChange::Registers};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
