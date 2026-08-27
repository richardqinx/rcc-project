// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCSE.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] const MachinePassContract &cseContract() {
  static const MachinePassContract Contract = [] {
    MachinePassContract Value{
        .Name = "machine-cse",
        .Requires = {MachineFunctionProperty::IsSSA, MachineFunctionProperty::Selected},
        .Sets = {},
        .Clears = {},
        .RequiredAnalyses = {MachineFunctionAnalysisManager::id<MachineDominatorTreeAnalysis>()},
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
         Instruction.cfiType().has_value() || Instruction.deactivationSymbol().has_value();
}

template <typename ValueT>
[[nodiscard]] bool payloadEquals(const MachineOperand &Left, const MachineOperand &Right) noexcept {
  const ValueT *LeftValue = Left.getIf<ValueT>();
  const ValueT *RightValue = Right.getIf<ValueT>();
  return LeftValue != nullptr && RightValue != nullptr && *LeftValue == *RightValue;
}

[[nodiscard]] bool registerOperandEquals(const MachineOperand &Left, const MachineOperand &Right,
                                         bool IgnoreDefinitionValue) noexcept {
  const Register *LeftReg = Left.getIf<Register>();
  const Register *RightReg = Right.getIf<Register>();
  if (LeftReg == nullptr || RightReg == nullptr || Left.subReg() != Right.subReg() ||
      Left.targetFlags() != Right.targetFlags()) {
    return false;
  }
  RegisterOperandFlags LeftFlags = Left.registerFlags();
  RegisterOperandFlags RightFlags = Right.registerFlags();
  for (const RegisterOperandFlag Flag :
       {RegisterOperandFlag::Kill, RegisterOperandFlag::Dead, RegisterOperandFlag::Renamable}) {
    LeftFlags.clear(Flag);
    RightFlags.clear(Flag);
  }
  return LeftFlags == RightFlags && (IgnoreDefinitionValue || *LeftReg == *RightReg);
}

[[nodiscard]] bool operandEquals(const MachineOperand &Left, const MachineOperand &Right,
                                 bool IgnoreDefinitionValue) noexcept {
  if (Left.kind() != Right.kind() || Left.targetFlags() != Right.targetFlags())
    return false;
  switch (Left.kind()) {
  case MachineOperand::Kind::Register:
    return registerOperandEquals(Left, Right, IgnoreDefinitionValue);
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
  case MachineOperand::Kind::LiveOutRegisterMask:
    return false;
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

[[nodiscard]] bool isConstantMemoryBase(MemoryBase::Kind Kind) noexcept {
  return Kind == MemoryBase::Kind::ConstantPool || Kind == MemoryBase::Kind::ConstantPoolPseudo;
}

[[nodiscard]] bool hasSafeInvariantMemory(const MachineInstr &Instruction) noexcept {
  if (Instruction.memOperands().empty())
    return false;
  for (const MachineMemOperand *Memory : Instruction.memOperands()) {
    if (Memory == nullptr || Memory->isStore() || Memory->isAtomic() || Memory->isVolatile())
      return false;
    const MachineMemOperandDesc &Description = Memory->description();
    const bool ExplicitInvariant = Description.Accesses.has(MemoryAccessFlag::Invariant) &&
                                   Description.Accesses.has(MemoryAccessFlag::Dereferenceable);
    if (!ExplicitInvariant && !isConstantMemoryBase(Description.Base.kind()))
      return false;
  }
  return true;
}

[[nodiscard]] Expected<bool> isCandidate(const MachineInstr &Instruction,
                                         const TargetDescription &Target) {
  const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine CSE encountered an instruction without a descriptor");
  }
  const InstructionFlags Flags = Descriptor->Flags;
  if (Flags.has(InstructionFlag::Phi) || Flags.has(InstructionFlag::Pseudo) ||
      Flags.has(InstructionFlag::Meta) || Flags.has(InstructionFlag::Call) ||
      Flags.has(InstructionFlag::Terminator) || Flags.has(InstructionFlag::Branch) ||
      Flags.has(InstructionFlag::Barrier) || Flags.has(InstructionFlag::MayStore) ||
      Flags.has(InstructionFlag::HasSideEffects) ||
      Flags.has(InstructionFlag::UnmodeledSideEffects) || Flags.has(InstructionFlag::Convergent) ||
      hasProtectedAttachment(Instruction)) {
    return false;
  }
  if (Flags.has(InstructionFlag::MayLoad) && !hasSafeInvariantMemory(Instruction))
    return false;
  if (!Flags.has(InstructionFlag::MayLoad) && !Instruction.memOperands().empty())
    return false;
  if (Flags.has(InstructionFlag::MayRaiseFPException) &&
      !Instruction.flags().has(MachineInstrFlag::NoFPExcept)) {
    return false;
  }

  bool HasDefinition = false;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr)
      continue;
    if (Reg->isPhysical() || Reg->isNoRegister() || Operand.isDebugUse() ||
        (Operand.isDef() && Operand.isUse())) {
      return false;
    }
    if (Operand.isDef())
      HasDefinition = true;
  }
  return HasDefinition;
}

