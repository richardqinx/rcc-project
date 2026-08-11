// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/PrologueEpilogueInsertion.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetFrameLowering.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/Target/RISCV/RISCVCallLowering.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Block = nullptr;
};

class ObservingFrameLowering final : public rcc::TargetFrameLowering {
public:
  explicit ObservingFrameLowering(const rcc::TargetFrameLowering &Delegate,
                                  bool SkipFrameIndexElimination = false) noexcept
      : Delegate(&Delegate), SkipFrameIndexElimination(SkipFrameIndexElimination) {}

  [[nodiscard]] rcc::Expected<void>
  prepareFrame(rcc::MachineFunction &Function,
               const rcc::TargetDescription &Target) const override {
    PrepareSawUnfrozen = !Function.frameInfo().isFrozen();
    return Delegate->prepareFrame(Function, Target);
  }

  [[nodiscard]] rcc::Expected<void>
  insertCalleeSavedSpillsAndRestores(rcc::MachineFunction &Function,
                                     const rcc::TargetDescription &Target) const override {
    InsertSawUnfrozen = !Function.frameInfo().isFrozen();
    return Delegate->insertCalleeSavedSpillsAndRestores(Function, Target);
  }

  [[nodiscard]] rcc::Expected<void>
  processFunctionBeforeFrameFinalized(rcc::MachineFunction &Function,
                                      const rcc::TargetDescription &Target) const override {
    return Delegate->processFunctionBeforeFrameFinalized(Function, Target);
  }

  [[nodiscard]] rcc::Expected<void>
  finalizeFrame(rcc::MachineFunction &Function,
                const rcc::TargetDescription &Target) const override {
    FinalizeSawUnfrozen = !Function.frameInfo().isFrozen();
    bool HasCalleeSave = false;
    for (const rcc::MachineInstr *Instruction : Function.allInstructions()) {
      if (Instruction == nullptr || Instruction->parent() == nullptr)
        continue;
      for (const rcc::MachineOperand &Operand : Instruction->operands()) {
        const auto *Frame = Operand.getIf<rcc::FrameIndexOperand>();
        const rcc::StackObject *Object =
            Frame == nullptr ? nullptr : Function.frameInfo().object(Frame->Index);
        HasCalleeSave |=
            Object != nullptr && Object->description()->Kind == rcc::StackObjectKind::CalleeSaved;
      }
    }
    FinalizeSawCalleeSave = HasCalleeSave;
    auto Result = Delegate->finalizeFrame(Function, Target);
    FinalizeProducedFrozen = Result && Function.frameInfo().isFrozen();
    return Result;
  }

  [[nodiscard]] bool
  isCallFramePseudo(const rcc::MachineInstr &Instruction) const noexcept override {
    return Delegate->isCallFramePseudo(Instruction);
  }

  [[nodiscard]] rcc::Expected<void>
  eliminateCallFramePseudo(rcc::MachineInstr &Instruction, rcc::MachineFunction &Function,
                           const rcc::TargetDescription &Target,
                           rcc::RegisterScavenger &Scavenger) const override {
    return Delegate->eliminateCallFramePseudo(Instruction, Function, Target, Scavenger);
  }

  [[nodiscard]] rcc::Expected<void> emitPrologue(rcc::MachineFunction &Function,
                                                 const rcc::TargetDescription &Target,
                                                 rcc::RegisterScavenger &Scavenger) const override {
    PrologueSawFrozen = Function.frameInfo().isFrozen();
    return Delegate->emitPrologue(Function, Target, Scavenger);
  }

  [[nodiscard]] bool
  isEpilogueTerminator(const rcc::MachineInstr &Instruction,
                       const rcc::TargetDescription &Target) const noexcept override {
    return Delegate->isEpilogueTerminator(Instruction, Target);
  }

  [[nodiscard]] rcc::Expected<void> emitEpilogue(rcc::MachineBasicBlock &Block,
                                                 rcc::MachineInstr &Terminator,
                                                 rcc::MachineFunction &Function,
                                                 const rcc::TargetDescription &Target,
                                                 rcc::RegisterScavenger &Scavenger) const override {
    EpilogueSawFrozen = Function.frameInfo().isFrozen();
    return Delegate->emitEpilogue(Block, Terminator, Function, Target, Scavenger);
  }

  [[nodiscard]] rcc::Expected<void>
  processFunctionBeforeFrameIndexElimination(rcc::MachineFunction &Function,
                                             const rcc::TargetDescription &Target) const override {
    return Delegate->processFunctionBeforeFrameIndexElimination(Function, Target);
  }

  [[nodiscard]] rcc::Expected<rcc::FrameIndexReference>
  getFrameIndexReference(const rcc::MachineFunction &Function, const rcc::MachineInstr &Instruction,
                         rcc::FrameIndex Index, std::int64_t OperandOffset) const override {
    return Delegate->getFrameIndexReference(Function, Instruction, Index, OperandOffset);
  }

  [[nodiscard]] rcc::Expected<void>
  eliminateFrameIndex(rcc::MachineInstr &Instruction, std::size_t OperandIndex,
                      rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                      rcc::RegisterScavenger &Scavenger) const override {
    if (SkipFrameIndexElimination)
      return {};
    return Delegate->eliminateFrameIndex(Instruction, OperandIndex, Function, Target, Scavenger);
  }

  mutable bool PrepareSawUnfrozen = false;
  mutable bool InsertSawUnfrozen = false;
  mutable bool FinalizeSawUnfrozen = false;
  mutable bool FinalizeSawCalleeSave = false;
  mutable bool FinalizeProducedFrozen = false;
  mutable bool PrologueSawFrozen = false;
  mutable bool EpilogueSawFrozen = false;

private:
  const rcc::TargetFrameLowering *Delegate;
  bool SkipFrameIndexElimination = false;
};

[[nodiscard]] constexpr rcc::Opcode opcode(rcc::riscv::InstructionOpcode Value) noexcept {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] rcc::PhysicalRegister physReg(rcc::riscv::PhysicalRegisterID ID) {
  return *rcc::riscv::physicalRegister(ID);
}

[[nodiscard]] rcc::Register physical(rcc::riscv::PhysicalRegisterID ID) {
  return rcc::Register::fromPhysical(physReg(ID));
}

[[nodiscard]] rcc::Expected<FunctionFixture> createFunction(std::string Name,
                                                            std::uint64_t StableID) {
  auto Function = rcc::MachineFunction::create(std::move(Name),
                                               rcc::MachineFunctionStableID::fromValue(StableID));
  if (!Function)
    return std::unexpected(Function.error());
  auto Block = (*Function)->createBasicBlock();
  if (!Block)
    return std::unexpected(Block.error());
  if (auto Result = (*Function)->appendBlock(**Block); !Result)
    return std::unexpected(Result.error());
  return FunctionFixture{std::move(*Function), *Block};
}

void setFramePassProperties(rcc::MachineFunction &Function) {
  for (const rcc::MachineFunctionProperty Property : {
           rcc::MachineFunctionProperty::Legalized,
           rcc::MachineFunctionProperty::RegBankSelected,
           rcc::MachineFunctionProperty::Selected,
           rcc::MachineFunctionProperty::NoPHIs,
           rcc::MachineFunctionProperty::TiedOpsRewritten,
           rcc::MachineFunctionProperty::NoVRegs,
       }) {
    Function.properties().set(Property);
  }
}

[[nodiscard]] rcc::Expected<void>
runFrameLoweringWith(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                     const rcc::TargetFrameLowering &FrameLowering) {
  setFramePassProperties(Function);
  rcc::MachinePassManager Pipeline;
  auto Added =
      Pipeline.addPass(std::make_unique<rcc::PrologueEpilogueInsertionPass>(FrameLowering));
  if (!Added)
    return std::unexpected(Added.error());
  rcc::MachineAnalysisManager Analyses;
  return Pipeline.run(Function, Target, Analyses);
}

