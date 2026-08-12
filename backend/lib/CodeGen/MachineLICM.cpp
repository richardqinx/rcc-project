// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineLICM.h"

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
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &licmContract(MachineCodeMotionStage Stage) {
  static const MachinePassContract Selected = [] {
    MachinePassContract Value{
        .Name = "machine-licm",
        .Requires = {MachineFunctionProperty::IsSSA, MachineFunctionProperty::Selected},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                             MachineAnalysisManager::id<MachineLoopAnalysis>(),
                             MachineAnalysisManager::id<MachineBlockFrequencyAnalysis>()},
        .VerificationStage = PipelineStage::Selected,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions};
    return Value;
  }();
  static const MachinePassContract PostRA = [] {
    MachinePassContract Value{
        .Name = "post-ra-machine-licm",
        .Requires = {MachineFunctionProperty::Selected, MachineFunctionProperty::NoPHIs,
                     MachineFunctionProperty::TiedOpsRewritten, MachineFunctionProperty::NoVRegs,
                     MachineFunctionProperty::TracksLiveness},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineAnalysisManager::id<MachineDominatorTreeAnalysis>(),
                             MachineAnalysisManager::id<MachineLoopAnalysis>(),
                             MachineAnalysisManager::id<MachineBlockFrequencyAnalysis>()},
        .VerificationStage = PipelineStage::RegisterAllocated,
    };
    Value.ThreadSafety = PassThreadSafety::ThreadSafe;
    Value.DeclaredChanges = {MachinePassChange::Instructions, MachinePassChange::Operands};
    return Value;
  }();
  return Stage == MachineCodeMotionStage::SelectedSSA ? Selected : PostRA;
}

[[nodiscard]] Error prefixedError(const Error &Failure, std::string Prefix) {
  Prefix.append(Failure.message());
  return Error(Failure.code(), std::move(Prefix));
}

[[nodiscard]] bool containsBlock(const MachineLoop &Loop, const MachineBasicBlock *Block) noexcept {
  return std::ranges::find(Loop.Blocks, Block) != Loop.Blocks.end();
}

[[nodiscard]] MachineBasicBlock *preheader(const MachineLoop &Loop) noexcept {
  if (Loop.Header == nullptr)
    return nullptr;
  MachineBasicBlock *Result = nullptr;
  for (MachineBasicBlock *Predecessor : Loop.Header->predecessors()) {
    if (containsBlock(Loop, Predecessor))
      continue;
    if (Result != nullptr)
      return nullptr;
    Result = Predecessor;
  }
  if (Result == nullptr || Result->successors().size() != 1 ||
      Result->successors().front().Block != Loop.Header) {
    return nullptr;
  }
  return Result;
}

[[nodiscard]] bool hasProtectedAttachment(const MachineInstr &Instruction) noexcept {
  return Instruction.debugInstrNumber() != 0 || Instruction.preInstrSymbol().has_value() ||
         Instruction.postInstrSymbol().has_value() || Instruction.heapAllocMarker().has_value() ||
         Instruction.pcSections().has_value() || Instruction.mmra().has_value() ||
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value() ||
         Instruction.flags().has(MachineInstrFlag::BundledWithPred) ||
         Instruction.flags().has(MachineInstrFlag::BundledWithSucc);
}

[[nodiscard]] Expected<bool> isSafeToHoist(const MachineInstr &Instruction,
                                           const MachineFunction &Function,
                                           const TargetDescription &Target,
                                           MachineCodeMotionStage Stage) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine LICM encountered an instruction without a descriptor");
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
    const auto &Type = Info->get().Type;
    if (Type && Type->isFloat() && !Instruction.flags().has(MachineInstrFlag::NoFPExcept)) {
      return false;
    }
  }
  if (Stage == MachineCodeMotionStage::PostRegisterAllocation &&
      !Instruction.flags().has(MachineInstrFlag::NoFPExcept)) {
    return false;
  }
  return HasDefinition;
}

[[nodiscard]] std::size_t definitionCount(const MachineInstr &Instruction) noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      Instruction.operands(), [](const MachineOperand &Operand) { return Operand.isDef(); }));
}

