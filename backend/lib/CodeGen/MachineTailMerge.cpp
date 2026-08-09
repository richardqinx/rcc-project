// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineTailMerge.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineBasicBlockAttributes.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetBranchInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &tailMergeContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-tail-merge",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::TracksLiveness,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
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

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
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

template <typename ValueT>
[[nodiscard]] bool payloadEquals(const MachineOperand &Left, const MachineOperand &Right) noexcept {
  const ValueT *LeftValue = Left.getIf<ValueT>();
  const ValueT *RightValue = Right.getIf<ValueT>();
  return LeftValue != nullptr && RightValue != nullptr && *LeftValue == *RightValue;
}

template <typename MaskOperandT>
[[nodiscard]] bool maskEquals(const MachineOperand &Left, const MachineOperand &Right) noexcept {
  const MaskOperandT *LeftValue = Left.getIf<MaskOperandT>();
  const MaskOperandT *RightValue = Right.getIf<MaskOperandT>();
  if (LeftValue == nullptr || RightValue == nullptr)
    return false;
  if (!LeftValue->Mask || !RightValue->Mask)
    return LeftValue->Mask == RightValue->Mask;
  return *LeftValue->Mask == *RightValue->Mask;
}

[[nodiscard]] bool operandEquals(const MachineOperand &Left, const MachineOperand &Right) noexcept {
  if (Left.kind() != Right.kind() || Left.targetFlags() != Right.targetFlags() ||
      Left.tiedOperandIndex() != Right.tiedOperandIndex()) {
    return false;
  }
  if (Left.kind() == MachineOperand::Kind::Register) {
    const Register *LeftReg = Left.getIf<Register>();
    const Register *RightReg = Right.getIf<Register>();
    if (LeftReg == nullptr || RightReg == nullptr || *LeftReg != *RightReg ||
        Left.subReg() != Right.subReg()) {
      return false;
    }
    RegisterOperandFlags LeftFlags = Left.registerFlags();
    RegisterOperandFlags RightFlags = Right.registerFlags();
    for (const RegisterOperandFlag Flag :
         {RegisterOperandFlag::Kill, RegisterOperandFlag::Dead, RegisterOperandFlag::Renamable}) {
      LeftFlags.clear(Flag);
      RightFlags.clear(Flag);
    }
    return LeftFlags == RightFlags;
  }
  switch (Left.kind()) {
  case MachineOperand::Kind::Register:
    return false;
  case MachineOperand::Kind::Immediate:
    return payloadEquals<std::int64_t>(Left, Right);
  case MachineOperand::Kind::IntegerImmediate:
    return payloadEquals<IntegerImmediateOperand>(Left, Right);
  case MachineOperand::Kind::FloatingImmediate:
    return payloadEquals<FloatingImmediateOperand>(Left, Right);
  case MachineOperand::Kind::BasicBlock:
    return payloadEquals<BasicBlockOperand>(Left, Right);
  case MachineOperand::Kind::FrameIndex:
    return payloadEquals<FrameIndexOperand>(Left, Right);
  case MachineOperand::Kind::ConstantPoolIndex:
    return payloadEquals<ConstantPoolOperand>(Left, Right);
  case MachineOperand::Kind::JumpTableIndex:
    return payloadEquals<JumpTableOperand>(Left, Right);
  case MachineOperand::Kind::GlobalSymbol:
    return payloadEquals<GlobalSymbolOperand>(Left, Right);
  case MachineOperand::Kind::ExternalSymbol:
    return payloadEquals<ExternalSymbolOperand>(Left, Right);
  case MachineOperand::Kind::BlockAddress:
    return payloadEquals<BlockAddressOperand>(Left, Right);
  case MachineOperand::Kind::RegisterMask:
    return maskEquals<RegisterMaskOperand>(Left, Right);
  case MachineOperand::Kind::LiveOutRegisterMask:
    return maskEquals<LiveOutRegisterMaskOperand>(Left, Right);
  case MachineOperand::Kind::Metadata:
    return payloadEquals<MetadataOperand>(Left, Right);
  case MachineOperand::Kind::TargetIndex:
    return payloadEquals<TargetIndexOperand>(Left, Right);
  case MachineOperand::Kind::MCSymbol:
    return payloadEquals<MCSymbolOperand>(Left, Right);
  case MachineOperand::Kind::CFIIndex:
    return payloadEquals<CFIIndexOperand>(Left, Right);
  case MachineOperand::Kind::Intrinsic:
    return payloadEquals<IntrinsicOperand>(Left, Right);
  case MachineOperand::Kind::Predicate:
    return payloadEquals<PredicateOperand>(Left, Right);
  case MachineOperand::Kind::ShuffleMask:
    return payloadEquals<ShuffleMaskOperand>(Left, Right);
  case MachineOperand::Kind::DebugInstrRef:
    return payloadEquals<DebugInstrRefOperand>(Left, Right);
  case MachineOperand::Kind::LaneMask:
    return payloadEquals<LaneMaskOperand>(Left, Right);
  case MachineOperand::Kind::Count:
    return false;
  }
  return false;
}