[[nodiscard]] rcc::Expected<void> runFrameLowering(rcc::MachineFunction &Function,
                                                   const rcc::TargetDescription &Target) {
  return runFrameLoweringWith(Function, Target, rcc::riscv::frameLowering());
}

[[nodiscard]] rcc::Expected<rcc::FrameIndex>
createStackObject(rcc::MachineFunction &Function, rcc::StackObjectKind Kind,
                  std::optional<std::uint64_t> Size, std::uint64_t Alignment,
                  std::optional<std::int64_t> Offset = std::nullopt) {
  auto Align = rcc::Alignment::create(Alignment);
  if (!Align)
    return std::unexpected(Align.error());
  rcc::StackObjectDesc Description;
  Description.Kind = Kind;
  Description.Size = Size;
  Description.Align = *Align;
  Description.Offset = Offset;
  return Function.frameInfo().createObject(std::move(Description));
}

[[nodiscard]] rcc::Expected<const rcc::MachineMemOperand *>
createStackMemory(rcc::MachineFunction &Function, rcc::FrameIndex Index,
                  rcc::MemoryAccessFlag Access, std::int64_t Offset) {
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  if (!Size || !Align)
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "invalid test memory facts");
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = rcc::MemoryBase::frameIndex(Index);
  Description.Offset = Offset;
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildStackAccess(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                 rcc::MachineBasicBlock &Block, rcc::riscv::InstructionOpcode OpcodeValue,
                 rcc::Register Value, rcc::FrameIndex Index, std::int64_t Offset) {
  const bool IsLoad = OpcodeValue == rcc::riscv::InstructionOpcode::LD ||
                      OpcodeValue == rcc::riscv::InstructionOpcode::FLD;
  auto Memory = createStackMemory(
      Function, Index, IsLoad ? rcc::MemoryAccessFlag::Load : rcc::MemoryAccessFlag::Store, Offset);
  if (!Memory)
    return std::unexpected(Memory.error());
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(OpcodeValue));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto RegisterResult = IsLoad ? Pending->addDef(Value) : Pending->addUse(Value);
  if (!RegisterResult)
    return std::unexpected(RegisterResult.error());
  if (auto Result = Pending->addFrameIndex(Index); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(Offset); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addMemOperand(**Memory); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildAddImmediate(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                  rcc::MachineBasicBlock &Block, rcc::Register Destination, rcc::Register Source,
                  std::int64_t Immediate) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ADDI));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Source); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(Immediate); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *> buildAdd(rcc::MachineFunction &Function,
                                                          const rcc::TargetDescription &Target,
                                                          rcc::MachineBasicBlock &Block,
                                                          rcc::Register Destination,
                                                          rcc::Register Left, rcc::Register Right) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ADD));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Left); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Right); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildMoveToDouble(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                  rcc::MachineBasicBlock &Block, rcc::Register Destination, rcc::Register Source) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::FMV_D_X));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Source); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildCallFramePseudo(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                     rcc::MachineBasicBlock &Block, rcc::riscv::InstructionOpcode OpcodeValue,
                     std::int64_t Amount, std::int64_t PreparedBytes = 0) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(OpcodeValue));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addImmediate(Amount); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(PreparedBytes); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildRegisterMaskClobber(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                         rcc::MachineBasicBlock &Block, rcc::PhysicalRegister Clobbered) {
  std::uint64_t Preserved = ~std::uint64_t{1};
  Preserved &= ~(std::uint64_t{1} << Clobbered.number());
  auto Mask = std::make_shared<const rcc::RegisterMask>(std::vector<std::uint64_t>{Preserved});
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::INLINEASM));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addRegisterMask(std::move(Mask)); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildIndirectTail(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                  rcc::MachineBasicBlock &Block, rcc::Register Callee) {
  auto Mask =
      std::make_shared<const rcc::RegisterMask>(std::vector<std::uint64_t>{~std::uint64_t{1}});
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoTAIL));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addUse(Callee); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(1); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addRegisterMask(std::move(Mask)); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *> buildReturn(rcc::MachineFunction &Function,
                                                             const rcc::TargetDescription &Target,
                                                             rcc::MachineBasicBlock &Block) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoRET));
  if (!Pending)
    return std::unexpected(Pending.error());
  return Pending->build();
}

[[nodiscard]] std::vector<rcc::MachineInstr *> instructions(rcc::MachineBasicBlock &Block) {
  std::vector<rcc::MachineInstr *> Result;
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    Result.push_back(Instruction);
  }
  return Result;
}

[[nodiscard]] std::size_t countOpcode(const rcc::MachineFunction &Function,
                                      rcc::Opcode OpcodeValue) {
  std::size_t Result = 0;
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Result += Instruction->opcode() == OpcodeValue ? 1U : 0U;
    }
  }
  return Result;
}

[[nodiscard]] std::size_t countOpcode(const rcc::MachineFunction &Function,
                                      rcc::riscv::InstructionOpcode OpcodeValue) {
  return countOpcode(Function, opcode(OpcodeValue));
}

[[nodiscard]] std::optional<rcc::PhysicalRegister> registerAt(const rcc::MachineInstr &Instruction,
                                                              std::size_t Index) {
  const rcc::MachineOperand *Operand = Instruction.operandAt(Index);
  if (Operand == nullptr)
    return std::nullopt;
  const rcc::Register *Reg = Operand->getIf<rcc::Register>();
  if (Reg == nullptr || !Reg->isPhysical())
    return std::nullopt;
  return Reg->physicalRegister();
}

[[nodiscard]] std::optional<std::int64_t> immediateAt(const rcc::MachineInstr &Instruction,
                                                      std::size_t Index) {
  const rcc::MachineOperand *Operand = Instruction.operandAt(Index);
  if (Operand == nullptr)
    return std::nullopt;
  const auto *Immediate = Operand->getIf<std::int64_t>();
  return Immediate == nullptr ? std::nullopt : std::optional<std::int64_t>(*Immediate);
}

[[nodiscard]] bool hasFrameIndex(const rcc::MachineFunction &Function) {
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      for (const rcc::MachineOperand &Operand : Instruction->operands()) {
        if (Operand.kind() == rcc::MachineOperand::Kind::FrameIndex)
          return true;
      }
      for (const rcc::MachineMemOperand *Memory : Instruction->memOperands()) {
        if (Memory->description().Base.frameIndex())
          return true;
      }
    }
  }
  return false;
}

[[nodiscard]] const rcc::StackObject *findStackObject(const rcc::MachineFunction &Function,
                                                      rcc::StackObjectKind Kind) {
  const auto Position =
      std::ranges::find_if(Function.frameInfo().objects(), [Kind](const rcc::StackObject &Object) {
        return !Object.isTombstone() && !Object.description()->IsDead &&
               Object.description()->Kind == Kind;
      });
  return Position == Function.frameInfo().objects().end() ? nullptr : &*Position;
}

[[nodiscard]] const rcc::StackObject *findCalleeSave(const rcc::MachineFunction &Function,
                                                     rcc::PhysicalRegister Reg) {
  const auto Position =
      std::ranges::find_if(Function.frameInfo().objects(), [Reg](const rcc::StackObject &Object) {
        return !Object.isTombstone() && !Object.description()->IsDead &&
               Object.description()->CalleeSavedRegister == Reg;
      });
  return Position == Function.frameInfo().objects().end() ? nullptr : &*Position;
}