[[nodiscard]] bool
operandsAreInvariant(const MachineInstr &Instruction, const MachineLoop &Loop,
                     const MachineRegisterInfo &Registers,
                     const std::unordered_set<const MachineInstr *> &Hoisted) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Operand.isUse())
      continue;
    if (!Reg->isVirtual())
      return false;
    const MachineOperand *Definition = Registers.getUniqueVRegDef(*Reg);
    const MachineInstr *DefiningInstruction =
        Definition == nullptr ? nullptr : Definition->parent();
    const MachineBasicBlock *DefiningBlock =
        DefiningInstruction == nullptr ? nullptr : DefiningInstruction->parent();
    if (DefiningInstruction == nullptr || DefiningBlock == nullptr)
      return false;
    if (containsBlock(Loop, DefiningBlock) && !Hoisted.contains(DefiningInstruction))
      return false;
  }
  return true;
}

[[nodiscard]] bool postRAOperandsAreInvariant(const MachineInstr &Instruction,
                                              const MachineLoop &Loop,
                                              const MachineFunction &Function,
                                              const TargetDescription &Target) noexcept {
  if (Loop.Header == nullptr || Instruction.parent() != Loop.Header)
    return false;

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
        return false;
      }
      Definitions.push_back(*Physical);
    }
  }
  if (Definitions.empty())
    return false;
  for (std::size_t Left = 0; Left < Definitions.size(); ++Left) {
    if (std::ranges::any_of(Loop.Header->liveIns(),
                            [&](const MachineBasicBlockLiveIn &LiveIn) {
                              return Target.registers().regsOverlap(Definitions[Left],
                                                                    LiveIn.PhysReg);
                            }) ||
        std::ranges::any_of(Uses, [&](PhysicalRegister Use) {
          return Target.registers().regsOverlap(Definitions[Left], Use);
        })) {
      return false;
    }
    for (std::size_t Right = Left + 1U; Right < Definitions.size(); ++Right) {
      if (Target.registers().regsOverlap(Definitions[Left], Definitions[Right]))
        return false;
    }
  }

  const auto ClobbersCandidate = [&](const RegisterMask &Mask) {
    return std::ranges::any_of(Uses, [&](PhysicalRegister Reg) { return Mask.clobbers(Reg); }) ||
           std::ranges::any_of(Definitions,
                               [&](PhysicalRegister Reg) { return Mask.clobbers(Reg); });
  };
  for (const MachineBasicBlock *Block : Loop.Blocks) {
    if (Block == nullptr)
      return false;
    for (const MachineInstr *Candidate = Block->firstInstruction(); Candidate != nullptr;
         Candidate = Candidate->next()) {
      if (Candidate == &Instruction)
        continue;
      for (const MachineOperand &Operand : Candidate->operands()) {
        if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
            Mask != nullptr && Mask->Mask != nullptr) {
          if (ClobbersCandidate(*Mask->Mask))
            return false;
          continue;
        }
        const Register *Reg = Operand.getIf<Register>();
        const std::optional<PhysicalRegister> Physical =
            Reg == nullptr ? std::nullopt : Reg->physicalRegister();
        if (!Physical || !Operand.isDef())
          continue;
        if (std::ranges::any_of(Uses,
                                [&](PhysicalRegister Use) {
                                  return Target.registers().regsOverlap(*Physical, Use);
                                }) ||
            std::ranges::any_of(Definitions, [&](PhysicalRegister Definition) {
              return Target.registers().regsOverlap(*Physical, Definition);
            })) {
          return false;
        }
      }
    }
  }

  for (const MachineInstr *Before = Loop.Header->firstInstruction();
       Before != nullptr && Before != &Instruction; Before = Before->next()) {
    for (const MachineOperand &Operand : Before->operands()) {
      if (const auto *Mask = Operand.getIf<RegisterMaskOperand>();
          Mask != nullptr && Mask->Mask != nullptr) {
        if (std::ranges::any_of(Definitions,
                                [&](PhysicalRegister Reg) { return Mask->Mask->clobbers(Reg); })) {
          return false;
        }
        continue;
      }
      const Register *Reg = Operand.getIf<Register>();
      const std::optional<PhysicalRegister> Physical =
          Reg == nullptr ? std::nullopt : Reg->physicalRegister();
      if (Physical && (Operand.isUse() || Operand.isDef()) &&
          std::ranges::any_of(Definitions, [&](PhysicalRegister Definition) {
            return Target.registers().regsOverlap(*Physical, Definition);
          })) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] Expected<MachineInstr *> firstTerminator(MachineBasicBlock &Block,
                                                       const TargetDescription &Target) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine LICM preheader contains an unknown instruction");
    }
    if (Descriptor->Flags.has(InstructionFlag::Terminator))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] Expected<void> moveToPreheader(MachineInstr &Instruction,
                                             MachineBasicBlock &Preheader,
                                             const TargetDescription &Target) {
  MachineBasicBlock *Source = Instruction.parent();
  if (Source == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "machine LICM candidate is detached");
  auto Anchor = firstTerminator(Preheader, Target);
  if (!Anchor)
    return std::unexpected(Anchor.error());
  auto Removed = Source->removeInstruction(Instruction);
  if (!Removed)
    return std::unexpected(Removed.error());
  if (*Anchor != nullptr)
    return Preheader.insertInstructionBefore(**Anchor, Instruction);
  return Preheader.appendInstruction(Instruction);
}

} // namespace

