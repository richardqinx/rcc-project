// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineSink.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &sinkContract(MachineCodeMotionStage Stage) {
  static const MachinePassContract Selected = [] {
    MachinePassContract Value{
        .Name = "machine-sink",
        .Requires = {MachineFunctionProperty::IsSSA},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                             MachineFunctionAnalysisManager::id<MachineBlockFrequencyAnalysis>()},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions};
    return Value;
  }();
  static const MachinePassContract PostRA = [] {
    MachinePassContract Value{
        .Name = "post-ra-machine-sink",
        .Requires = {MachineFunctionProperty::NoVRegs},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                             MachineFunctionAnalysisManager::id<MachineBlockFrequencyAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Stage == MachineCodeMotionStage::SelectedSSA ? Selected : PostRA;
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value() ||
         Instruction.flags().has(MachineInstrFlag::BundledWithPred) ||
         Instruction.flags().has(MachineInstrFlag::BundledWithSucc);
}

[[nodiscard]] Expected<bool> isSafeToSink(const MachineInstr &Instruction,
                                          const MachineFunction &Function,
                                          const TargetDescription &Target,
                                          MachineCodeMotionStage Stage) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine sink encountered an instruction without a descriptor");
  }
  const InstructionFlags Flags = Descriptor->Flags;
  if (Flags.has(InstructionFlag::Pseudo) || Flags.has(InstructionFlag::Meta) ||
      Flags.has(InstructionFlag::Phi) || Flags.has(InstructionFlag::Terminator) ||
      Flags.has(InstructionFlag::Branch) || Flags.has(InstructionFlag::Call) ||
      Flags.has(InstructionFlag::Return) || Flags.has(InstructionFlag::Barrier) ||
      Flags.has(InstructionFlag::MayLoad) || Flags.has(InstructionFlag::MayStore) ||
      Flags.has(InstructionFlag::HasSideEffects) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) || Flags.has(InstructionFlag::Convergent) ||
      hasProtectedAttachment(Instruction) || !Instruction.memOperands().empty()) {
    return false;
  }
  if (Flags.has(InstructionFlag::MayRaiseFPException) &&
      !Instruction.flags().has(MachineInstrFlag::NoFPExcept))
    return false;

  bool HasDefinition = false;
  for (const MachineOperand &Operand : Instruction.operands()) {
    if (Operand.kind() == MachineOperand::Kind::RegisterMask ||
        Operand.kind() == MachineOperand::Kind::LiveOutRegisterMask) {
      return false;
    }
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr)
      continue;
    if (Operand.isDebugUse() || Operand.isImplicit() || Operand.tiedOperandIndex() ||
        Operand.registerFlags().has(RegisterOperandFlag::EarlyClobber) ||
        (Operand.isDef() && Operand.isUse())) {
      return false;
    }
    if (Stage == MachineCodeMotionStage::PostRegisterAllocation) {
      if (!Reg->isPhysical())
        return false;
      HasDefinition |= Operand.isDef();
      continue;
    }
    if (!Reg->isVirtual())
      return false;
    if (Operand.isDef())
      HasDefinition = true;
    auto Info = Function.registerInfo().virtualRegisterInfo(*Reg);
    if (!Info)
      return std::unexpected(Info.error());
  }
  return HasDefinition;
}

struct SinkDestination final {
  MachineBasicBlock *Block = nullptr;
  MachineInstr *FirstUse = nullptr;
};

[[nodiscard]] std::optional<SinkDestination> findDestination(MachineInstr &Instruction,
                                                             MachineRegisterInfo &Registers) {
  MachineBasicBlock *Source = Instruction.parent();
  if (Source == nullptr)
    return std::nullopt;
  std::unordered_set<Register> Definitions;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg != nullptr && Operand.isDef())
      Definitions.insert(*Reg);
  }
  if (Definitions.empty())
    return std::nullopt;

  MachineBasicBlock *Destination = nullptr;
  for (const Register Reg : Definitions) {
    for (MachineOperand &Use : Registers.useOperands(Reg, false)) {
      MachineInstr *UseInstruction = Use.parent();
      MachineBasicBlock *UseBlock = UseInstruction == nullptr ? nullptr : UseInstruction->parent();
      if (UseBlock == nullptr || UseBlock == Source)
        return std::nullopt;
      if (Destination == nullptr)
        Destination = UseBlock;
      else if (Destination != UseBlock)
        return std::nullopt;
    }
  }
  if (Destination == nullptr || Destination->predecessors().size() != 1 ||
      Destination->predecessors().front() != Source ||
      std::ranges::find_if(Source->successors(), [Destination](const MachineSuccessor &Successor) {
        return Successor.Block == Destination;
      }) == Source->successors().end()) {
    return std::nullopt;
  }

  MachineInstr *FirstUse = nullptr;
  for (MachineInstr *Candidate = Destination->firstInstruction(); Candidate != nullptr;
       Candidate = Candidate->next()) {
    const bool UsesDefinition =
        std::ranges::any_of(Candidate->operands(), [&Definitions](const MachineOperand &Operand) {
          const Register *Reg = Operand.getIf<Register>();
          return Reg != nullptr && Operand.isUse() && !Operand.isDebugUse() &&
                 Definitions.contains(*Reg);
        });
    if (UsesDefinition) {
      FirstUse = Candidate;
      break;
    }
  }
  return FirstUse == nullptr ? std::nullopt
                             : std::optional<SinkDestination>{{Destination, FirstUse}};
}