void testLeafAndSmallFrame(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Leaf = createFunction("leaf", 1);
  if (!Leaf || !buildReturn(*Leaf->Function, Target, *Leaf->Block))
    return;
  auto LoweredLeaf = runFrameLowering(*Leaf->Function, Target);
  Context.expect(
      LoweredLeaf && Leaf->Function->frameInfo().isFrozen() &&
          Leaf->Function->frameInfo().state().StackSize == 0 &&
          instructions(*Leaf->Block).size() == 1 &&
          Leaf->Function->properties().has(rcc::MachineFunctionProperty::FrameFinalized) &&
          Leaf->Function->properties().has(rcc::MachineFunctionProperty::NoFrameIndices),
      "leaf frame lowering must freeze an empty frame without extra code");

  auto DeadObject = createFunction("dead-stack-object", 24);
  if (!DeadObject)
    return;
  auto DeadSlot = createStackObject(*DeadObject->Function, rcc::StackObjectKind::Default, 32, 16);
  if (!DeadSlot || !DeadObject->Function->frameInfo().markObjectDead(*DeadSlot) ||
      !buildReturn(*DeadObject->Function, Target, *DeadObject->Block)) {
    return;
  }
  auto DeadResult = runFrameLowering(*DeadObject->Function, Target);
  Context.expect(DeadResult && DeadObject->Function->frameInfo().isFrozen() &&
                     DeadObject->Function->frameInfo().state().StackSize == 0,
                 "dead stack objects must not require layout offsets or stack space");

  auto Small = createFunction("small-frame", 2);
  if (!Small)
    return;
  auto Slot = createStackObject(*Small->Function, rcc::StackObjectKind::SpillSlot, 8, 8);
  if (!Slot)
    return;
  auto Store =
      buildStackAccess(*Small->Function, Target, *Small->Block, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X10), *Slot, 0);
  if (!Store || !buildReturn(*Small->Function, Target, *Small->Block))
    return;
  auto Lowered = runFrameLowering(*Small->Function, Target);
  Context.expect(Lowered && Small->Function->frameInfo().state().StackSize == 16 &&
                     !hasFrameIndex(*Small->Function) &&
                     countOpcode(*Small->Function, rcc::riscv::InstructionOpcode::ADDI) == 2,
                 "a small frame must use one aligned allocation and eliminate every FrameIndex");
  const auto SmallCFI = Small->Function->cfiInfo().instructions();
  Context.expect(
      Lowered && SmallCFI.size() == 4 &&
          SmallCFI.front().operation() == rcc::MachineCFIOperation::DefCFA &&
          SmallCFI.front().getIf<rcc::MachineCFIRegisterOffsetRule>()->Offset == 16 &&
          SmallCFI[1].operation() == rcc::MachineCFIOperation::RememberState &&
          SmallCFI[2].operation() == rcc::MachineCFIOperation::DefCFA &&
          SmallCFI[2].getIf<rcc::MachineCFIRegisterOffsetRule>()->Offset == 0 &&
          SmallCFI.back().operation() == rcc::MachineCFIOperation::RestoreState &&
          countOpcode(*Small->Function, rcc::opcode(rcc::GenericOpcode::CFI_INSTRUCTION)) == 4,
      "small-frame CFI must track the allocated and restored CFA exactly");
  Context.expect(Lowered && registerAt(**Store, 1) == physReg(rcc::riscv::PhysicalRegisterID::X2) &&
                     immediateAt(**Store, 2) == 0 && (*Store)->memOperands().size() == 1 &&
                     (*Store)->memOperands().front()->description().Base.kind() ==
                         rcc::MemoryBase::Kind::Stack &&
                     (*Store)->memOperands().front()->description().Offset == 0,
                 "small-frame instruction and MMO addresses must agree after elimination");

  auto MultiExit = createFunction("multi-exit-frame", 14);
  if (!MultiExit)
    return;
  auto SecondBlock = MultiExit->Function->createBasicBlock();
  if (!SecondBlock || !MultiExit->Function->appendBlock(**SecondBlock))
    return;
  auto MultiSlot = createStackObject(*MultiExit->Function, rcc::StackObjectKind::Default, 8, 8);
  if (!MultiSlot ||
      !buildStackAccess(*MultiExit->Function, Target, *MultiExit->Block,
                        rcc::riscv::InstructionOpcode::SD,
                        physical(rcc::riscv::PhysicalRegisterID::X10), *MultiSlot, 0) ||
      !buildReturn(*MultiExit->Function, Target, *MultiExit->Block) ||
      !buildReturn(*MultiExit->Function, Target, **SecondBlock)) {
    return;
  }
  auto MultiResult = runFrameLowering(*MultiExit->Function, Target);
  const auto MultiCFI = MultiExit->Function->cfiInfo().instructions();
  const auto CountCFIOperation = [MultiCFI](rcc::MachineCFIOperation Operation) {
    return std::ranges::count_if(MultiCFI,
                                 [Operation](const rcc::MachineCFIInstruction &Instruction) {
                                   return Instruction.operation() == Operation;
                                 });
  };
  Context.expect(MultiResult && MultiCFI.size() == 7 &&
                     CountCFIOperation(rcc::MachineCFIOperation::RememberState) == 2 &&
                     CountCFIOperation(rcc::MachineCFIOperation::RestoreState) == 2,
                 "multiple epilogues must isolate their linear CFI state");
}

void testCalleeSavedRegisters(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("callee-saves", 3);
  if (!Fixture)
    return;
  const rcc::Register Zero = physical(rcc::riscv::PhysicalRegisterID::X0);
  if (!buildAddImmediate(*Fixture->Function, Target, *Fixture->Block,
                         physical(rcc::riscv::PhysicalRegisterID::X1), Zero, 1) ||
      !buildAddImmediate(*Fixture->Function, Target, *Fixture->Block,
                         physical(rcc::riscv::PhysicalRegisterID::X9), Zero, 2) ||
      !buildMoveToDouble(*Fixture->Function, Target, *Fixture->Block,
                         physical(rcc::riscv::PhysicalRegisterID::F8), Zero) ||
      !buildReturn(*Fixture->Function, Target, *Fixture->Block)) {
    return;
  }
  auto Lowered = runFrameLowering(*Fixture->Function, Target);
  const auto Saved = Fixture->Function->records().calleeSavedRegisters();
  Context.expect(
      Lowered && Saved.size() == 3 && Saved[0] == physReg(rcc::riscv::PhysicalRegisterID::X1) &&
          Saved[1] == physReg(rcc::riscv::PhysicalRegisterID::X9) &&
          Saved[2] == physReg(rcc::riscv::PhysicalRegisterID::F8) &&
          countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::SD) == 2 &&
          countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::FSD) == 1 &&
          countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::LD) == 2 &&
          countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::FLD) == 1,
      "used GPR, FPR, and return-address callee saves must have symmetric save/restore code");
  Context.expect(
      Lowered && Fixture->Function->cfiInfo().instructions().size() == 10 &&
          countOpcode(*Fixture->Function, rcc::opcode(rcc::GenericOpcode::CFI_INSTRUCTION)) == 10,
      "callee-save CFI must describe every save, restore, and CFA transition");
  Context.expect(
      findCalleeSave(*Fixture->Function, physReg(rcc::riscv::PhysicalRegisterID::X1)) != nullptr &&
          findCalleeSave(*Fixture->Function, physReg(rcc::riscv::PhysicalRegisterID::X9)) !=
              nullptr &&
          findCalleeSave(*Fixture->Function, physReg(rcc::riscv::PhysicalRegisterID::F8)) !=
              nullptr &&
          !hasFrameIndex(*Fixture->Function),
      "callee-save records, stack objects, and eliminated operands must remain consistent");

  auto MaskFixture = createFunction("regmask-callee-save", 15);
  const rcc::PhysicalRegister X18 = physReg(rcc::riscv::PhysicalRegisterID::X18);
  if (!MaskFixture ||
      !buildRegisterMaskClobber(*MaskFixture->Function, Target, *MaskFixture->Block, X18) ||
      !buildReturn(*MaskFixture->Function, Target, *MaskFixture->Block)) {
    return;
  }
  auto MaskLowered = runFrameLowering(*MaskFixture->Function, Target);
  Context.expect(MaskLowered &&
                     MaskFixture->Function->records().calleeSavedRegisters().size() == 1 &&
                     MaskFixture->Function->records().calleeSavedRegisters().front() == X18 &&
                     countOpcode(*MaskFixture->Function, rcc::riscv::InstructionOpcode::SD) == 1 &&
                     countOpcode(*MaskFixture->Function, rcc::riscv::InstructionOpcode::LD) == 1,
                 "RegMask clobbers must participate in callee-save discovery");

  auto NoRestore = createFunction("callee-save-no-restore", 16);
  auto Align8 = rcc::Alignment::create(8);
  if (!NoRestore || !Align8)
    return;
  rcc::StackObjectDesc NoRestoreDescription;
  NoRestoreDescription.Kind = rcc::StackObjectKind::CalleeSaved;
  NoRestoreDescription.Size = 8;
  NoRestoreDescription.Align = *Align8;
  NoRestoreDescription.CalleeSavedRegister = X18;
  NoRestoreDescription.CalleeSavedRestored = false;
  if (!NoRestore->Function->frameInfo().createObject(std::move(NoRestoreDescription)) ||
      !buildReturn(*NoRestore->Function, Target, *NoRestore->Block)) {
    return;
  }
  auto NoRestoreResult = runFrameLowering(*NoRestore->Function, Target);
  Context.expect(
      NoRestoreResult &&
          countOpcode(*NoRestore->Function, rcc::riscv::InstructionOpcode::SD) == 1 &&
          countOpcode(*NoRestore->Function, rcc::riscv::InstructionOpcode::LD) == 0 &&
          NoRestore->Function->frameInfo().state().RestorePoints.empty(),
      "CalleeSavedRestored=false must suppress the restore without suppressing the save");
}

