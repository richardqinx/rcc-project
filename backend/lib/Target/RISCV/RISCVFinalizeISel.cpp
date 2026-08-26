// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

struct SelectOperands final {
  Register Destination;
  Register Left;
  Register Right;
  std::int64_t Condition = 0;
  Register TrueValue;
  Register FalseValue;
};

struct SelectSequence final {
  std::vector<MachineInstr *> Selects;
  std::vector<MachineInstr *> DebugValues;
  MachineInstr *LastSelect = nullptr;
};

[[nodiscard]] bool isSelectPseudo(Opcode Value) noexcept {
  return Value == instructionOpcode(InstructionOpcode::Select_GPR_Using_CC_GPR) ||
         Value == instructionOpcode(InstructionOpcode::Select_FPR32_Using_CC_GPR) ||
         Value == instructionOpcode(InstructionOpcode::Select_FPR64_Using_CC_GPR);
}

[[nodiscard]] Expected<SelectOperands> selectOperands(const MachineInstr &Instruction) {
  if (!isSelectPseudo(Instruction.opcode()) || Instruction.operandCount() < 6U) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V custom select has an invalid opcode or operand count");
  }

  const auto RegisterAt = [&Instruction](std::size_t Index) -> std::optional<Register> {
    const MachineOperand *Operand = Instruction.operandAt(Index);
    if (Operand == nullptr)
      return std::nullopt;
    return Operand->registerValue();
  };
  const auto Destination = RegisterAt(0);
  const auto Left = RegisterAt(1);
  const auto Right = RegisterAt(2);
  const MachineOperand *ConditionOperand = Instruction.operandAt(3);
  const auto *Condition =
      ConditionOperand == nullptr ? nullptr : ConditionOperand->getIf<std::int64_t>();
  const auto TrueValue = RegisterAt(4);
  const auto FalseValue = RegisterAt(5);
  if (!Destination || !Left || !Right || Condition == nullptr || !TrueValue || !FalseValue) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V custom select has malformed operands");
  }
  if (*Condition < static_cast<std::int64_t>(BranchConditionCode::Equal) ||
      *Condition > static_cast<std::int64_t>(BranchConditionCode::UnsignedGreaterEqual)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V custom select has an invalid condition code");
  }
  return SelectOperands{*Destination, *Left, *Right, *Condition, *TrueValue, *FalseValue};
}

[[nodiscard]] bool containsRegister(const std::vector<Register> &Registers,
                                    Register Value) noexcept {
  for (const Register Candidate : Registers)
    if (Candidate == Value)
      return true;
  return false;
}

[[nodiscard]] bool instructionUsesAny(const MachineInstr &Instruction,
                                      const std::vector<Register> &Registers) noexcept {
  for (const MachineOperand &Operand : Instruction.operands()) {
    const std::optional<Register> Reg = Operand.registerValue();
    if (Reg && Operand.isUse() && containsRegister(Registers, *Reg))
      return true;
  }
  return false;
}

void collectAdjacentDebugValues(MachineInstr &Select, Register Destination,
                                std::vector<MachineInstr *> &Result) {
  for (MachineInstr *Instruction = Select.next();
       Instruction != nullptr && Instruction->isDebugInstr(); Instruction = Instruction->next()) {
    const std::vector<Register> DestinationList{Destination};
    if (instructionUsesAny(*Instruction, DestinationList))
      Result.push_back(Instruction);
  }
}

[[nodiscard]] bool stopsSelectSequence(const MachineInstr &Instruction,
                                       const InstructionDescriptor &Descriptor,
                                       const std::vector<Register> &SelectDestinations) noexcept {
  const InstructionFlags Flags = Descriptor.Flags;
  if (Flags.has(InstructionFlag::UnmodeledSideEffects) ||
      Flags.has(InstructionFlag::HasSideEffects) || Flags.has(InstructionFlag::MayLoad) ||
      Flags.has(InstructionFlag::MayStore) || Flags.has(InstructionFlag::UsesCustomInserter) ||
      !Instruction.memOperands().empty() || Instruction.flags().has(MachineInstrFlag::FrameSetup) ||
      Instruction.flags().has(MachineInstrFlag::FrameDestroy) ||
      Instruction.opcode() == opcode(GenericOpcode::INLINEASM) ||
      Instruction.opcode() == opcode(GenericOpcode::INLINEASM_BR)) {
    return true;
  }
  return instructionUsesAny(Instruction, SelectDestinations);
}

