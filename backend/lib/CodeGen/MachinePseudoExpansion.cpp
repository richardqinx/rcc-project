// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePseudoExpansion.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetBranchInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace rcc {
namespace {

using CFGEdge = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

struct ResourceSnapshot final {
  std::size_t BlockCount = 0;
  std::size_t VirtualRegisterCount = 0;
  std::size_t FrameObjectCount = 0;
  bool FrameFrozen = false;
  std::vector<CFGEdge> Edges;
};

using BlockSizeSnapshot = std::vector<std::pair<MachineBlockStableID, std::uint64_t>>;

[[nodiscard]] Expected<std::uint64_t> blockSize(const MachineBasicBlock &Block,
                                                const MachineFunction &Function,
                                                const TargetDescription &Target,
                                                const TargetBranchInfo &BranchInfo) {
  std::uint64_t Size = 0;
  for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    auto InstructionSize = BranchInfo.instructionSize(*Instruction, Function, Target);
    if (!InstructionSize)
      return std::unexpected(InstructionSize.error());
    if (Size > std::numeric_limits<std::uint64_t>::max() - *InstructionSize) {
      return makeUnexpected(ErrorCode::PseudoExpansionFailed, "target-late pseudo size overflowed");
    }
    Size += *InstructionSize;
  }
  return Size;
}

[[nodiscard]] Expected<BlockSizeSnapshot> snapshotBlockSizes(const MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             const TargetBranchInfo &BranchInfo) {
  BlockSizeSnapshot Result;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    auto Size = blockSize(*Block, Function, Target, BranchInfo);
    if (!Size)
      return std::unexpected(Size.error());
    Result.emplace_back(Block->stableID(), *Size);
  }
  return Result;
}

[[nodiscard]] ResourceSnapshot snapshotResources(const MachineFunction &Function) {
  ResourceSnapshot Result;
  Result.BlockCount = Function.blockCount();
  Result.VirtualRegisterCount = Function.registerInfo().virtualRegisters().size();
  Result.FrameObjectCount = Function.frameInfo().objects().size();
  Result.FrameFrozen = Function.frameInfo().isFrozen();
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineSuccessor &Successor : Block->successors()) {
      Result.Edges.emplace_back(Block->stableID().value(), Successor.Block->stableID().value(),
                                Successor.Probability.rawValue());
    }
  }
  return Result;
}

[[nodiscard]] Expected<void> verifyResourceContract(const ResourceSnapshot &Before,
                                                    const MachineFunction &Function,
                                                    MachinePseudoExpansionPhase Phase) {
  const ResourceSnapshot After = snapshotResources(Function);
  if (Before.BlockCount != After.BlockCount || Before.Edges != After.Edges) {
    return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                          "pseudo expansion changed CFG resources outside its owner contract");
  }
  if (Before.VirtualRegisterCount != After.VirtualRegisterCount) {
    return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                          "post-selection pseudo expansion created a virtual register");
  }
  if (Phase != MachinePseudoExpansionPhase::PrePEI &&
      (Before.FrameObjectCount != After.FrameObjectCount ||
       Before.FrameFrozen != After.FrameFrozen)) {
    return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                          "post-PEI pseudo expansion changed frozen frame resources");
  }
  return {};
}

[[nodiscard]] PseudoOwner ownerFor(MachinePseudoExpansionPhase Phase) noexcept {
  switch (Phase) {
  case MachinePseudoExpansionPhase::PrePEI:
    return PseudoOwner::PrePEIExpansion;
  case MachinePseudoExpansionPhase::PostRA:
    return PseudoOwner::PostRAExpansion;
  case MachinePseudoExpansionPhase::TargetLate:
    return PseudoOwner::TargetLateExpansion;
  }
  return PseudoOwner::None;
}

[[nodiscard]] const MachinePassContract &preparationContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "prepare-post-ra-pseudos",
        .Requires = {MachineFunctionProperty::Selected},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Operands};
    return Value;
  }();
  return Contract;
}