void testFramePointerAndFixedObjects(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("frame-pointer", 4);
  if (!Fixture)
    return;
  rcc::MachineFrameState State = Fixture->Function->frameInfo().state();
  State.FramePointerPolicy = rcc::MachineFramePointerPolicy::All;
  if (!Fixture->Function->frameInfo().setState(State))
    return;
  auto Local = createStackObject(*Fixture->Function, rcc::StackObjectKind::Default, 8, 8);
  auto Fixed = createStackObject(*Fixture->Function, rcc::StackObjectKind::FixedABI, 8, 8, 16);
  auto Variable =
      createStackObject(*Fixture->Function, rcc::StackObjectKind::VariableSized, std::nullopt, 16);
  if (!Local || !Fixed || !Variable)
    return;
  auto LocalLoad = buildStackAccess(*Fixture->Function, Target, *Fixture->Block,
                                    rcc::riscv::InstructionOpcode::LD,
                                    physical(rcc::riscv::PhysicalRegisterID::X10), *Local, 0);
  auto FixedLoad = buildStackAccess(*Fixture->Function, Target, *Fixture->Block,
                                    rcc::riscv::InstructionOpcode::LD,
                                    physical(rcc::riscv::PhysicalRegisterID::X11), *Fixed, 0);
  if (!LocalLoad || !FixedLoad || !buildReturn(*Fixture->Function, Target, *Fixture->Block)) {
    return;
  }
  auto Lowered = runFrameLowering(*Fixture->Function, Target);
  const rcc::PhysicalRegister FP = physReg(rcc::riscv::PhysicalRegisterID::X8);
  Context.expect(Lowered && registerAt(**LocalLoad, 1) == FP &&
                     immediateAt(**LocalLoad, 2) == -16 && registerAt(**FixedLoad, 1) == FP &&
                     immediateAt(**FixedLoad, 2) == 16 &&
                     findCalleeSave(*Fixture->Function, FP) != nullptr,
                 "FP frames must address locals below and fixed ABI objects above incoming SP");

  auto Preallocated = createFunction("preallocated-local-frame", 13);
  if (!Preallocated)
    return;
  rcc::MachineFrameState PreallocatedState = Preallocated->Function->frameInfo().state();
  PreallocatedState.LocalFrameSize = 32;
  if (!Preallocated->Function->frameInfo().setState(PreallocatedState))
    return;
  auto Align8 = rcc::Alignment::create(8);
  if (!Align8)
    return;
  rcc::StackObjectDesc PreallocatedDescription;
  PreallocatedDescription.Size = 8;
  PreallocatedDescription.Align = *Align8;
  PreallocatedDescription.LocalOffset = 16;
  auto PreallocatedObject =
      Preallocated->Function->frameInfo().createObject(std::move(PreallocatedDescription));
  if (!PreallocatedObject)
    return;
  auto PreallocatedStore = buildStackAccess(
      *Preallocated->Function, Target, *Preallocated->Block, rcc::riscv::InstructionOpcode::SD,
      physical(rcc::riscv::PhysicalRegisterID::X10), *PreallocatedObject, 0);
  if (!PreallocatedStore || !buildReturn(*Preallocated->Function, Target, *Preallocated->Block)) {
    return;
  }
  auto PreallocatedResult = runFrameLowering(*Preallocated->Function, Target);
  Context.expect(
      PreallocatedResult && Preallocated->Function->frameInfo().state().StackSize == 32 &&
          Preallocated->Function->frameInfo().state().LocalFrameSize == 32 &&
          registerAt(**PreallocatedStore, 1) == physReg(rcc::riscv::PhysicalRegisterID::X2) &&
          immediateAt(**PreallocatedStore, 2) == 16,
      "preallocated local blobs must preserve localFrameSize and local-offset semantics");
}

void testStackRealignment(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("realigned-frame", 5);
  if (!Fixture)
    return;
  auto Local = createStackObject(*Fixture->Function, rcc::StackObjectKind::Default, 32, 32);
  if (!Local)
    return;
  auto Store = buildStackAccess(*Fixture->Function, Target, *Fixture->Block,
                                rcc::riscv::InstructionOpcode::SD,
                                physical(rcc::riscv::PhysicalRegisterID::X10), *Local, 0);
  if (!Store || !buildReturn(*Fixture->Function, Target, *Fixture->Block))
    return;

  const auto &RA = rcc::riscv::registerAllocationInfo();
  const rcc::PhysicalRegister FP = physReg(rcc::riscv::PhysicalRegisterID::X8);
  const rcc::PhysicalRegister BP = physReg(rcc::riscv::PhysicalRegisterID::X9);
  Context.expect(RA.isReservedForFunction(*Fixture->Function, FP, Target) &&
                     RA.isReservedForFunction(*Fixture->Function, BP, Target),
                 "stack realignment must reserve both FP and BP before allocation");

  auto Lowered = runFrameLowering(*Fixture->Function, Target);
  const bool Valid = Lowered && Fixture->Function->frameInfo().stackAlignment().bytes() == 32 &&
                     Fixture->Function->frameInfo().state().StackSize == 64 &&
                     registerAt(**Store, 1) == BP && immediateAt(**Store, 2) == 32 &&
                     countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::ANDI) == 1 &&
                     findCalleeSave(*Fixture->Function, FP) != nullptr &&
                     findCalleeSave(*Fixture->Function, BP) != nullptr;
  Context.expect(Valid,
                 "over-aligned frames must align SP and use a preserved aligned base pointer");
  const auto CFI = Fixture->Function->cfiInfo().instructions();
  const std::size_t Expressions = static_cast<std::size_t>(
      std::ranges::count_if(CFI, [](const rcc::MachineCFIInstruction &Instruction) {
        const auto *Escape = Instruction.getIf<rcc::MachineCFIEscapeRule>();
        return Escape != nullptr && !Escape->Bytes.empty() && Escape->Bytes.front() == 0x10U;
      }));
  Context.expect(Lowered && Expressions == 2,
                 "realigned callee saves must use BP-relative DWARF expressions");
}