[[nodiscard]] bool expressionEquals(const MachineInstr &Left, const MachineInstr &Right) noexcept {
  if (Left.opcode() != Right.opcode() || Left.flags() != Right.flags() ||
      Left.operandCount() != Right.operandCount() ||
      Left.memOperands().size() != Right.memOperands().size()) {
    return false;
  }
  for (std::size_t Index = 0; Index < Left.operandCount(); ++Index) {
    const MachineOperand *LeftOperand = Left.operandAt(Index);
    const MachineOperand *RightOperand = Right.operandAt(Index);
    if (LeftOperand == nullptr || RightOperand == nullptr ||
        LeftOperand->isDef() != RightOperand->isDef() ||
        !operandEquals(*LeftOperand, *RightOperand, LeftOperand->isDef())) {
      return false;
    }
  }
  for (std::size_t Index = 0; Index < Left.memOperands().size(); ++Index) {
    const MachineMemOperand *LeftMemory = Left.memOperands()[Index];
    const MachineMemOperand *RightMemory = Right.memOperands()[Index];
    if (LeftMemory == nullptr || RightMemory == nullptr || *LeftMemory != *RightMemory)
      return false;
  }
  return true;
}

[[nodiscard]] Expected<std::vector<std::pair<Register, Register>>>
replacementPairs(const MachineInstr &Available, const MachineInstr &Redundant,
                 const MachineFunction &Function, const TargetDescription &Target) {
  std::vector<std::pair<Register, Register>> Result;
  std::unordered_set<Register> RedundantDefinitions;
  const MachineRegisterInfo &Registers = Function.registerInfo();
  for (std::size_t Index = 0; Index < Available.operandCount(); ++Index) {
    const MachineOperand *AvailableOperand = Available.operandAt(Index);
    const MachineOperand *RedundantOperand = Redundant.operandAt(Index);
    if (AvailableOperand == nullptr || RedundantOperand == nullptr || !AvailableOperand->isDef()) {
      continue;
    }
    const Register *AvailableReg = AvailableOperand->getIf<Register>();
    const Register *RedundantReg = RedundantOperand->getIf<Register>();
    if (AvailableReg == nullptr || RedundantReg == nullptr || !AvailableReg->isVirtual() ||
        !RedundantReg->isVirtual() || !RedundantDefinitions.insert(*RedundantReg).second) {
      return std::vector<std::pair<Register, Register>>{};
    }
    auto AvailableInfo = Registers.virtualRegisterInfo(*AvailableReg);
    if (!AvailableInfo)
      return std::unexpected(AvailableInfo.error());
    auto RedundantInfo = Registers.virtualRegisterInfo(*RedundantReg);
    if (!RedundantInfo)
      return std::unexpected(RedundantInfo.error());
    const VirtualRegisterInfo &Left = AvailableInfo->get();
    const VirtualRegisterInfo &Right = RedundantInfo->get();
    if (!Left.Type || !Right.Type || Left.Type != Right.Type ||
        Left.TargetFlags != Right.TargetFlags)
      return std::vector<std::pair<Register, Register>>{};
    if (Left.Bank && Right.Bank &&
        !Target.registers().registerBanksOverlap(*Left.Bank, *Right.Bank)) {
      return std::vector<std::pair<Register, Register>>{};
    }
    if (Left.RegClass && Right.RegClass &&
        !Target.registers().registerClassesOverlap(*Left.RegClass, *Right.RegClass)) {
      return std::vector<std::pair<Register, Register>>{};
    }
    Result.emplace_back(*RedundantReg, *AvailableReg);
  }
  return Result;
}