[[nodiscard]] const MachinePassContract &phaseContract(MachinePseudoExpansionPhase Phase) {
  static const MachinePassContract PrePEI = [] {
    MachinePassContract Value{
        .Name = "pre-pei-target-pseudo-expansion",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::NoVRegs, MachineFunctionProperty::TracksLiveness},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands,
                             MachinePassChange::Frame};
    return Value;
  }();
  static const MachinePassContract PostRA = [] {
    MachinePassContract Value{
        .Name = "expand-post-ra-pseudos",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::NoVRegs, MachineFunctionProperty::TracksLiveness,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::PostRAFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  static const MachinePassContract TargetLate = [] {
    MachinePassContract Value{
        .Name = "target-late-pseudo-expansion",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::NoVRegs, MachineFunctionProperty::TracksLiveness,
                     MachineFunctionProperty::FrameFinalized,
                     MachineFunctionProperty::NoFrameIndices},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {},
        .VerificationStage = PipelineStage::PostRAFinalized,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  switch (Phase) {
  case MachinePseudoExpansionPhase::PrePEI:
    return PrePEI;
  case MachinePseudoExpansionPhase::PostRA:
    return PostRA;
  case MachinePseudoExpansionPhase::TargetLate:
    return TargetLate;
  }
  return TargetLate;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

[[nodiscard]] Expected<MachinePassResult>
prepareImplementation(MachineFunction &Function, const TargetDescription &Target,
                      const TargetPseudoExpansionInfo &Info) {
  bool Changed = false;
  for (MachineInstr *Instruction : Function.allInstructions()) {
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "pseudo preparation encountered an unknown opcode");
    if (Descriptor->Owner != PseudoOwner::PostRAExpansion)
      continue;
    auto Prepared = Info.preparePostRAPseudo(*Instruction, Function, Target);
    if (!Prepared)
      return std::unexpected(Prepared.error());
    Changed |= *Prepared;
  }
  MachinePassResult Result;
  Result.Changed = Changed;
  if (Changed) {
    Result.Changes = {MachinePassChange::Operands};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

[[nodiscard]] Expected<MachinePassResult>
expandImplementation(MachineFunction &Function, const TargetDescription &Target,
                     const TargetPseudoExpansionInfo &Info, MachinePseudoExpansionPhase Phase,
                     const TargetBranchInfo *BranchInfo) {
  const ResourceSnapshot Before = snapshotResources(Function);
  std::optional<BlockSizeSnapshot> SizesBefore;
  if (Phase == MachinePseudoExpansionPhase::TargetLate) {
    if (BranchInfo == nullptr) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target-late pseudo expansion requires branch sizing");
    }
    auto Snapshot = snapshotBlockSizes(Function, Target, *BranchInfo);
    if (!Snapshot)
      return std::unexpected(Snapshot.error());
    SizesBefore = std::move(*Snapshot);
  }
  std::vector<MachineInstr *> Worklist;
  for (MachineInstr *Instruction : Function.allInstructions()) {
    if (Instruction != nullptr && Instruction->parent() != nullptr)
      Worklist.push_back(Instruction);
  }
  bool Changed = false;
  for (MachineInstr *Instruction : Worklist) {
    if (Instruction->parent() == nullptr)
      continue;
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "pseudo expansion encountered an unknown opcode");
    if (!Descriptor->Flags.has(InstructionFlag::Pseudo) || Descriptor->Owner != ownerFor(Phase)) {
      continue;
    }
    MachineBasicBlock *ExpansionBlock = Instruction->parent();
    std::optional<std::uint64_t> ExpansionSizeBefore;
    if (Phase == MachinePseudoExpansionPhase::TargetLate) {
      auto Size = blockSize(*ExpansionBlock, Function, Target, *BranchInfo);
      if (!Size)
        return std::unexpected(Size.error());
      ExpansionSizeBefore = *Size;
    }
    auto Expanded = Info.expandPseudo(*Instruction, Phase, Function, Target);
    if (!Expanded)
      return std::unexpected(Expanded.error());
    if (!*Expanded) {
      return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                            "target has no expansion for an owned executable pseudo");
    }
    if (ExpansionSizeBefore.has_value()) {
      auto SizeAfter = blockSize(*ExpansionBlock, Function, Target, *BranchInfo);
      if (!SizeAfter)
        return std::unexpected(SizeAfter.error());
      if (*SizeAfter > *ExpansionSizeBefore) {
        return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                              "one target-late pseudo expansion increased its encoded block size");
      }
    }
    Changed = true;
  }

  auto Resources = verifyResourceContract(Before, Function, Phase);
  if (!Resources)
    return std::unexpected(Resources.error());
  if (SizesBefore.has_value()) {
    auto SizesAfter = snapshotBlockSizes(Function, Target, *BranchInfo);
    if (!SizesAfter)
      return std::unexpected(SizesAfter.error());
    if (SizesAfter->size() != SizesBefore->size()) {
      return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                            "target-late pseudo expansion changed block layout");
    }
    for (std::size_t Index = 0; Index < SizesAfter->size(); ++Index) {
      if ((*SizesAfter)[Index].first != (*SizesBefore)[Index].first ||
          (*SizesAfter)[Index].second > (*SizesBefore)[Index].second) {
        return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                              "target-late pseudo expansion increased an encoded block size");
      }
    }
  }
  for (MachineInstr *Instruction : Function.allInstructions()) {
    if (Instruction == nullptr || Instruction->parent() == nullptr)
      continue;
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor != nullptr && Descriptor->Flags.has(InstructionFlag::Pseudo) &&
        Descriptor->Owner == ownerFor(Phase)) {
      return makeUnexpected(ErrorCode::PseudoExpansionFailed,
                            "pseudo expansion left an instruction owned by the completed phase");
    }
  }
  if (Changed && Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
    auto Rebuilt = rebuildMachineLiveness(Function, Target);
    if (!Rebuilt)
      return std::unexpected(Rebuilt.error());
  }
  MachinePassResult Result;
  Result.Changed = Changed;
  if (Changed) {
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands};
    Result.Preserved = PreservedAnalyses::none();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