[[nodiscard]] Expected<SelectSequence> collectSelectSequence(MachineInstr &First,
                                                             const TargetDescription &Target) {
  auto FirstOperands = selectOperands(First);
  if (!FirstOperands)
    return std::unexpected(FirstOperands.error());

  SelectSequence Result;
  std::vector<Register> SelectDestinations{FirstOperands->Destination};
  for (MachineInstr *Instruction = &First; Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->isDebugInstr())
      continue;
    if (isSelectPseudo(Instruction->opcode())) {
      auto Operands = selectOperands(*Instruction);
      if (!Operands)
        return std::unexpected(Operands.error());
      if (Operands->Left != FirstOperands->Left || Operands->Right != FirstOperands->Right ||
          Operands->Condition != FirstOperands->Condition ||
          containsRegister(SelectDestinations, Operands->TrueValue) ||
          containsRegister(SelectDestinations, Operands->FalseValue)) {
        break;
      }
      Result.Selects.push_back(Instruction);
      Result.LastSelect = Instruction;
      collectAdjacentDebugValues(*Instruction, Operands->Destination, Result.DebugValues);
      if (!containsRegister(SelectDestinations, Operands->Destination))
        SelectDestinations.push_back(Operands->Destination);
      continue;
    }

    const InstructionDescriptor *Descriptor = Target.instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "RISC-V select expansion encountered an unknown opcode");
    }
    if (stopsSelectSequence(*Instruction, *Descriptor, SelectDestinations))
      break;
  }
  if (Result.Selects.empty() || Result.LastSelect == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V select sequence did not contain its anchor instruction");
  }
  return Result;
}

[[nodiscard]] Expected<Opcode> conditionalBranchOpcode(std::int64_t Condition) {
  switch (static_cast<BranchConditionCode>(Condition)) {
  case BranchConditionCode::Equal:
    return instructionOpcode(InstructionOpcode::BEQ);
  case BranchConditionCode::NotEqual:
    return instructionOpcode(InstructionOpcode::BNE);
  case BranchConditionCode::SignedLessThan:
    return instructionOpcode(InstructionOpcode::BLT);
  case BranchConditionCode::SignedGreaterEqual:
    return instructionOpcode(InstructionOpcode::BGE);
  case BranchConditionCode::UnsignedLessThan:
    return instructionOpcode(InstructionOpcode::BLTU);
  case BranchConditionCode::UnsignedGreaterEqual:
    return instructionOpcode(InstructionOpcode::BGEU);
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "RISC-V select has an invalid condition code");
}

[[nodiscard]] Expected<MachineInstr *>
buildConditionalBranch(MachineFunction &Function, const TargetDescription &Target,
                       MachineBasicBlock &Block, MachineBasicBlock &Destination,
                       const SelectOperands &Operands, const MachineInstr &Source) {
  auto BranchOpcode = conditionalBranchOpcode(Operands.Condition);
  if (!BranchOpcode)
    return std::unexpected(BranchOpcode.error());
  MachineIRBuilder Builder(Function, Target);
  if (auto Point = Builder.setInsertPoint(Block); !Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(*BranchOpcode);
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addUse(Operands.Left); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Operands.Right); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addBasicBlock(Destination); !Added)
    return std::unexpected(Added.error());
  if (Source.debugLocation()) {
    if (auto Location = Pending->setDebugLocation(*Source.debugLocation()); !Location)
      return std::unexpected(Location.error());
  }
  return Pending->build();
}