[[nodiscard]] std::optional<SinkDestination>
findPostRADestination(MachineInstr &Instruction, const MachineFunction &Function,
                      const TargetDescription &Target) {
  MachineBasicBlock *Source = Instruction.parent();
  if (Source == nullptr || Source->successors().empty())
    return std::nullopt;

  std::vector<PhysicalRegister> Uses;
  std::vector<PhysicalRegister> Definitions;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    const std::optional<PhysicalRegister> Physical =
        Reg == nullptr ? std::nullopt : Reg->physicalRegister();
    if (!Physical)
      continue;
    if (Operand.isUse())
      Uses.push_back(*Physical);
    if (Operand.isDef()) {
      if (Target.registers().isReservedRegister(*Physical) ||
          Function.registerInfo().isReserved(*Physical)) {
        return std::nullopt;
      }
      Definitions.push_back(*Physical);
    }
  }
  if (Definitions.empty())
    return std::nullopt;
  MachineBasicBlock *Destination = nullptr;
  for (const MachineSuccessor &Successor : Source->successors()) {
    if (Successor.Block == nullptr)
      return std::nullopt;
    const bool CarriesAny = std::ranges::any_of(Definitions, [&](PhysicalRegister Definition) {
      return std::ranges::any_of(
          Successor.Block->liveIns(), [&](const MachineBasicBlockLiveIn &LiveIn) {
            return Target.registers().regsOverlap(Definition, LiveIn.PhysReg);
          });
    });
    const bool CarriesAll = std::ranges::all_of(Definitions, [&](PhysicalRegister Definition) {
      return std::ranges::any_of(
          Successor.Block->liveIns(), [&](const MachineBasicBlockLiveIn &LiveIn) {
            return Target.registers().regsOverlap(Definition, LiveIn.PhysReg);
          });
    });
    if (CarriesAny != CarriesAll || (CarriesAll && Destination != nullptr))
      return std::nullopt;
    if (CarriesAll)
      Destination = Successor.Block;
  }
  if (Destination == nullptr || Destination->predecessors().size() != 1 ||
      Destination->predecessors().front() != Source) {
    return std::nullopt;
  }
  for (std::size_t Left = 0; Left < Definitions.size(); ++Left) {
    if (!std::ranges::any_of(Destination->liveIns(),
                             [&](const MachineBasicBlockLiveIn &LiveIn) {
                               return Target.registers().regsOverlap(Definitions[Left],
                                                                     LiveIn.PhysReg);
                             }) ||
        std::ranges::any_of(Uses, [&](PhysicalRegister Use) {
          return Target.registers().regsOverlap(Definitions[Left], Use);
        })) {
      return std::nullopt;
    }
    for (std::size_t Right = Left + 1U; Right < Definitions.size(); ++Right) {
      if (Target.registers().regsOverlap(Definitions[Left], Definitions[Right]))
        return std::nullopt;
    }
  }

  const auto OverlapsAny = [&](PhysicalRegister Reg, std::span<const PhysicalRegister> Values) {
    return std::ranges::any_of(
        Values, [&](PhysicalRegister Value) { return Target.registers().regsOverlap(Reg, Value); });
  };
  for (MachineInstr *After = Instruction.next(); After != nullptr; After = After->next()) {
    for (const MachineOperand &Operand : After->operands()) {
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
          Mask != nullptr && Mask->Mask != nullptr) {
        if (std::ranges::any_of(Uses,
                                [&](PhysicalRegister Reg) { return Mask->Mask->clobbers(Reg); }) ||
            std::ranges::any_of(Definitions,
                                [&](PhysicalRegister Reg) { return Mask->Mask->clobbers(Reg); })) {
          return std::nullopt;
        }
        continue;
      }
      const Register *Reg = Operand.getIf<Register>();
      const std::optional<PhysicalRegister> Physical =
          Reg == nullptr ? std::nullopt : Reg->physicalRegister();
      if (!Physical)
        continue;
      if ((Operand.isUse() || Operand.isDef()) && OverlapsAny(*Physical, Definitions)) {
        return std::nullopt;
      }
      if (Operand.isDef() && OverlapsAny(*Physical, Uses))
        return std::nullopt;
    }
  }

  std::vector<bool> SeenUse(Definitions.size());
  MachineInstr *FirstUse = nullptr;
  for (MachineInstr *Candidate = Destination->firstInstruction(); Candidate != nullptr;
       Candidate = Candidate->next()) {
    for (const MachineOperand &Operand : Candidate->operands()) {
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
          Mask != nullptr && Mask->Mask != nullptr) {
        for (std::size_t Index = 0; Index < Definitions.size(); ++Index) {
          if (!SeenUse[Index] && Mask->Mask->clobbers(Definitions[Index]))
            return std::nullopt;
        }
        if (FirstUse == nullptr && std::ranges::any_of(Uses, [&](PhysicalRegister Reg) {
              return Mask->Mask->clobbers(Reg);
            })) {
          return std::nullopt;
        }
        continue;
      }
      const Register *Reg = Operand.getIf<Register>();
      const std::optional<PhysicalRegister> Physical =
          Reg == nullptr ? std::nullopt : Reg->physicalRegister();
      if (!Physical)
        continue;
      for (std::size_t Index = 0; Index < Definitions.size(); ++Index) {
        if (!Target.registers().regsOverlap(*Physical, Definitions[Index]))
          continue;
        if (Operand.isUse() && !Operand.isDebugUse()) {
          SeenUse[Index] = true;
          if (FirstUse == nullptr)
            FirstUse = Candidate;
        }
        if (Operand.isDef() && !SeenUse[Index])
          return std::nullopt;
      }
      if (FirstUse == nullptr && Operand.isDef() && OverlapsAny(*Physical, Uses)) {
        return std::nullopt;
      }
    }
  }
  if (FirstUse == nullptr || !std::ranges::all_of(SeenUse, [](bool Value) { return Value; }))
    return std::nullopt;
  return SinkDestination{Destination, FirstUse};
}