void testCallFrames(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Reserved = createFunction("reserved-call-frame", 6);
  if (!Reserved || !Reserved->Function->frameInfo().setMaxCallFrameSize(48))
    return;
  auto Align16 = rcc::Alignment::create(16);
  if (!Align16)
    return;
  rcc::StackObjectDesc OutgoingDescription;
  OutgoingDescription.Kind = rcc::StackObjectKind::Target;
  OutgoingDescription.Size = 8;
  OutgoingDescription.Align = *Align16;
  OutgoingDescription.LocalOffset = 32;
  OutgoingDescription.TargetFlags =
      static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::OutgoingArgument);
  auto Outgoing = Reserved->Function->frameInfo().createObject(std::move(OutgoingDescription));
  if (!Outgoing)
    return;
  auto OutgoingStore = buildStackAccess(
      *Reserved->Function, Target, *Reserved->Block, rcc::riscv::InstructionOpcode::SD,
      physical(rcc::riscv::PhysicalRegisterID::X10), *Outgoing, 0);
  if (!OutgoingStore ||
      !buildCallFramePseudo(*Reserved->Function, Target, *Reserved->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN, 48) ||
      !buildCallFramePseudo(*Reserved->Function, Target, *Reserved->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKUP, 48) ||
      !buildReturn(*Reserved->Function, Target, *Reserved->Block)) {
    return;
  }
  auto ReservedResult = runFrameLowering(*Reserved->Function, Target);
  Context.expect(
      ReservedResult && Reserved->Function->frameInfo().state().StackSize == 48 &&
          countOpcode(*Reserved->Function, rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN) == 0 &&
          countOpcode(*Reserved->Function, rcc::riscv::InstructionOpcode::ADJCALLSTACKUP) == 0 &&
          countOpcode(*Reserved->Function, rcc::riscv::InstructionOpcode::ADDI) == 2 &&
          registerAt(**OutgoingStore, 1) == physReg(rcc::riscv::PhysicalRegisterID::X2) &&
          immediateAt(**OutgoingStore, 2) == 32,
      "reserved call frames must fold call pseudos into the fixed frame");

  auto Computed = createFunction("computed-call-frame", 17);
  if (!Computed)
    return;
  auto ComputedOutgoingAlign = rcc::Alignment::create(64);
  if (!ComputedOutgoingAlign)
    return;
  rcc::StackObjectDesc ComputedOutgoingDescription;
  ComputedOutgoingDescription.Kind = rcc::StackObjectKind::Target;
  ComputedOutgoingDescription.Size = 8;
  ComputedOutgoingDescription.Align = *ComputedOutgoingAlign;
  ComputedOutgoingDescription.LocalOffset = 0;
  ComputedOutgoingDescription.TargetFlags =
      static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::OutgoingArgument);
  auto ComputedOutgoing =
      Computed->Function->frameInfo().createObject(std::move(ComputedOutgoingDescription));
  auto ComputedLocal = createStackObject(*Computed->Function, rcc::StackObjectKind::Default, 8, 8);
  if (!ComputedOutgoing || !ComputedLocal ||
      !buildStackAccess(*Computed->Function, Target, *Computed->Block,
                        rcc::riscv::InstructionOpcode::SD,
                        physical(rcc::riscv::PhysicalRegisterID::X10), *ComputedOutgoing, 0) ||
      !buildStackAccess(*Computed->Function, Target, *Computed->Block,
                        rcc::riscv::InstructionOpcode::SD,
                        physical(rcc::riscv::PhysicalRegisterID::X11), *ComputedLocal, 0) ||
      !buildCallFramePseudo(*Computed->Function, Target, *Computed->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN, 64) ||
      !buildCallFramePseudo(*Computed->Function, Target, *Computed->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKUP, 64) ||
      !buildReturn(*Computed->Function, Target, *Computed->Block)) {
    return;
  }
  auto ComputedResult = runFrameLowering(*Computed->Function, Target);
  const rcc::StackObject *ComputedLocalObject =
      Computed->Function->frameInfo().object(*ComputedLocal);
  Context.expect(ComputedResult && Computed->Function->frameInfo().maxCallFrameSize() == 64 &&
                     Computed->Function->frameInfo().stackAlignment().bytes() == 64 &&
                     Computed->Function->frameInfo().state().StackSize == 128 &&
                     ComputedLocalObject != nullptr &&
                     ComputedLocalObject->description()->Offset >= 64,
                 "PEI must compute and align the reserved call frame from call-frame pseudos");

  auto Mismatched = createFunction("mismatched-call-frame", 18);
  if (!Mismatched ||
      !buildCallFramePseudo(*Mismatched->Function, Target, *Mismatched->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN, 48, 16) ||
      !buildCallFramePseudo(*Mismatched->Function, Target, *Mismatched->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKUP, 48, 24) ||
      !buildReturn(*Mismatched->Function, Target, *Mismatched->Block)) {
    return;
  }
  auto MismatchResult = runFrameLowering(*Mismatched->Function, Target);
  Context.expect(
      !MismatchResult && MismatchResult.error().code() == rcc::ErrorCode::VerificationError &&
          !Mismatched->Function->frameInfo().isFrozen() &&
          countOpcode(*Mismatched->Function, rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN) ==
              1 &&
          countOpcode(*Mismatched->Function, rcc::riscv::InstructionOpcode::ADJCALLSTACKUP) == 1,
      "mismatched call-frame pairs must fail transactionally");

  auto Unmatched = createFunction("unmatched-call-frame", 19);
  if (!Unmatched ||
      !buildCallFramePseudo(*Unmatched->Function, Target, *Unmatched->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN, 32) ||
      !buildReturn(*Unmatched->Function, Target, *Unmatched->Block)) {
    return;
  }
  auto UnmatchedResult = runFrameLowering(*Unmatched->Function, Target);
  Context.expect(!UnmatchedResult &&
                     UnmatchedResult.error().code() == rcc::ErrorCode::VerificationError &&
                     !Unmatched->Function->frameInfo().isFrozen(),
                 "unmatched call-frame setup must fail transactionally");

  auto Dynamic = createFunction("dynamic-call-frame", 7);
  if (!Dynamic)
    return;
  auto Variable =
      createStackObject(*Dynamic->Function, rcc::StackObjectKind::DynamicAlloca, std::nullopt, 16);
  auto OverAligned = createStackObject(*Dynamic->Function, rcc::StackObjectKind::Default, 8, 32);
  if (!Variable || !OverAligned ||
      !buildCallFramePseudo(*Dynamic->Function, Target, *Dynamic->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN, 4096) ||
      !buildCallFramePseudo(*Dynamic->Function, Target, *Dynamic->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKUP, 4096) ||
      !buildReturn(*Dynamic->Function, Target, *Dynamic->Block)) {
    return;
  }
  Context.expect(rcc::riscv::registerAllocationInfo().isReservedForFunction(
                     *Dynamic->Function, physReg(rcc::riscv::PhysicalRegisterID::X9), Target),
                 "over-aligned dynamic frames must reserve BP before register allocation");
  auto DynamicResult = runFrameLowering(*Dynamic->Function, Target);
  Context.expect(
      DynamicResult &&
          countOpcode(*Dynamic->Function, rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN) == 0 &&
          countOpcode(*Dynamic->Function, rcc::riscv::InstructionOpcode::ADJCALLSTACKUP) == 0 &&
          countOpcode(*Dynamic->Function, rcc::riscv::InstructionOpcode::ADD) >= 2 &&
          countOpcode(*Dynamic->Function, rcc::riscv::InstructionOpcode::ANDI) == 1 &&
          findStackObject(*Dynamic->Function, rcc::StackObjectKind::EmergencySpill) != nullptr,
      "dynamic large call frames must materialize SP adjustments and reserve emergency space");

  auto DynamicSpill = createFunction("dynamic-call-frame-spill", 25);
  if (!DynamicSpill)
    return;
  auto DynamicVariable = createStackObject(*DynamicSpill->Function,
                                           rcc::StackObjectKind::DynamicAlloca, std::nullopt, 16);
  const std::vector<rcc::riscv::PhysicalRegisterID> ScratchIDs = {
      rcc::riscv::PhysicalRegisterID::X5,  rcc::riscv::PhysicalRegisterID::X6,
      rcc::riscv::PhysicalRegisterID::X7,  rcc::riscv::PhysicalRegisterID::X28,
      rcc::riscv::PhysicalRegisterID::X29, rcc::riscv::PhysicalRegisterID::X30,
      rcc::riscv::PhysicalRegisterID::X31,
  };
  const rcc::Register Zero = physical(rcc::riscv::PhysicalRegisterID::X0);
  if (!DynamicVariable)
    return;
  for (std::size_t Index = 0; Index < ScratchIDs.size(); ++Index) {
    if (!buildAddImmediate(*DynamicSpill->Function, Target, *DynamicSpill->Block,
                           physical(ScratchIDs[Index]), Zero,
                           static_cast<std::int64_t>(Index + 1U))) {
      return;
    }
  }
  if (!buildCallFramePseudo(*DynamicSpill->Function, Target, *DynamicSpill->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN, 4096) ||
      !buildCallFramePseudo(*DynamicSpill->Function, Target, *DynamicSpill->Block,
                            rcc::riscv::InstructionOpcode::ADJCALLSTACKUP, 4096)) {
    return;
  }
  for (const rcc::riscv::PhysicalRegisterID ID : ScratchIDs) {
    if (!buildAdd(*DynamicSpill->Function, Target, *DynamicSpill->Block,
                  physical(rcc::riscv::PhysicalRegisterID::X10), physical(ID), Zero)) {
      return;
    }
  }
  if (!buildReturn(*DynamicSpill->Function, Target, *DynamicSpill->Block))
    return;
  auto DynamicSpillResult = runFrameLowering(*DynamicSpill->Function, Target);
  const rcc::PhysicalRegister FP = physReg(rcc::riscv::PhysicalRegisterID::X8);
  std::size_t StableEmergencyStores = 0;
  std::size_t StableEmergencyLoads = 0;
  for (const rcc::MachineInstr *Instruction : DynamicSpill->Function->allInstructions()) {
    if (Instruction == nullptr || Instruction->parent() == nullptr ||
        Instruction->memOperands().empty() ||
        Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup) ||
        Instruction->flags().has(rcc::MachineInstrFlag::FrameDestroy)) {
      continue;
    }
    const auto &Memory = Instruction->memOperands().front()->description();
    if (Memory.Base.kind() != rcc::MemoryBase::Kind::Stack || registerAt(*Instruction, 1) != FP)
      continue;
    StableEmergencyStores +=
        Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::SD) ? 1U : 0U;
    StableEmergencyLoads +=
        Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::LD) ? 1U : 0U;
  }
  Context.expect(DynamicSpillResult && StableEmergencyStores == 2 && StableEmergencyLoads == 2,
                 "dynamic call-frame scavenging must preserve scratch values through stable "
                 "FP-relative emergency slots");
}