[[nodiscard]] Expected<MachineInstr *>
buildPhi(MachineFunction &Function, const TargetDescription &Target, MachineBasicBlock &Tail,
         MachineInstr *InsertBefore, const SelectOperands &Operands, MachineBasicBlock &Head,
         MachineBasicBlock &IfFalse, const MachineInstr &Source) {
  MachineIRBuilder Builder(Function, Target);
  Expected<void> Point = InsertBefore == nullptr ? Builder.setInsertPoint(Tail)
                                                 : Builder.setInsertPointBefore(*InsertBefore);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::PHI));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(Operands.Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Operands.TrueValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addBasicBlock(Head); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Operands.FalseValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addBasicBlock(IfFalse); !Added)
    return std::unexpected(Added.error());
  if (Source.debugLocation()) {
    if (auto Location = Pending->setDebugLocation(*Source.debugLocation()); !Location)
      return std::unexpected(Location.error());
  }
  return Pending->build();
}

[[nodiscard]] Expected<void> insertBlocksAfter(MachineFunction &Function, MachineBasicBlock &Head,
                                               MachineBasicBlock &First,
                                               MachineBasicBlock &Second) {
  MachineBasicBlock *Following = Head.next();
  if (Following == nullptr) {
    if (auto Inserted = Function.appendBlock(First); !Inserted)
      return std::unexpected(Inserted.error());
    return Function.appendBlock(Second);
  }
  if (auto Inserted = Function.insertBlockBefore(*Following, First); !Inserted)
    return std::unexpected(Inserted.error());
  return Function.insertBlockBefore(*Following, Second);
}

[[nodiscard]] Expected<void> moveInstructionToEnd(MachineInstr &Instruction,
                                                  MachineBasicBlock &Destination) {
  MachineBasicBlock *Source = Instruction.parent();
  if (Source == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "moved instruction is detached");
  if (auto Removed = Source->removeInstruction(Instruction); !Removed)
    return std::unexpected(Removed.error());
  return Destination.appendInstruction(Instruction);
}