[[nodiscard]] Expected<void> moveBefore(MachineInstr &Instruction,
                                        const SinkDestination &Destination) {
  MachineBasicBlock *Source = Instruction.parent();
  if (Source == nullptr || Destination.Block == nullptr || Destination.FirstUse == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "machine sink has an invalid move plan");
  auto Removed = Source->removeInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  return Destination.Block->insertInstructionBefore(*Destination.FirstUse, Instruction);
}

} // namespace

const MachinePassContract &MachineSinkPass::contract() const noexcept {
  return sinkContract(Stage);
}

Expected<MachinePassResult> MachineSinkPass::run(MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 MachineFunctionAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(
        ErrorCode::VerificationError,
        Stage == MachineCodeMotionStage::SelectedSSA
            ? "machine sink requires SSA MachineIR"
            : "post-RA machine sink requires allocated tracked-liveness MachineIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(
        withErrorContext(Trial.error(), "machine sink transaction setup failed: "));
  }
  MachineFunctionAnalysisManager TrialAnalyses;
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, TrialAnalyses);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  auto Result = runImplementation(Function, Target, Analyses);
  if (!Result) {
    return std::unexpected(withErrorContext(
        Result.error(), "machine sink diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult>
MachineSinkPass::runImplementation(MachineFunction &Function, const TargetDescription &Target,
                                   MachineFunctionAnalysisManager &Analyses) {
  auto Dominators = Analyses.getResult<MachineDominatorTreeAnalysis>(Function, Target);
  if (!Dominators)
    return std::unexpected(Dominators.error());
  auto Frequencies = Analyses.getResult<MachineBlockFrequencyAnalysis>(Function, Target);
  if (!Frequencies)
    return std::unexpected(Frequencies.error());

  MachinePassResult Result;
  std::size_t Sunk = 0;
  for (MachineBasicBlock *Block = Function.firstBlock();
       Block != nullptr && Sunk < Options.MaximumSunkInstructions; Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction();
         Instruction != nullptr && Sunk < Options.MaximumSunkInstructions;) {
      MachineInstr *Next = Instruction->next();
      auto Safe = isSafeToSink(*Instruction, Function, Target, Stage);
      if (!Safe)
        return std::unexpected(Safe.error());
      if (!*Safe) {
        Instruction = Next;
        continue;
      }
      const std::optional<SinkDestination> Destination =
          Stage == MachineCodeMotionStage::SelectedSSA
              ? findDestination(*Instruction, Function.registerInfo())
              : findPostRADestination(*Instruction, Function, Target);
      if (!Destination || !(*Dominators)->dominates(*Block, *Destination->Block) ||
          (*Frequencies)->frequency(*Destination->Block) > (*Frequencies)->frequency(*Block)) {
        Instruction = Next;
        continue;
      }
      auto Moved = moveBefore(*Instruction, *Destination);
      if (!Moved)
        return std::unexpected(Moved.error());
      ++Sunk;
      Result.Changed = true;
      Instruction = Next;
    }
  }

  if (Result.Changed) {
    if (Function.properties().has(MachineFunctionProperty::TracksLiveness)) {
      auto Rebuilt = rebuildMachineLiveness(Function, Target);
      if (!Rebuilt)
        return std::unexpected(Rebuilt.error());
    }
    Result.Changes = {MachinePassChange::Instructions};
    Result.Preserved.preserve<MachineDominatorTreeAnalysis>();
    Result.Preserved.preserve<MachineBranchProbabilityAnalysis>();
    Result.Preserved.preserve<MachineBlockFrequencyAnalysis>();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