[[nodiscard]] Expected<bool> instructionEquals(const MachineInstr &Left, const MachineInstr &Right,
                                               const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Left.opcode());
  if (Descriptor == nullptr || Target.instructionDescriptor(Right.opcode()) == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tail merge encountered an unknown instruction");
  }
  if (Left.opcode() != Right.opcode() || Left.flags() != Right.flags() ||
      Left.operandCount() != Right.operandCount() ||
      Left.memOperands().size() != Right.memOperands().size() ||
      Left.sourceLocation() != Right.sourceLocation() ||
      Left.debugLocation() != Right.debugLocation() || Left.origin() != Right.origin() ||
      hasProtectedAttachment(Left) || hasProtectedAttachment(Right) ||
      Descriptor->Flags.has(InstructionFlag::Call) ||
      Descriptor->Flags.has(InstructionFlag::Convergent) ||
      Descriptor->Flags.has(InstructionFlag::NotDuplicable) ||
      Left.flags().has(MachineInstrFlag::NoMerge) ||
      Left.flags().has(MachineInstrFlag::BundledWithPred) ||
      Left.flags().has(MachineInstrFlag::BundledWithSucc)) {
    return false;
  }
  for (std::size_t Index = 0; Index < Left.operandCount(); ++Index) {
    const MachineOperand *LeftOperand = Left.operandAt(Index);
    const MachineOperand *RightOperand = Right.operandAt(Index);
    if (LeftOperand == nullptr || RightOperand == nullptr ||
        !operandEquals(*LeftOperand, *RightOperand)) {
      return false;
    }
  }
  for (std::size_t Index = 0; Index < Left.memOperands().size(); ++Index) {
    const MachineMemOperand *LeftMemory = Left.memOperands()[Index];
    const MachineMemOperand *RightMemory = Right.memOperands()[Index];
    if (LeftMemory == nullptr || RightMemory == nullptr ||
        LeftMemory->description() != RightMemory->description()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Expected<bool> blockEquals(const MachineBasicBlock &Left,
                                         const MachineBasicBlock &Right,
                                         const MachineFunction &Function,
                                         const TargetDescription &Target) {
  if (&Left == &Right || &Left == Function.firstBlock() || &Right == Function.firstBlock() ||
      hasProtectedIdentity(Left, Function) || hasProtectedIdentity(Right, Function) ||
      Left.attributes().alignment() != Right.attributes().alignment() ||
      Left.attributes().sectionID() != Right.attributes().sectionID() ||
      Left.attributes().callFrameSize() != Right.attributes().callFrameSize() ||
      !std::ranges::equal(Left.liveIns(), Right.liveIns()) ||
      !std::ranges::equal(Left.successors(), Right.successors()) ||
      Left.instructionCount() != Right.instructionCount()) {
    return false;
  }
  const MachineInstr *LeftInstruction = Left.firstInstruction();
  const MachineInstr *RightInstruction = Right.firstInstruction();
  while (LeftInstruction != nullptr && RightInstruction != nullptr) {
    auto Equal = instructionEquals(*LeftInstruction, *RightInstruction, Target);
    if (!Equal)
      return std::unexpected(Equal.error());
    if (!*Equal)
      return false;
    LeftInstruction = LeftInstruction->next();
    RightInstruction = RightInstruction->next();
  }
  return LeftInstruction == nullptr && RightInstruction == nullptr;
}

[[nodiscard]] Expected<bool> canRedirectPredecessor(MachineBasicBlock &Predecessor,
                                                    MachineBasicBlock &Duplicate,
                                                    MachineFunction &Function,
                                                    const TargetDescription &Target,
                                                    const TargetBranchInfo &Info) {
  auto Analysis = Info.analyzeBranch(Predecessor, Function, Target);
  if (!Analysis)
    return std::unexpected(Analysis.error());
  if (Analysis->HasUnanalyzableTerminator)
    return false;
  if ((Analysis->Conditional != nullptr && Analysis->TrueTarget == &Duplicate &&
       hasProtectedAttachment(*Analysis->Conditional)) ||
      (Analysis->Unconditional != nullptr && Analysis->TrueTarget == &Duplicate &&
       hasProtectedAttachment(*Analysis->Unconditional))) {
    return false;
  }
  if (Analysis->TrueTarget == &Duplicate || Analysis->FalseTarget == &Duplicate)
    return true;
  return Analysis->Conditional == nullptr && Analysis->Unconditional == nullptr &&
         Predecessor.successors().size() == 1 && Predecessor.next() == &Duplicate;
}

[[nodiscard]] Expected<bool>
redirectPredecessor(MachineBasicBlock &Predecessor, MachineBasicBlock &Duplicate,
                    MachineBasicBlock &Canonical, MachineFunction &Function,
                    const TargetDescription &Target, const TargetBranchInfo &Info) {
  auto Analysis = Info.analyzeBranch(Predecessor, Function, Target);
  if (!Analysis)
    return std::unexpected(Analysis.error());
  auto Rewritten = Info.rewriteBranchTarget(Predecessor, Duplicate, Canonical, Function, Target);
  if (!Rewritten)
    return std::unexpected(Rewritten.error());
  if (!*Rewritten) {
    const bool IsImplicitEdge =
        (Analysis->Conditional == nullptr && Analysis->Unconditional == nullptr &&
         Predecessor.successors().size() == 1 && Predecessor.next() == &Duplicate) ||
        (Analysis->Conditional != nullptr && Analysis->Unconditional == nullptr &&
         Analysis->FalseTarget == &Duplicate);
    if (!IsImplicitEdge)
      return false;
    auto Inserted = Info.insertUnconditionalBranch(Predecessor, Canonical, Function, Target);
    if (!Inserted)
      return std::unexpected(Inserted.error());
  }
  MachineCFGUpdater CFG = Function.cfgUpdater();
  auto Redirected = CFG.replaceSuccessor(Predecessor, Duplicate, Canonical);
  if (!Redirected)
    return std::unexpected(Redirected.error());
  return true;
}

} // namespace

const MachinePassContract &MachineTailMergePass::contract() const noexcept {
  return tailMergeContract();
}

Expected<MachinePassResult> MachineTailMergePass::run(MachineFunction &Function,
                                                      const TargetDescription &Target,
                                                      MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "tail merge requires finalized allocated tracked-liveness MIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(prefixedError(Trial.error(), "tail merge transaction setup failed: "));
  }
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = runImplementation(Function, Target);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "tail merge diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachineTailMergePass::runImplementation(MachineFunction &Function,
                                        const TargetDescription &Target) const {
  MachinePassResult Result;
  std::size_t MergedBlocks = 0;
  bool LocalChange = true;
  while (LocalChange && MergedBlocks < Options.MaximumMergedBlocks) {
    LocalChange = false;
    std::vector<MachineBasicBlock *> Blocks;
    for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
         Block = Block->next()) {
      Blocks.push_back(Block);
    }
    for (std::size_t DuplicateIndex = 1; DuplicateIndex < Blocks.size(); ++DuplicateIndex) {
      MachineBasicBlock *Duplicate = Blocks[DuplicateIndex];
      if (Duplicate == nullptr || !Duplicate->isLinked())
        continue;
      for (std::size_t CanonicalIndex = 1; CanonicalIndex < DuplicateIndex; ++CanonicalIndex) {
        MachineBasicBlock *Canonical = Blocks[CanonicalIndex];
        if (Canonical == nullptr || !Canonical->isLinked())
          continue;
        auto Equal = blockEquals(*Canonical, *Duplicate, Function, Target);
        if (!Equal)
          return std::unexpected(Equal.error());
        if (!*Equal)
          continue;

        const std::vector<MachineBasicBlock *> Predecessors(Duplicate->predecessors().begin(),
                                                            Duplicate->predecessors().end());
        bool RedirectedAll = true;
        for (MachineBasicBlock *Predecessor : Predecessors) {
          if (Predecessor == nullptr || Predecessor == Canonical || Predecessor == Duplicate) {
            RedirectedAll = false;
            break;
          }
          auto CanRedirect =
              canRedirectPredecessor(*Predecessor, *Duplicate, Function, Target, *Info);
          if (!CanRedirect)
            return std::unexpected(CanRedirect.error());
          if (!*CanRedirect) {
            RedirectedAll = false;
            break;
          }
        }
        if (!RedirectedAll)
          continue;
        for (MachineBasicBlock *Predecessor : Predecessors) {
          auto Redirected =
              redirectPredecessor(*Predecessor, *Duplicate, *Canonical, Function, Target, *Info);
          if (!Redirected)
            return std::unexpected(Redirected.error());
          if (!*Redirected) {
            return makeUnexpected(ErrorCode::CFGOptimizationFailed,
                                  "tail merge predecessor changed after redirect preflight");
          }
        }
        MachineCFGUpdater CFG = Function.cfgUpdater();
        auto Deleted = CFG.deleteBlock(*Duplicate);
        if (!Deleted)
          return std::unexpected(Deleted.error());
        ++MergedBlocks;
        Result.Changed = true;
        LocalChange = true;
        break;
      }
      if (LocalChange)
        break;
    }
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