const MachinePassContract &MachineLICMPass::contract() const noexcept {
  return licmContract(Stage);
}

Expected<MachinePassResult> MachineLICMPass::run(MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 MachineAnalysisManager &Analyses) {
  if (!Function.properties().containsAll(contract().Requires)) {
    return makeUnexpected(
        ErrorCode::VerificationError,
        Stage == MachineCodeMotionStage::SelectedSSA
            ? "machine LICM requires selected SSA MachineIR"
            : "post-RA machine LICM requires allocated tracked-liveness MachineIR");
  }
  auto Trial = cloneMachineFunction(
      Function, Target,
      MachineFunctionCloneOptions{.NewStableID = Function.stableID(), .NewName = std::nullopt});
  if (!Trial) {
    return std::unexpected(prefixedError(Trial.error(), "machine LICM transaction setup failed: "));
  }
  MachineAnalysisManager TrialAnalyses;
  auto TrialResult = runImplementation(*Trial->ClonedFunction, Target, TrialAnalyses);
  if (!TrialResult)
    return std::unexpected(TrialResult.error());

  auto Result = runImplementation(Function, Target, Analyses);
  if (!Result) {
    return std::unexpected(prefixedError(
        Result.error(), "machine LICM diverged after a successful transaction preflight: "));
  }
  return Result;
}

Expected<MachinePassResult> MachineLICMPass::runImplementation(MachineFunction &Function,
                                                               const TargetDescription &Target,
                                                               MachineAnalysisManager &Analyses) {
  auto Loops = Analyses.getResult<MachineLoopAnalysis>(Function, Target);
  if (!Loops)
    return std::unexpected(Loops.error());
  auto Frequencies = Analyses.getResult<MachineBlockFrequencyAnalysis>(Function, Target);
  if (!Frequencies)
    return std::unexpected(Frequencies.error());

  MachinePassResult Result;
  std::size_t TotalHoisted = 0;
  for (const MachineLoop &Loop : (*Loops)->loops()) {
    MachineBasicBlock *Preheader = preheader(Loop);
    if (Preheader == nullptr || Loop.Header == nullptr ||
        (*Frequencies)->frequency(*Preheader) > (*Frequencies)->frequency(*Loop.Header)) {
      continue;
    }
    std::unordered_set<const MachineInstr *> Hoisted;
    std::size_t HoistedDefinitions = 0;
    bool LocalChange = true;
    while (LocalChange && TotalHoisted < Options.MaximumHoistedInstructions) {
      LocalChange = false;
      for (MachineBasicBlock *Block : Loop.Blocks) {
        if (Block == nullptr)
          continue;
        for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
          MachineInstr *Next = Instruction->next();
          auto Safe = isSafeToHoist(*Instruction, Function, Target, Stage);
          if (!Safe)
            return std::unexpected(Safe.error());
          const std::size_t Definitions = definitionCount(*Instruction);
          const bool Invariant =
              Stage == MachineCodeMotionStage::SelectedSSA
                  ? operandsAreInvariant(*Instruction, Loop, Function.registerInfo(), Hoisted)
                  : postRAOperandsAreInvariant(*Instruction, Loop, Function, Target);
          if (*Safe && Invariant &&
              Definitions <=
                  Options.MaximumHoistedDefinitionsPerLoop -
                      std::min(HoistedDefinitions, Options.MaximumHoistedDefinitionsPerLoop)) {
            auto Moved = moveToPreheader(*Instruction, *Preheader, Target);
            if (!Moved)
              return std::unexpected(Moved.error());
            Hoisted.insert(Instruction);
            HoistedDefinitions += Definitions;
            ++TotalHoisted;
            LocalChange = true;
            Result.Changed = true;
            if (TotalHoisted >= Options.MaximumHoistedInstructions)
              break;
          }
          Instruction = Next;
        }
        if (TotalHoisted >= Options.MaximumHoistedInstructions)
          break;
      }
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
    Result.Preserved.preserve<MachineLoopAnalysis>();
    Result.Preserved.preserve<MachineBranchProbabilityAnalysis>();
    Result.Preserved.preserve<MachineBlockFrequencyAnalysis>();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