[[nodiscard]] Expected<MachinePassResult>
runPreparationTransaction(MachineFunction &Function, const TargetDescription &Target,
                          const TargetPseudoExpansionInfo &Info) {
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(prefixedError(Trial.error(), "pseudo preparation setup failed: "));
  auto TrialResult = prepareImplementation(*Trial->ClonedFunction, Target, Info);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = prepareImplementation(Function, Target, Info);
  if (!Result) {
    return std::unexpected(
        prefixedError(Result.error(), "pseudo preparation diverged after successful preflight: "));
  }
  return Result;
}

[[nodiscard]] Expected<MachinePassResult>
runExpansionTransaction(MachineFunction &Function, const TargetDescription &Target,
                        const TargetPseudoExpansionInfo &Info, MachinePseudoExpansionPhase Phase,
                        const TargetBranchInfo *BranchInfo) {
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial)
    return std::unexpected(prefixedError(Trial.error(), "pseudo expansion setup failed: "));
  auto TrialResult = expandImplementation(*Trial->ClonedFunction, Target, Info, Phase, BranchInfo);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());
  auto Result = expandImplementation(Function, Target, Info, Phase, BranchInfo);
  if (!Result) {
    return std::unexpected(
        prefixedError(Result.error(), "pseudo expansion diverged after successful preflight: "));
  }
  return Result;
}

} // namespace

const MachinePassContract &PreparePostRAPseudosPass::contract() const noexcept {
  return preparationContract();
}

Expected<MachinePassResult> PreparePostRAPseudosPass::run(MachineFunction &Function,
                                                          const TargetDescription &Target,
                                                          MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires))
    return makeUnexpected(ErrorCode::VerificationError,
                          "post-RA pseudo preparation requires selected MachineIR");
  return runPreparationTransaction(Function, Target, *Info);
}

const MachinePassContract &PrePEITargetPseudoExpansionPass::contract() const noexcept {
  return phaseContract(MachinePseudoExpansionPhase::PrePEI);
}

Expected<MachinePassResult> PrePEITargetPseudoExpansionPass::run(MachineFunction &Function,
                                                                 const TargetDescription &Target,
                                                                 MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires))
    return makeUnexpected(ErrorCode::VerificationError,
                          "pre-PEI pseudo expansion requires allocated tracked-liveness MIR");
  return runExpansionTransaction(Function, Target, *Info, MachinePseudoExpansionPhase::PrePEI,
                                 nullptr);
}

const MachinePassContract &ExpandPostRAPseudosPass::contract() const noexcept {
  return phaseContract(MachinePseudoExpansionPhase::PostRA);
}

Expected<MachinePassResult> ExpandPostRAPseudosPass::run(MachineFunction &Function,
                                                         const TargetDescription &Target,
                                                         MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires))
    return makeUnexpected(ErrorCode::VerificationError,
                          "post-RA pseudo expansion requires finalized frame and liveness");
  return runExpansionTransaction(Function, Target, *Info, MachinePseudoExpansionPhase::PostRA,
                                 nullptr);
}

const MachinePassContract &TargetLatePseudoExpansionPass::contract() const noexcept {
  return phaseContract(MachinePseudoExpansionPhase::TargetLate);
}

Expected<MachinePassResult> TargetLatePseudoExpansionPass::run(MachineFunction &Function,
                                                               const TargetDescription &Target,
                                                               MachineAnalysisManager &) {
  if (!Function.properties().containsAll(contract().Requires))
    return makeUnexpected(ErrorCode::VerificationError,
                          "target-late pseudo expansion requires finalized allocated MIR");
  return runExpansionTransaction(Function, Target, *Info, MachinePseudoExpansionPhase::TargetLate,
                                 BranchInfo);
}

} // namespace rcc