void testLargeOffsetAndScavenging(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("large-frame-offset", 8);
  if (!Fixture)
    return;
  auto Local = createStackObject(*Fixture->Function, rcc::StackObjectKind::Default, 8192, 8);
  if (!Local)
    return;

  const std::vector<rcc::riscv::PhysicalRegisterID> ScratchIDs = {
      rcc::riscv::PhysicalRegisterID::X5,  rcc::riscv::PhysicalRegisterID::X6,
      rcc::riscv::PhysicalRegisterID::X7,  rcc::riscv::PhysicalRegisterID::X28,
      rcc::riscv::PhysicalRegisterID::X29, rcc::riscv::PhysicalRegisterID::X30,
      rcc::riscv::PhysicalRegisterID::X31,
  };
  const rcc::Register Zero = physical(rcc::riscv::PhysicalRegisterID::X0);
  for (std::size_t Index = 0; Index < ScratchIDs.size(); ++Index) {
    if (!buildAddImmediate(*Fixture->Function, Target, *Fixture->Block, physical(ScratchIDs[Index]),
                           Zero, static_cast<std::int64_t>(Index + 1U))) {
      return;
    }
  }
  auto Store = buildStackAccess(*Fixture->Function, Target, *Fixture->Block,
                                rcc::riscv::InstructionOpcode::SD,
                                physical(rcc::riscv::PhysicalRegisterID::X10), *Local, 5000);
  if (!Store)
    return;
  for (const rcc::riscv::PhysicalRegisterID ID : ScratchIDs) {
    if (!buildAdd(*Fixture->Function, Target, *Fixture->Block,
                  physical(rcc::riscv::PhysicalRegisterID::X10), physical(ID), Zero)) {
      return;
    }
  }
  if (!buildReturn(*Fixture->Function, Target, *Fixture->Block))
    return;

  auto Lowered = runFrameLowering(*Fixture->Function, Target);
  Context.expect(Lowered &&
                     findStackObject(*Fixture->Function, rcc::StackObjectKind::EmergencySpill) !=
                         nullptr &&
                     registerAt(**Store, 1) == physReg(rcc::riscv::PhysicalRegisterID::X5) &&
                     immediateAt(**Store, 2) == 0 && !hasFrameIndex(*Fixture->Function),
                 "large FrameIndex offsets must use a scavenged physical address register");

  bool HasUnflaggedEmergencyStore = false;
  bool HasUnflaggedEmergencyLoad = false;
  for (rcc::MachineInstr *Instruction : instructions(*Fixture->Block)) {
    if (Instruction == *Store || Instruction->memOperands().empty())
      continue;
    const auto &Memory = Instruction->memOperands().front()->description();
    if (Memory.Base.kind() != rcc::MemoryBase::Kind::Stack || Memory.Offset != 0)
      continue;
    const bool HasFrameFlag = Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup) ||
                              Instruction->flags().has(rcc::MachineInstrFlag::FrameDestroy);
    HasUnflaggedEmergencyStore |=
        Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::SD) && !HasFrameFlag;
    HasUnflaggedEmergencyLoad |=
        Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::LD) && !HasFrameFlag;
  }
  Context.expect(HasUnflaggedEmergencyStore && HasUnflaggedEmergencyLoad,
                 "live scratch registers must be preserved without false frame flags");

  auto Huge = createFunction("huge-rv64-frame", 26);
  constexpr std::uint64_t HugeFrameSize = (std::uint64_t{1} << 32U) + 16U;
  if (!Huge ||
      !createStackObject(*Huge->Function, rcc::StackObjectKind::Default, HugeFrameSize, 16) ||
      !buildReturn(*Huge->Function, Target, *Huge->Block)) {
    return;
  }
  auto HugeResult = runFrameLowering(*Huge->Function, Target);
  Context.expect(HugeResult && Huge->Function->frameInfo().state().StackSize == HugeFrameSize &&
                     countOpcode(*Huge->Function, rcc::riscv::InstructionOpcode::ADD) >= 2,
                 "RV64 frames larger than uint32 must use 64-bit stack adjustments");

  auto TailScratch = createFunction("large-frame-tail-scratch", 27);
  if (!TailScratch ||
      !createStackObject(*TailScratch->Function, rcc::StackObjectKind::Default, 8192, 16) ||
      !buildIndirectTail(*TailScratch->Function, Target, *TailScratch->Block,
                         physical(rcc::riscv::PhysicalRegisterID::X5))) {
    return;
  }
  auto TailResult = runFrameLowering(*TailScratch->Function, Target);
  bool FrameCodeClobbersTailCallee = false;
  for (const rcc::MachineInstr *Instruction : TailScratch->Function->allInstructions()) {
    if (Instruction == nullptr || Instruction->parent() == nullptr ||
        (!Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup) &&
         !Instruction->flags().has(rcc::MachineInstrFlag::FrameDestroy))) {
      continue;
    }
    FrameCodeClobbersTailCallee |=
        registerAt(*Instruction, 0) == physReg(rcc::riscv::PhysicalRegisterID::X5);
  }
  Context.expect(TailResult && !FrameCodeClobbersTailCallee,
                 "large prologue/epilogue adjustments must not clobber a tail-call operand");
}