[[nodiscard]] Expected<void>
replaceAndErase(MachineInstr &Redundant,
                const std::vector<std::pair<Register, Register>> &Replacements,
                MachineRegisterInfo &Registers) {
  MachineBasicBlock *Block = Redundant.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "machine CSE candidate is detached");
  for (const auto &[OldReg, NewReg] : Replacements) {
    auto Replaced = Registers.replaceRegWith(OldReg, NewReg);
    if (!Replaced)
      return std::unexpected(Replaced.error());
  }
  auto Removed = Block->removeInstruction(Redundant);
  if (!Removed)
    return std::unexpected(Removed.error());
  for (const auto &[OldReg, NewReg] : Replacements) {
    (void)NewReg;
    if (!Registers.useEmpty(OldReg, true) || Registers.getUniqueVRegDef(OldReg) != nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "machine CSE left references to a redundant result");
    }
    auto Erased = Registers.eraseVirtualRegister(OldReg);
    if (!Erased)
      return std::unexpected(Erased.error());
  }
  return {};
}

} // namespace

const MachinePassContract &MachineCSEPass::contract() const noexcept {
  return cseContract();
}

Expected<MachinePassResult> MachineCSEPass::run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) {
  return runImplementation(Function, Target, Analyses);
}

Expected<MachinePassResult>
MachineCSEPass::runImplementation(MachineFunction &Function, const TargetDescription &Target,
                                  MachineFunctionAnalysisManager &Analyses) {
  auto Dominators = Analyses.getResult<MachineDominatorTreeAnalysis>(Function, Target);
  if (!Dominators)
    return std::unexpected(Dominators.error());

  MachinePassResult Result;
  MachineRegisterInfo &Registers = Function.registerInfo();
  std::unordered_map<Opcode, std::vector<MachineInstr *>> Available;
  std::size_t Comparisons = 0;
  bool BudgetExhausted = false;

  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr && !BudgetExhausted;
       Block = Block->next()) {
    for (MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;) {
      MachineInstr *Next = Instruction->next();
      auto Candidate = isCandidate(*Instruction, Target);
      if (!Candidate)
        return std::unexpected(Candidate.error());
      if (!*Candidate) {
        Instruction = Next;
        continue;
      }

      MachineInstr *Match = nullptr;
      auto &OpcodeCandidates = Available[Instruction->opcode()];
      for (MachineInstr *Prior : OpcodeCandidates) {
        if (Comparisons >= Options.MaximumComparisons) {
          BudgetExhausted = true;
          break;
        }
        ++Comparisons;
        if (!(*Dominators)->dominates(*Prior, *Instruction) ||
            !expressionEquals(*Prior, *Instruction)) {
          continue;
        }
        Match = Prior;
        break;
      }
      if (BudgetExhausted)
        break;
      if (Match == nullptr) {
        OpcodeCandidates.push_back(Instruction);
        Instruction = Next;
        continue;
      }

      auto Replacements = replacementPairs(*Match, *Instruction, Function, Target);
      if (!Replacements)
        return std::unexpected(Replacements.error());
      if (Replacements->empty()) {
        OpcodeCandidates.push_back(Instruction);
        Instruction = Next;
        continue;
      }
      auto Replaced = replaceAndErase(*Instruction, *Replacements, Registers);
      if (!Replaced)
        return std::unexpected(Replaced.error());
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
    Result.Changes = {MachinePassChange::Instructions, MachinePassChange::Operands,
                      MachinePassChange::Registers};
    Result.Preserved.preserve<MachineDominatorTreeAnalysis>();
  } else {
    Result.Preserved = PreservedAnalyses::all();
  }
  return Result;
}

} // namespace rcc
