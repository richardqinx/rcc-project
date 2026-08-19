// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineShrinkWrapping.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetFrameLowering.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &shrinkWrappingContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-shrink-wrapping",
        .Requires = {MachineFunctionProperty::NoVRegs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                             MachineFunctionAnalysisManager::id<MachineLoopAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Frame};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] bool requiresEntryFrame(const MachineFrameState &State) noexcept {
  return State.IsFrameAddressTaken || State.IsReturnAddressTaken || State.HasStackMap ||
         State.HasPatchPoint || State.HasOpaqueSPAdjustment || State.HasVAStart ||
         State.HasMustTailInVarArgFunc || State.HasTailCall;
}

[[nodiscard]] bool containsEpilogueTerminator(const MachineBasicBlock &Block,
                                              const TargetFrameLowering &Info,
                                              const TargetDescription &Target) noexcept {
  for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Info.isEpilogueTerminator(*Instruction, Target))
      return true;
  }
  return false;
}

} // namespace

const MachinePassContract &MachineShrinkWrappingPass::contract() const noexcept {
  return shrinkWrappingContract();
}

Expected<MachinePassResult>
MachineShrinkWrappingPass::run(MachineFunction &Function, const TargetDescription &Target,
                               MachineFunctionAnalysisManager &Analyses) {
  if (Function.frameInfo().isFrozen()) {
    return makeUnexpected(ErrorCode::FrameLayoutFailed,
                          "shrink wrapping must run before frame layout freeze");
  }
  if (Function.blockCount() > Options.MaximumAnalyzedBlocks) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }

  const MachineFrameState &OriginalState = Function.frameInfo().state();
  if (!OriginalState.SavePoints.empty() || !OriginalState.RestorePoints.empty()) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }
  MachineBasicBlock *Entry = Function.firstBlock();
  if (Entry == nullptr || requiresEntryFrame(OriginalState)) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }

  auto Dominators = Analyses.getResult<MachineDominatorTreeAnalysis>(Function, Target);
  if (!Dominators)
    return std::unexpected(Dominators.error());
  auto Loops = Analyses.getResult<MachineLoopAnalysis>(Function, Target);
  if (!Loops)
    return std::unexpected(Loops.error());

  std::vector<MachineBasicBlock *> RequiredBlocks;
  std::unordered_set<FrameIndex> ReferencedObjects;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    bool Required = false;
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "shrink wrapping encountered an unknown opcode");
      }
      Required |= Descriptor->Flags.has(InstructionFlag::Call);
      for (const MachineOperand &Operand : Instruction->operands()) {
        if (const auto *Frame = Operand.getIf<FrameIndexOperand>()) {
          ReferencedObjects.insert(Frame->Index);
          Required = true;
        }
        const Register *Reg = Operand.getIf<Register>();
        const std::optional<PhysicalRegister> Physical =
            Reg == nullptr ? std::nullopt : Reg->physicalRegister();
        if (Physical && Operand.isDef() &&
            Info->isCalleeSavedRegister(*Physical, Function, Target)) {
          Required = true;
        }
      }
      for (const MachineMemOperand *Memory : Instruction->memOperands()) {
        const std::optional<FrameIndex> Frame =
            Memory == nullptr ? std::nullopt : Memory->description().Base.frameIndex();
        if (Frame) {
          ReferencedObjects.insert(*Frame);
          Required = true;
        }
      }
    }
    if (Required)
      RequiredBlocks.push_back(Block);
  }

  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description == nullptr || Description->IsDead)
      continue;
    if (!ReferencedObjects.contains(Object.index())) {
      RequiredBlocks.push_back(Entry);
      break;
    }
  }
  if (RequiredBlocks.empty()) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }

  const MachineBasicBlock *Save = RequiredBlocks.front();
  if (!(*Dominators)->isReachable(*Save))
    Save = Entry;
  for (MachineBasicBlock *Block : RequiredBlocks) {
    if (!(*Dominators)->isReachable(*Block)) {
      Save = Entry;
      break;
    }
    const MachineBasicBlock *Common = (*Dominators)->nearestCommonDominator(*Save, *Block);
    if (Common == nullptr) {
      Save = Entry;
      break;
    }
    Save = Common;
  }
  if (Save == Entry || Save->predecessors().size() != 1 || (*Loops)->loopDepth(*Save) != 0 ||
      (*Loops)->isIrreducible(*Save)) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }

  std::vector<MachineBasicBlock *> RestoreBlocks;
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    if (!(*Dominators)->dominates(*Save, *Block))
      continue;
    for (const MachineSuccessor &Successor : Block->successors()) {
      if (Successor.Block == nullptr || !(*Dominators)->dominates(*Save, *Successor.Block)) {
        MachinePassResult Result;
        Result.Preserved = PreservedAnalyses::all();
        return Result;
      }
    }
    if (!Block->successors().empty())
      continue;
    if (!containsEpilogueTerminator(*Block, *Info, Target)) {
      MachinePassResult Result;
      Result.Preserved = PreservedAnalyses::all();
      return Result;
    }
    RestoreBlocks.push_back(Block);
  }
  if (RestoreBlocks.empty()) {
    MachinePassResult Result;
    Result.Preserved = PreservedAnalyses::all();
    return Result;
  }

  MachineFrameState Planned = OriginalState;
  Planned.SavePoints.push_back({Save->stableID(), {}});
  std::ranges::sort(RestoreBlocks, {},
                    [](const MachineBasicBlock *Block) { return Block->stableID().value(); });
  for (const MachineBasicBlock *Block : RestoreBlocks)
    Planned.RestorePoints.push_back({Block->stableID(), {}});
  auto Updated = Function.frameInfo().setState(std::move(Planned));
  if (!Updated)
    return std::unexpected(Updated.error());

  MachinePassResult Result;
  Result.Changed = true;
  Result.Changes = {MachinePassChange::Frame};
  Result.Preserved.preserve<MachineDominatorTreeAnalysis>();
  Result.Preserved.preserve<MachineLoopAnalysis>();
  Result.Preserved.preserve<MachineBranchProbabilityAnalysis>();
  Result.Preserved.preserve<MachineBlockFrequencyAnalysis>();
  return Result;
}

} // namespace rcc