[[nodiscard]] Expected<MachineBasicBlock *> emitSelectSequence(MachineInstr &First,
                                                               MachineBasicBlock &Head,
                                                               MachineFunction &Function,
                                                               const TargetDescription &Target) {
  auto Sequence = collectSelectSequence(First, Target);
  if (!Sequence)
    return std::unexpected(Sequence.error());
  auto FirstOperands = selectOperands(First);
  if (!FirstOperands)
    return std::unexpected(FirstOperands.error());

  auto TailResult = Function.createBasicBlock();
  if (!TailResult)
    return std::unexpected(TailResult.error());
  auto IfFalseResult = Function.createBasicBlock();
  if (!IfFalseResult)
    return std::unexpected(IfFalseResult.error());
  MachineBasicBlock *Tail = *TailResult;
  MachineBasicBlock *IfFalse = *IfFalseResult;
  if (auto Inserted = insertBlocksAfter(Function, Head, *IfFalse, *Tail); !Inserted)
    return std::unexpected(Inserted.error());
  const std::uint32_t CallFrameSize = Head.attributes().callFrameSize();
  IfFalse->attributes().setCallFrameSize(CallFrameSize);
  Tail->attributes().setCallFrameSize(CallFrameSize);

  for (MachineInstr *DebugValue : Sequence->DebugValues) {
    if (auto Moved = moveInstructionToEnd(*DebugValue, *Tail); !Moved)
      return std::unexpected(Moved.error());
  }
  for (MachineInstr *Instruction = Sequence->LastSelect->next(); Instruction != nullptr;) {
    MachineInstr *Next = Instruction->next();
    if (auto Moved = moveInstructionToEnd(*Instruction, *Tail); !Moved)
      return std::unexpected(Moved.error());
    Instruction = Next;
  }

  MachineCFGUpdater CFG = Function.cfgUpdater();
  if (auto Transferred = CFG.transferSuccessors(Head, *Tail); !Transferred)
    return std::unexpected(Transferred.error());
  auto Half = BranchProbability::fromFraction(1U, 2U);
  if (!Half)
    return std::unexpected(Half.error());
  if (auto Added = CFG.addSuccessor(Head, *IfFalse, *Half); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CFG.addSuccessor(Head, *Tail, *Half); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CFG.addSuccessor(*IfFalse, *Tail, BranchProbability::one()); !Added)
    return std::unexpected(Added.error());

  auto Branch = buildConditionalBranch(Function, Target, Head, *Tail, *FirstOperands, First);
  if (!Branch)
    return std::unexpected(Branch.error());

  MachineInstr *PhiInsertBefore = Tail->firstInstruction();
  for (MachineInstr *Select : Sequence->Selects) {
    auto Operands = selectOperands(*Select);
    if (!Operands)
      return std::unexpected(Operands.error());
    auto Phi =
        buildPhi(Function, Target, *Tail, PhiInsertBefore, *Operands, Head, *IfFalse, *Select);
    if (!Phi)
      return std::unexpected(Phi.error());
  }
  for (MachineInstr *Select : Sequence->Selects) {
    MachineBasicBlock *Parent = Select->parent();
    if (Parent == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V custom select became detached during expansion");
    if (auto Removed = Parent->removeInstruction(*Select); !Removed)
      return std::unexpected(Removed.error());
  }
  Function.properties().clear(MachineFunctionProperty::NoPHIs);
  return Tail;
}

[[nodiscard]] MachineInstr *nextNonDebug(MachineInstr &Instruction) noexcept {
  MachineInstr *Next = Instruction.next();
  while (Next != nullptr && Next->isDebugInstr())
    Next = Next->next();
  return Next;
}

[[nodiscard]] bool isCascadedFloatingSelect(MachineInstr &First, MachineInstr *Second) noexcept {
  if (First.opcode() == instructionOpcode(InstructionOpcode::Select_GPR_Using_CC_GPR) ||
      Second == nullptr || Second->opcode() != First.opcode()) {
    return false;
  }
  const MachineOperand *DestinationOperand = First.operandAt(0);
  const MachineOperand *FalseOperand = Second->operandAt(5);
  const auto Destination =
      DestinationOperand == nullptr ? std::nullopt : DestinationOperand->registerValue();
  const auto FalseValue = FalseOperand == nullptr ? std::nullopt : FalseOperand->registerValue();
  return Destination && FalseValue && *Destination == *FalseValue &&
         FalseOperand->registerFlags().has(RegisterOperandFlag::Kill);
}

[[nodiscard]] Expected<MachineInstr *>
buildCascadedPhi(MachineFunction &Function, const TargetDescription &Target,
                 MachineBasicBlock &Sink, const SelectOperands &FirstOperands,
                 const SelectOperands &SecondOperands, MachineBasicBlock &Head,
                 MachineBasicBlock &FirstPath, MachineBasicBlock &SecondPath,
                 const MachineInstr &Source) {
  MachineIRBuilder Builder(Function, Target);
  Expected<void> Point = Sink.firstInstruction() == nullptr
                             ? Builder.setInsertPoint(Sink)
                             : Builder.setInsertPointBefore(*Sink.firstInstruction());
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::PHI));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(SecondOperands.Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(SecondOperands.TrueValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addBasicBlock(Head); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(FirstOperands.TrueValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addBasicBlock(FirstPath); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(FirstOperands.FalseValue); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addBasicBlock(SecondPath); !Added)
    return std::unexpected(Added.error());
  if (Source.debugLocation()) {
    if (auto Location = Pending->setDebugLocation(*Source.debugLocation()); !Location)
      return std::unexpected(Location.error());
  }
  return Pending->build();
}

[[nodiscard]] Expected<MachineBasicBlock *>
emitCascadedSelect(MachineInstr &First, MachineInstr &Second, MachineBasicBlock &Head,
                   MachineFunction &Function, const TargetDescription &Target) {
  auto FirstOperands = selectOperands(First);
  if (!FirstOperands)
    return std::unexpected(FirstOperands.error());
  auto SecondOperands = selectOperands(Second);
  if (!SecondOperands)
    return std::unexpected(SecondOperands.error());

  auto FirstPathResult = Function.createBasicBlock();
  if (!FirstPathResult)
    return std::unexpected(FirstPathResult.error());
  auto SecondPathResult = Function.createBasicBlock();
  if (!SecondPathResult)
    return std::unexpected(SecondPathResult.error());
  auto SinkResult = Function.createBasicBlock();
  if (!SinkResult)
    return std::unexpected(SinkResult.error());
  MachineBasicBlock *FirstPath = *FirstPathResult;
  MachineBasicBlock *SecondPath = *SecondPathResult;
  MachineBasicBlock *Sink = *SinkResult;
  MachineBasicBlock *Following = Head.next();
  const auto Insert = [&](MachineBasicBlock &Block) -> Expected<void> {
    return Following == nullptr ? Function.appendBlock(Block)
                                : Function.insertBlockBefore(*Following, Block);
  };
  if (auto Inserted = Insert(*FirstPath); !Inserted)
    return std::unexpected(Inserted.error());
  if (auto Inserted = Insert(*SecondPath); !Inserted)
    return std::unexpected(Inserted.error());
  if (auto Inserted = Insert(*Sink); !Inserted)
    return std::unexpected(Inserted.error());
  const std::uint32_t CallFrameSize = Head.attributes().callFrameSize();
  FirstPath->attributes().setCallFrameSize(CallFrameSize);
  SecondPath->attributes().setCallFrameSize(CallFrameSize);
  Sink->attributes().setCallFrameSize(CallFrameSize);

  for (MachineInstr *Instruction = First.next(); Instruction != nullptr;) {
    MachineInstr *Next = Instruction->next();
    if (auto Moved = moveInstructionToEnd(*Instruction, *Sink); !Moved)
      return std::unexpected(Moved.error());
    Instruction = Next;
  }

  MachineCFGUpdater CFG = Function.cfgUpdater();
  if (auto Transferred = CFG.transferSuccessors(Head, *Sink); !Transferred)
    return std::unexpected(Transferred.error());
  auto Half = BranchProbability::fromFraction(1U, 2U);
  if (!Half)
    return std::unexpected(Half.error());
  if (auto Added = CFG.addSuccessor(Head, *FirstPath, *Half); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CFG.addSuccessor(Head, *Sink, *Half); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CFG.addSuccessor(*FirstPath, *SecondPath, *Half); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CFG.addSuccessor(*FirstPath, *Sink, *Half); !Added)
    return std::unexpected(Added.error());
  if (auto Added = CFG.addSuccessor(*SecondPath, *Sink, BranchProbability::one()); !Added)
    return std::unexpected(Added.error());

  auto FirstBranch =
      buildConditionalBranch(Function, Target, *FirstPath, *Sink, *FirstOperands, First);
  if (!FirstBranch)
    return std::unexpected(FirstBranch.error());
  auto SecondBranch = buildConditionalBranch(Function, Target, Head, *Sink, *SecondOperands, First);
  if (!SecondBranch)
    return std::unexpected(SecondBranch.error());
  auto Phi = buildCascadedPhi(Function, Target, *Sink, *FirstOperands, *SecondOperands, Head,
                              *FirstPath, *SecondPath, First);
  if (!Phi)
    return std::unexpected(Phi.error());

  if (auto Removed = Head.removeInstruction(First); !Removed)
    return std::unexpected(Removed.error());
  MachineBasicBlock *SecondParent = Second.parent();
  if (SecondParent == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "cascaded select became detached");
  if (auto Removed = SecondParent->removeInstruction(Second); !Removed)
    return std::unexpected(Removed.error());
  Function.properties().clear(MachineFunctionProperty::NoPHIs);
  return Sink;
}

class RISCVFinalizeISelInfo final : public TargetFinalizeISelInfo {
public:
  [[nodiscard]] Expected<MachineBasicBlock *>
  emitInstrWithCustomInserter(MachineInstr &Instruction, MachineBasicBlock &Block,
                              MachineFunction &Function,
                              const TargetDescription &Target) const override {
    if (!isSelectPseudo(Instruction.opcode())) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V has no custom inserter for the requested opcode");
    }
    MachineInstr *Second = nextNonDebug(Instruction);
    if (isCascadedFloatingSelect(Instruction, Second))
      return emitCascadedSelect(Instruction, *Second, Block, Function, Target);
    return emitSelectSequence(Instruction, Block, Function, Target);
  }
};

} // namespace

const TargetFinalizeISelInfo &finalizeISelInfo() noexcept {
  static const RISCVFinalizeISelInfo Info;
  return Info;
}

} // namespace rcc::riscv