void testCFGScavenging(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("cfg-scavenging", 13);
  if (!Fixture)
    return;
  auto Loop = Fixture->Function->createBasicBlock();
  if (!Loop || !Fixture->Function->appendBlock(**Loop) ||
      !Fixture->Function->cfgUpdater().addSuccessor(*Fixture->Block, **Loop,
                                                    rcc::BranchProbability::one()) ||
      !Fixture->Function->cfgUpdater().addSuccessor(**Loop, **Loop,
                                                    rcc::BranchProbability::one())) {
    return;
  }
  auto Local = createStackObject(*Fixture->Function, rcc::StackObjectKind::Default, 8192, 8);
  if (!Local)
    return;

  const std::vector<rcc::riscv::PhysicalRegisterID> ScratchIDs = {
      rcc::riscv::PhysicalRegisterID::X5,  rcc::riscv::PhysicalRegisterID::X6,
      rcc::riscv::PhysicalRegisterID::X7,  rcc::riscv::PhysicalRegisterID::X28,
      rcc::riscv::PhysicalRegisterID::X29, rcc::riscv::PhysicalRegisterID::X30,
      rcc::riscv::PhysicalRegisterID::X31,
  };
  const rcc::Register Zero = physical(rcc::riscv::PhysicalRegisterID::X0);
  for (std::size_t Index = 0; Index < ScratchIDs.size(); ++Index) {
    if (!buildAddImmediate(*Fixture->Function, Target, *Fixture->Block, physical(ScratchIDs[Index]),
                           Zero, static_cast<std::int64_t>(Index + 1U))) {
      return;
    }
  }
  auto Store = buildStackAccess(*Fixture->Function, Target, *Fixture->Block,
                                rcc::riscv::InstructionOpcode::SD,
                                physical(rcc::riscv::PhysicalRegisterID::X10), *Local, 5000);
  if (!Store)
    return;
  for (const rcc::riscv::PhysicalRegisterID ID : ScratchIDs) {
    if (!buildAdd(*Fixture->Function, Target, **Loop, physical(rcc::riscv::PhysicalRegisterID::X10),
                  physical(ID), Zero)) {
      return;
    }
  }
  if (!buildReturn(*Fixture->Function, Target, **Loop))
    return;

  Context.expect((*Loop)->liveIns().empty(),
                 "CFG scavenging fixture must not depend on prebuilt live-ins");
  auto Lowered = runFrameLowering(*Fixture->Function, Target);
  bool HasEmergencyStore = false;
  bool HasEmergencyLoad = false;
  for (rcc::MachineInstr *Instruction : instructions(*Fixture->Block)) {
    if (Instruction == *Store || Instruction->memOperands().empty())
      continue;
    const auto &Memory = Instruction->memOperands().front()->description();
    if (Memory.Base.kind() != rcc::MemoryBase::Kind::Stack || Memory.Offset != 0) {
      continue;
    }
    HasEmergencyStore |= Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::SD);
    HasEmergencyLoad |= Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::LD);
  }
  Context.expect(
      Lowered && HasEmergencyStore && HasEmergencyLoad &&
          registerAt(**Store, 1) == physReg(rcc::riscv::PhysicalRegisterID::X5) &&
          !hasFrameIndex(*Fixture->Function),
      "successor and loop uses must keep scratch registers live across FrameIndex elimination");
}

[[nodiscard]] std::vector<std::int64_t> frameSignature(const rcc::MachineFunction &Function) {
  std::vector<std::int64_t> Result;
  Result.push_back(static_cast<std::int64_t>(Function.frameInfo().state().StackSize));
  Result.push_back(static_cast<std::int64_t>(Function.frameInfo().stackAlignment().bytes()));
  for (const rcc::StackObject &Object : Function.frameInfo().objects()) {
    if (Object.isTombstone() || Object.description()->IsDead)
      continue;
    Result.push_back(static_cast<std::int64_t>(Object.description()->Kind));
    Result.push_back(Object.description()->Offset.value_or(-1));
  }
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Result.push_back(static_cast<std::int64_t>(Instruction->opcode().value()));
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        Result.push_back(immediateAt(*Instruction, Index).value_or(0));
        Result.push_back(registerAt(*Instruction, Index)
                             .transform([](rcc::PhysicalRegister Reg) {
                               return static_cast<std::int64_t>(Reg.number());
                             })
                             .value_or(0));
      }
    }
  }
  return Result;
}

[[nodiscard]] rcc::Expected<FunctionFixture>
createDeterminismFixture(const rcc::TargetDescription &Target, std::uint64_t StableID) {
  auto Fixture = createFunction("deterministic-frame", StableID);
  if (!Fixture)
    return std::unexpected(Fixture.error());
  auto Small = createStackObject(*Fixture->Function, rcc::StackObjectKind::Default, 8, 8);
  auto Large = createStackObject(*Fixture->Function, rcc::StackObjectKind::SpillSlot, 24, 16);
  if (!Small || !Large)
    return std::unexpected(!Small ? Small.error() : Large.error());
  auto Store = buildStackAccess(*Fixture->Function, Target, *Fixture->Block,
                                rcc::riscv::InstructionOpcode::SD,
                                physical(rcc::riscv::PhysicalRegisterID::X10), *Small, 0);
  if (!Store)
    return std::unexpected(Store.error());
  auto Return = buildReturn(*Fixture->Function, Target, *Fixture->Block);
  if (!Return)
    return std::unexpected(Return.error());
  return Fixture;
}

void testPEIOrderingAndPostconditions(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Ordered = createFunction("pei-ordering", 22);
  if (!Ordered ||
      !buildAddImmediate(*Ordered->Function, Target, *Ordered->Block,
                         physical(rcc::riscv::PhysicalRegisterID::X18),
                         physical(rcc::riscv::PhysicalRegisterID::X0), 1) ||
      !buildReturn(*Ordered->Function, Target, *Ordered->Block)) {
    return;
  }
  ObservingFrameLowering Observer(rcc::riscv::frameLowering());
  auto OrderedResult = runFrameLoweringWith(*Ordered->Function, Target, Observer);
  Context.expect(
      OrderedResult && Observer.PrepareSawUnfrozen && Observer.InsertSawUnfrozen &&
          Observer.FinalizeSawUnfrozen && Observer.FinalizeSawCalleeSave &&
          Observer.FinalizeProducedFrozen && Observer.PrologueSawFrozen &&
          Observer.EpilogueSawFrozen,
      "PEI must insert callee saves before freeze and emit prologue/epilogue after freeze");

  auto InvalidPostcondition = createFunction("pei-invalid-postcondition", 23);
  if (!InvalidPostcondition)
    return;
  auto Slot =
      createStackObject(*InvalidPostcondition->Function, rcc::StackObjectKind::Default, 8, 8);
  if (!Slot)
    return;
  auto Store = buildStackAccess(*InvalidPostcondition->Function, Target,
                                *InvalidPostcondition->Block, rcc::riscv::InstructionOpcode::SD,
                                physical(rcc::riscv::PhysicalRegisterID::X10), *Slot, 0);
  if (!Store ||
      !buildReturn(*InvalidPostcondition->Function, Target, *InvalidPostcondition->Block)) {
    return;
  }
  const std::size_t OriginalInstructionCount = InvalidPostcondition->Block->instructionCount();
  ObservingFrameLowering Broken(rcc::riscv::frameLowering(), true);
  auto Rejected = runFrameLoweringWith(*InvalidPostcondition->Function, Target, Broken);
  Context.expect(
      !Rejected && !InvalidPostcondition->Function->frameInfo().isFrozen() &&
          InvalidPostcondition->Block->instructionCount() == OriginalInstructionCount &&
          (*Store)->operandAt(1)->kind() == rcc::MachineOperand::Kind::FrameIndex &&
          InvalidPostcondition->Function->cfiInfo().instructions().empty(),
      "PEI transaction preflight must verify final properties before mutating the original");
}

void testAtomicityAndDeterminism(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Invalid = createFunction("invalid-frame", 9);
  if (!Invalid)
    return;
  auto Slot = createStackObject(*Invalid->Function, rcc::StackObjectKind::Default, 8, 8);
  if (!Slot)
    return;
  auto Store = buildStackAccess(*Invalid->Function, Target, *Invalid->Block,
                                rcc::riscv::InstructionOpcode::SD,
                                physical(rcc::riscv::PhysicalRegisterID::X10), *Slot, 0);
  if (!Store || !(*Store)->eraseOperand(2) ||
      !buildReturn(*Invalid->Function, Target, *Invalid->Block)) {
    return;
  }
  auto Rejected = runFrameLowering(*Invalid->Function, Target);
  Context.expect(!Rejected && !Invalid->Function->frameInfo().isFrozen() &&
                     Invalid->Function->frameInfo().liveObjectCount() == 1 &&
                     (*Store)->operandCount() == 2 &&
                     (*Store)->operandAt(1)->kind() == rcc::MachineOperand::Kind::FrameIndex,
                 "failed frame lowering must leave the original MachineFunction unchanged");

  auto OverflowingCallFrame = createFunction("overflowing-call-frame", 12);
  if (!OverflowingCallFrame ||
      !OverflowingCallFrame->Function->frameInfo().setMaxCallFrameSize(16)) {
    return;
  }
  auto Align8 = rcc::Alignment::create(8);
  if (!Align8)
    return;
  rcc::StackObjectDesc OutgoingDescription;
  OutgoingDescription.Kind = rcc::StackObjectKind::Target;
  OutgoingDescription.Size = 8;
  OutgoingDescription.Align = *Align8;
  OutgoingDescription.LocalOffset = 16;
  OutgoingDescription.TargetFlags =
      static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::OutgoingArgument);
  auto Outgoing =
      OverflowingCallFrame->Function->frameInfo().createObject(std::move(OutgoingDescription));
  if (!Outgoing)
    return;
  auto OutgoingStore =
      buildStackAccess(*OverflowingCallFrame->Function, Target, *OverflowingCallFrame->Block,
                       rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X10), *Outgoing, 0);
  if (!OutgoingStore ||
      !buildReturn(*OverflowingCallFrame->Function, Target, *OverflowingCallFrame->Block)) {
    return;
  }
  auto FrameRejected = runFrameLowering(*OverflowingCallFrame->Function, Target);
  Context.expect(
      !FrameRejected && FrameRejected.error().code() == rcc::ErrorCode::FrameLayoutFailed &&
          !OverflowingCallFrame->Function->frameInfo().isFrozen() &&
          OverflowingCallFrame->Function->cfiInfo().instructions().empty() &&
          (*OutgoingStore)->operandAt(1)->kind() == rcc::MachineOperand::Kind::FrameIndex &&
          (*OutgoingStore)->memOperands().front()->description().Base.frameIndex() == *Outgoing,
      "transaction preflight must reject invalid call-frame layout without mutation");

  auto MalformedSave = createFunction("malformed-callee-save", 20);
  auto Align4 = rcc::Alignment::create(4);
  if (!MalformedSave || !Align4)
    return;
  rcc::StackObjectDesc MalformedDescription;
  MalformedDescription.Kind = rcc::StackObjectKind::CalleeSaved;
  MalformedDescription.Size = 4;
  MalformedDescription.Align = *Align4;
  MalformedDescription.CalleeSavedRegister = physReg(rcc::riscv::PhysicalRegisterID::X18);
  if (!MalformedSave->Function->frameInfo().createObject(std::move(MalformedDescription)) ||
      !buildReturn(*MalformedSave->Function, Target, *MalformedSave->Block)) {
    return;
  }
  auto MalformedResult = runFrameLowering(*MalformedSave->Function, Target);
  Context.expect(
      !MalformedResult && MalformedResult.error().code() == rcc::ErrorCode::VerificationError &&
          !MalformedSave->Function->frameInfo().isFrozen() &&
          MalformedSave->Function->frameInfo().liveObjectCount() == 1 &&
          MalformedSave->Function->cfiInfo().instructions().empty(),
      "malformed callee-save slots must be rejected before mutating the original function");

  auto DuplicateSave = createFunction("duplicate-callee-save", 28);
  auto Align8ForDuplicates = rcc::Alignment::create(8);
  if (!DuplicateSave || !Align8ForDuplicates)
    return;
  for (std::size_t Index = 0; Index < 2; ++Index) {
    rcc::StackObjectDesc DuplicateDescription;
    DuplicateDescription.Kind = rcc::StackObjectKind::CalleeSaved;
    DuplicateDescription.Size = 8;
    DuplicateDescription.Align = *Align8ForDuplicates;
    DuplicateDescription.CalleeSavedRegister = physReg(rcc::riscv::PhysicalRegisterID::X18);
    if (!DuplicateSave->Function->frameInfo().createObject(std::move(DuplicateDescription))) {
      return;
    }
  }
  if (!buildReturn(*DuplicateSave->Function, Target, *DuplicateSave->Block))
    return;
  auto DuplicateResult = runFrameLowering(*DuplicateSave->Function, Target);
  Context.expect(!DuplicateResult &&
                     DuplicateResult.error().code() == rcc::ErrorCode::VerificationError &&
                     !DuplicateSave->Function->frameInfo().isFrozen(),
                 "duplicate callee-save slots must be rejected transactionally");

  auto ReservedFrameRegister = createFunction("used-reserved-frame-register", 21);
  if (!ReservedFrameRegister)
    return;
  auto OverAligned =
      createStackObject(*ReservedFrameRegister->Function, rcc::StackObjectKind::Default, 32, 32);
  if (!OverAligned ||
      !buildAddImmediate(*ReservedFrameRegister->Function, Target, *ReservedFrameRegister->Block,
                         physical(rcc::riscv::PhysicalRegisterID::X8),
                         physical(rcc::riscv::PhysicalRegisterID::X0), 1) ||
      !buildReturn(*ReservedFrameRegister->Function, Target, *ReservedFrameRegister->Block)) {
    return;
  }
  auto ReservedFrameResult = runFrameLowering(*ReservedFrameRegister->Function, Target);
  Context.expect(
      !ReservedFrameResult &&
          ReservedFrameResult.error().code() == rcc::ErrorCode::VerificationError &&
          !ReservedFrameRegister->Function->frameInfo().isFrozen() &&
          ReservedFrameRegister->Function->frameInfo().liveObjectCount() == 1,
      "post-RA uses of required FP/BP registers must fail without partial frame mutation");

  auto First = createDeterminismFixture(Target, 10);
  auto Second = createDeterminismFixture(Target, 11);
  if (!First || !Second)
    return;
  auto FirstResult = runFrameLowering(*First->Function, Target);
  auto SecondResult = runFrameLowering(*Second->Function, Target);
  Context.expect(FirstResult && SecondResult &&
                     frameSignature(*First->Function) == frameSignature(*Second->Function),
                 "identical frame inputs must produce deterministic layouts and code");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC frame-lowering target must be constructible");
  if (!Target)
    return Context.result();

  testLeafAndSmallFrame(Context, *Target);
  testCalleeSavedRegisters(Context, *Target);
  testFramePointerAndFixedObjects(Context, *Target);
  testStackRealignment(Context, *Target);
  testCallFrames(Context, *Target);
  testLargeOffsetAndScavenging(Context, *Target);
  testCFGScavenging(Context, *Target);
  testPEIOrderingAndPostconditions(Context, *Target);
  testAtomicityAndDeterminism(Context, *Target);
  return Context.result();
}
