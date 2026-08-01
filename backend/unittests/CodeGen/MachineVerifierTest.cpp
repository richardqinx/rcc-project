// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineVerifier.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t GenericDefOpcode = 1;
constexpr std::uint32_t GenericUseOpcode = 2;
constexpr std::uint32_t CopyOpcode = 3;
constexpr std::uint32_t BranchOpcode = 4;
constexpr std::uint32_t PhiOpcode = 5;
constexpr std::uint32_t FrameOpcode = 6;
constexpr std::uint32_t PseudoOpcode = 7;
constexpr std::uint32_t NopOpcode = 8;
constexpr std::uint32_t ConstantPoolOpcode = 9;
constexpr std::uint32_t JumpTableOpcode = 10;

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] rcc::OperandDescriptor registerDescriptor(rcc::RegisterOperandRole Role) {
  rcc::OperandDescriptor Descriptor;
  Descriptor.Kinds = {rcc::MachineOperand::Kind::Register};
  Descriptor.RegisterRole = Role;
  return Descriptor;
}

[[nodiscard]] rcc::InstructionDescriptor instructionDescriptor(std::uint32_t OpcodeValue,
                                                               std::string Name) {
  rcc::InstructionDescriptor Descriptor;
  Descriptor.InstructionOpcode = rcc::Opcode::fromValue(OpcodeValue);
  Descriptor.Name = std::move(Name);
  Descriptor.Flags = {rcc::InstructionFlag::ProducesCode};
  return Descriptor;
}

[[nodiscard]] rcc::Expected<rcc::InstructionSet> createInstructionSet() {
  auto GenericDef = instructionDescriptor(GenericDefOpcode, "TEST_GENERIC_DEF");
  GenericDef.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Def));
  GenericDef.NumExplicitDefs = 1;
  GenericDef.Flags.set(rcc::InstructionFlag::Generic);
  GenericDef.Flags.set(rcc::InstructionFlag::AllowedPreLegalizer);

  auto GenericUse = instructionDescriptor(GenericUseOpcode, "TEST_GENERIC_USE");
  GenericUse.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Use));
  GenericUse.Flags.set(rcc::InstructionFlag::Generic);
  GenericUse.Flags.set(rcc::InstructionFlag::AllowedPreLegalizer);

  auto Copy = instructionDescriptor(CopyOpcode, "TEST_COPY");
  auto CopyDef = registerDescriptor(rcc::RegisterOperandRole::Def);
  auto CopyUse = registerDescriptor(rcc::RegisterOperandRole::Use);
  CopyDef.DefaultTiedOperand = 1;
  CopyUse.DefaultTiedOperand = 0;
  Copy.Operands.push_back(CopyDef);
  Copy.Operands.push_back(CopyUse);
  Copy.NumExplicitDefs = 1;

  auto Branch = instructionDescriptor(BranchOpcode, "TEST_BRANCH");
  rcc::OperandDescriptor Block;
  Block.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
  Branch.Operands.push_back(Block);
  Branch.Flags.set(rcc::InstructionFlag::Terminator);
  Branch.Flags.set(rcc::InstructionFlag::Branch);

  auto Phi = instructionDescriptor(PhiOpcode, "TEST_PHI");
  Phi.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Def));
  Phi.NumExplicitDefs = 1;
  Phi.Flags.set(rcc::InstructionFlag::Pseudo);
  Phi.Flags.set(rcc::InstructionFlag::Phi);
  Phi.Owner = rcc::PseudoOwner::PhiElimination;
  Phi.Deadline = rcc::PipelineStage::SSAEliminated;

  auto Frame = instructionDescriptor(FrameOpcode, "TEST_FRAME");
  rcc::OperandDescriptor FrameIndex;
  FrameIndex.Kinds = {rcc::MachineOperand::Kind::FrameIndex};
  Frame.Operands.push_back(FrameIndex);

  auto Pseudo = instructionDescriptor(PseudoOpcode, "TEST_PSEUDO");
  Pseudo.Flags.set(rcc::InstructionFlag::Pseudo);
  Pseudo.Owner = rcc::PseudoOwner::InstructionSelect;
  Pseudo.Deadline = rcc::PipelineStage::Selected;

  auto Nop = instructionDescriptor(NopOpcode, "TEST_NOP");

  auto ConstantPool = instructionDescriptor(ConstantPoolOpcode, "TEST_CONSTANT_POOL");
  rcc::OperandDescriptor ConstantPoolIndex;
  ConstantPoolIndex.Kinds = {rcc::MachineOperand::Kind::ConstantPoolIndex};
  ConstantPool.Operands.push_back(ConstantPoolIndex);

  auto JumpTable = instructionDescriptor(JumpTableOpcode, "TEST_JUMP_TABLE");
  rcc::OperandDescriptor JumpTableIndex;
  JumpTableIndex.Kinds = {rcc::MachineOperand::Kind::JumpTableIndex};
  JumpTable.Operands.push_back(JumpTableIndex);

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(GenericDef));
  Descriptors.push_back(std::move(GenericUse));
  Descriptors.push_back(std::move(Copy));
  Descriptors.push_back(std::move(Branch));
  Descriptors.push_back(std::move(Phi));
  Descriptors.push_back(std::move(Frame));
  Descriptors.push_back(std::move(Pseudo));
  Descriptors.push_back(std::move(Nop));
  Descriptors.push_back(std::move(ConstantPool));
  Descriptors.push_back(std::move(JumpTable));
  return rcc::InstructionSet::create(std::move(Descriptors));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFunction(TestContext &Context, std::string Name, std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] rcc::MachineBasicBlock *createBlock(TestContext &Context,
                                                  rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  Context.expect(Block.has_value(), "machine basic block creation must succeed");
  if (!Block)
    return nullptr;
  Context.expect(Function.appendBlock(**Block).has_value(),
                 "machine basic block linking must succeed");
  return *Block;
}

[[nodiscard]] rcc::MachineInstr *
buildNoOperand(TestContext &Context, rcc::MachineIRBuilder &Builder, std::uint32_t OpcodeValue) {
  auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(OpcodeValue));
  Context.expect(Pending.has_value(), "pending no-operand instruction must be created");
  if (!Pending)
    return nullptr;
  auto Result = Pending->build();
  Context.expect(Result.has_value(), "no-operand instruction must build");
  if (!Result)
    return nullptr;
  return *Result;
}

[[nodiscard]] bool populateValidPreLegalizer(TestContext &Context, rcc::MachineFunction &Function,
                                             const rcc::InstructionSet &InstructionSet) {
  rcc::MachineBasicBlock *Entry = createBlock(Context, Function);
  rcc::MachineBasicBlock *Exit = createBlock(Context, Function);
  if (Entry == nullptr || Exit == nullptr)
    return false;

  auto Type = rcc::LLT::makeInteger(64);
  auto Value = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Value.has_value(), "typed verifier virtual register must be allocated");
  if (!Value)
    return false;

  rcc::MachineIRBuilder Builder(Function, InstructionSet);
  Context.expect(Builder.setInsertPoint(*Entry).has_value(), "entry insertion point must be valid");
  auto Def = Builder.buildInstr(rcc::Opcode::fromValue(GenericDefOpcode));
  Context.expect(Def && Def->addDef(*Value).has_value(), "generic definition must be assembled");
  if (!Def || !Def->build())
    return false;

  auto Branch = Builder.buildInstr(rcc::Opcode::fromValue(BranchOpcode));
  Context.expect(Branch && Branch->addBasicBlock(*Exit).has_value(),
                 "branch operand must be assembled");
  if (!Branch || !Branch->build())
    return false;

  Context.expect(
      Function.cfgUpdater().addSuccessor(*Entry, *Exit, rcc::BranchProbability::one()).has_value(),
      "valid verifier CFG edge must be created");

  Context.expect(Builder.setInsertPoint(*Exit).has_value(), "exit insertion point must be valid");
  auto Use = Builder.buildInstr(rcc::Opcode::fromValue(GenericUseOpcode));
  Context.expect(Use && Use->addUse(*Value).has_value(), "generic use must be assembled");
  if (!Use || !Use->build())
    return false;

  Function.properties().set(rcc::MachineFunctionProperty::IsSSA);
  Function.properties().set(rcc::MachineFunctionProperty::TracksLiveness);
  return true;
}

void testValidFunctionAndModes(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Function = createFunction(Context, "valid", 1);
  if (!InstructionSet || !Function ||
      !populateValidPreLegalizer(Context, *Function, *InstructionSet)) {
    return;
  }

  Context.expect(rcc::MachineVerifier::verify(*Function, *InstructionSet).has_value(),
                 "well-formed function must pass generic verification");
  Context.expect(
      rcc::MachineVerifier::verify(
          *Function, *InstructionSet,
          rcc::MachineVerifierOptions{rcc::MachineVerifierMode::PreLegalizer, std::nullopt, true})
          .has_value(),
      "typed SSA input must pass pre-legalizer verification");

  auto DetachedBlock = Function->createBasicBlock();
  auto DetachedInstruction = Function->createInstruction(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(DetachedBlock && DetachedInstruction,
                 "detached arena history must be allocatable");
  Context.expect(rcc::MachineVerifier::verify(*Function, *InstructionSet).has_value(),
                 "empty detached arena objects must not invalidate live layout");

  Function->properties().set(rcc::MachineFunctionProperty::Legalized);
  Context.expect(
      !rcc::MachineVerifier::verify(
          *Function, *InstructionSet,
          rcc::MachineVerifierOptions{rcc::MachineVerifierMode::PreLegalizer, std::nullopt, true}),
      "pre-legalizer mode must reject later-stage properties");
}

void testReachableStructuralFailures(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  if (!InstructionSet)
    return;

  auto Unknown = createFunction(Context, "unknown_opcode", 2);
  rcc::MachineBasicBlock *UnknownBlock = Unknown ? createBlock(Context, *Unknown) : nullptr;
  if (Unknown && UnknownBlock != nullptr) {
    auto Instruction = Unknown->createInstruction(rcc::Opcode::fromValue(999));
    Context.expect(Instruction && UnknownBlock->appendInstruction(**Instruction).has_value(),
                   "unknown opcode negative test must be linked");
    Context.expect(!rcc::MachineVerifier::verify(*Unknown, *InstructionSet),
                   "live unregistered opcode must fail verification");
  }

  auto WrongOperands = createFunction(Context, "wrong_operands", 3);
  rcc::MachineBasicBlock *WrongBlock =
      WrongOperands ? createBlock(Context, *WrongOperands) : nullptr;
  if (WrongOperands && WrongBlock != nullptr) {
    auto Instruction = WrongOperands->createInstruction(rcc::Opcode::fromValue(GenericDefOpcode));
    Context.expect(Instruction && WrongBlock->appendInstruction(**Instruction).has_value(),
                   "wrong-operand negative test must be linked");
    Context.expect(!rcc::MachineVerifier::verify(*WrongOperands, *InstructionSet),
                   "descriptor operand-count mismatch must fail verification");
  }

  auto DetachedChild = createFunction(Context, "detached_child", 4);
  if (DetachedChild) {
    auto Block = DetachedChild->createBasicBlock();
    auto Instruction = DetachedChild->createInstruction(rcc::Opcode::fromValue(NopOpcode));
    Context.expect(Block && Instruction && (*Block)->appendInstruction(**Instruction).has_value(),
                   "detached block negative test must contain an instruction");
    Context.expect(!rcc::MachineVerifier::verify(*DetachedChild, *InstructionSet),
                   "detached block retaining instructions must fail verification");
  }

  auto DetachedCFG = createFunction(Context, "detached_cfg", 5);
  rcc::MachineBasicBlock *Linked = DetachedCFG ? createBlock(Context, *DetachedCFG) : nullptr;
  if (DetachedCFG && Linked != nullptr) {
    auto Detached = DetachedCFG->createBasicBlock();
    Context.expect(Detached && DetachedCFG->cfgUpdater()
                                   .addSuccessor(**Detached, *Linked, rcc::BranchProbability::one())
                                   .has_value(),
                   "detached CFG negative test edge must be created");
    Context.expect(!rcc::MachineVerifier::verify(*DetachedCFG, *InstructionSet),
                   "detached block retaining CFG state must fail verification");
  }
}

void testPropertyFailures(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  if (!InstructionSet)
    return;
  auto Type = rcc::LLT::makeInteger(64);

  auto SSA = createFunction(Context, "ssa", 6);
  rcc::MachineBasicBlock *SSABlock = SSA ? createBlock(Context, *SSA) : nullptr;
  if (SSA && SSABlock != nullptr) {
    auto Value = SSA->registerInfo().createVirtualRegister(*Type);
    rcc::MachineIRBuilder Builder(*SSA, *InstructionSet);
    Context.expect(Value && Builder.setInsertPoint(*SSABlock).has_value(),
                   "SSA negative test setup must succeed");
    for (int Index = 0; Index != 2 && Value; ++Index) {
      auto Def = Builder.buildInstr(rcc::Opcode::fromValue(GenericDefOpcode));
      Context.expect(Def && Def->addDef(*Value).has_value() && Def->build(),
                     "SSA negative test definition must build");
    }
    SSA->properties().set(rcc::MachineFunctionProperty::IsSSA);
    Context.expect(!rcc::MachineVerifier::verify(*SSA, *InstructionSet),
                   "IsSSA must reject a multiply-defined virtual register");
  }

  auto PHI = createFunction(Context, "phi", 7);
  rcc::MachineBasicBlock *PHIBlock = PHI ? createBlock(Context, *PHI) : nullptr;
  if (PHI && PHIBlock != nullptr) {
    auto Value = PHI->registerInfo().createVirtualRegister(*Type);
    rcc::MachineIRBuilder Builder(*PHI, *InstructionSet);
    Context.expect(Value && Builder.setInsertPoint(*PHIBlock).has_value(),
                   "PHI negative test setup must succeed");
    auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(PhiOpcode));
    Context.expect(Pending && Pending->addDef(*Value).has_value() && Pending->build(),
                   "PHI negative test instruction must build");
    PHI->properties().set(rcc::MachineFunctionProperty::NoPHIs);
    Context.expect(!rcc::MachineVerifier::verify(*PHI, *InstructionSet),
                   "NoPHIs must reject a PHI instruction");
  }

  auto NoVRegs = createFunction(Context, "no_vregs", 8);
  if (NoVRegs) {
    rcc::MachineBasicBlock *Block = createBlock(Context, *NoVRegs);
    auto Value = NoVRegs->registerInfo().createVirtualRegister(*Type);
    rcc::MachineIRBuilder Builder(*NoVRegs, *InstructionSet);
    Context.expect(Block != nullptr && Builder.setInsertPoint(*Block).has_value(),
                   "NoVRegs negative test setup must succeed");
    auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(GenericDefOpcode));
    Context.expect(Value && Pending && Pending->addDef(*Value).has_value(),
                   "NoVRegs linked history must be assembled");
    if (!Value || !Pending || Block == nullptr)
      return;
    auto Built = Pending->build();
    NoVRegs->properties().set(rcc::MachineFunctionProperty::NoVRegs);
    Context.expect(Value && Built && !rcc::MachineVerifier::verify(*NoVRegs, *InstructionSet),
                   "NoVRegs must reject a live virtual register");
    Context.expect(Value && Built && Block != nullptr &&
                       Block->removeInstruction(**Built).has_value() &&
                       NoVRegs->registerInfo().eraseVirtualRegister(*Value).has_value() &&
                       rcc::MachineVerifier::verify(*NoVRegs, *InstructionSet).has_value(),
                   "NoVRegs must accept tombstoned def-use history");
  }

  auto Frame = createFunction(Context, "frame_index", 9);
  rcc::MachineBasicBlock *FrameBlock = Frame ? createBlock(Context, *Frame) : nullptr;
  if (Frame && FrameBlock != nullptr) {
    rcc::StackObjectDesc StackSlot;
    StackSlot.Size = 8;
    StackSlot.Align = *rcc::Alignment::create(8);
    auto FrameIndex = Frame->frameInfo().createObject(StackSlot);
    rcc::MachineIRBuilder Builder(*Frame, *InstructionSet);
    Context.expect(FrameIndex && Builder.setInsertPoint(*FrameBlock).has_value(),
                   "frame-index negative test setup must succeed");
    if (!FrameIndex)
      return;
    auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(FrameOpcode));
    Context.expect(Pending && Pending->addFrameIndex(*FrameIndex).has_value() && Pending->build(),
                   "frame-index negative test instruction must build");
    Frame->properties().set(rcc::MachineFunctionProperty::NoFrameIndices);
    Context.expect(!rcc::MachineVerifier::verify(*Frame, *InstructionSet),
                   "NoFrameIndices must reject a frame-index operand");
  }

  auto Pseudo = createFunction(Context, "pseudo", 10);
  rcc::MachineBasicBlock *PseudoBlock = Pseudo ? createBlock(Context, *Pseudo) : nullptr;
  if (Pseudo && PseudoBlock != nullptr) {
    rcc::MachineIRBuilder Builder(*Pseudo, *InstructionSet);
    Context.expect(Builder.setInsertPoint(*PseudoBlock).has_value() &&
                       buildNoOperand(Context, Builder, PseudoOpcode) != nullptr,
                   "pseudo negative test instruction must build");
    Pseudo->properties().set(rcc::MachineFunctionProperty::NoExecutablePseudos);
    Context.expect(!rcc::MachineVerifier::verify(*Pseudo, *InstructionSet),
                   "NoExecutablePseudos must reject an executable pseudo");
  }
}

void testSelectedStateAndPseudoDeadline(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Type = rcc::LLT::makeInteger(64);
  if (!InstructionSet)
    return;

  auto Tied = createFunction(Context, "tied", 11);
  rcc::MachineBasicBlock *TiedBlock = Tied ? createBlock(Context, *Tied) : nullptr;
  if (Tied && TiedBlock != nullptr) {
    auto Def = Tied->registerInfo().createVirtualRegister(*Type);
    auto Use = Tied->registerInfo().createVirtualRegister(*Type);
    if (!Def || !Use)
      return;
    for (const rcc::Register Reg : {*Def, *Use}) {
      Context.expect(Tied->registerInfo()
                             .setRegisterBank(Reg, rcc::RegisterBankID::fromValue(1))
                             .has_value() &&
                         Tied->registerInfo()
                             .setRegisterClass(Reg, rcc::RegisterClassID::fromValue(1))
                             .has_value(),
                     "selected-state register constraints must be recorded");
    }

    rcc::MachineIRBuilder Builder(*Tied, *InstructionSet);
    Context.expect(Builder.setInsertPoint(*TiedBlock).has_value(),
                   "tied-state insertion point must be valid");
    auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(CopyOpcode));
    Context.expect(Pending && Pending->addDef(*Def).has_value() &&
                       Pending->addUse(*Use).has_value() && Pending->build(),
                   "tied-state instruction must build");
    for (const rcc::MachineFunctionProperty Property :
         {rcc::MachineFunctionProperty::Legalized, rcc::MachineFunctionProperty::RegBankSelected,
          rcc::MachineFunctionProperty::Selected, rcc::MachineFunctionProperty::TiedOpsRewritten}) {
      Tied->properties().set(Property);
    }
    Context.expect(!rcc::MachineVerifier::verify(*Tied, *InstructionSet),
                   "TiedOpsRewritten must reject unequal tied registers");
    rcc::MachineInstr *Instruction = TiedBlock->firstInstruction();
    Context.expect(Instruction != nullptr &&
                       Instruction->operandAt(1)->setRegister(*Def).has_value() &&
                       rcc::MachineVerifier::verify(*Tied, *InstructionSet).has_value(),
                   "equal tied registers must satisfy TiedOpsRewritten");
  }

  auto Pseudo = createFunction(Context, "pseudo_deadline", 12);
  rcc::MachineBasicBlock *Block = Pseudo ? createBlock(Context, *Pseudo) : nullptr;
  if (Pseudo && Block != nullptr) {
    rcc::MachineIRBuilder Builder(*Pseudo, *InstructionSet);
    Context.expect(Builder.setInsertPoint(*Block).has_value() &&
                       buildNoOperand(Context, Builder, PseudoOpcode) != nullptr,
                   "deadline pseudo must build");
    Context.expect(
        rcc::MachineVerifier::verify(*Pseudo, *InstructionSet,
                                     rcc::MachineVerifierOptions{rcc::MachineVerifierMode::Generic,
                                                                 rcc::PipelineStage::Selected})
            .has_value(),
        "pseudo may remain at its declared deadline");
    Context.expect(!rcc::MachineVerifier::verify(
                       *Pseudo, *InstructionSet,
                       rcc::MachineVerifierOptions{rcc::MachineVerifierMode::Generic,
                                                   rcc::PipelineStage::RegisterAllocated}),
                   "pseudo must not survive beyond its declared deadline");
  }

  auto Dependency = createFunction(Context, "property_dependency", 13);
  if (Dependency) {
    Dependency->properties().set(rcc::MachineFunctionProperty::Selected);
    Context.expect(!rcc::MachineVerifier::verify(*Dependency, *InstructionSet),
                   "Selected must require prior pipeline properties");
    Dependency->properties().clear(rcc::MachineFunctionProperty::Selected);
    Dependency->properties().set(rcc::MachineFunctionProperty::FrameFinalized);
    Context.expect(!rcc::MachineVerifier::verify(*Dependency, *InstructionSet),
                   "FrameFinalized must require NoVRegs and NoFrameIndices");
  }
}

void testFrameStateFailures(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Function = createFunction(Context, "frame_references", 14);
  rcc::MachineBasicBlock *Block = Function ? createBlock(Context, *Function) : nullptr;
  if (!InstructionSet || !Function || Block == nullptr)
    return;

  rcc::MachineIRBuilder Builder(*Function, *InstructionSet);
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "frame reference insertion point must be valid");
  auto Unknown = Builder.buildInstr(rcc::Opcode::fromValue(FrameOpcode));
  Context.expect(Unknown && Unknown->addFrameIndex(rcc::FrameIndex::fromValue(99)).has_value() &&
                     !Unknown->build() && Function->allInstructions().empty(),
                 "builder must reject an unknown frame index before instruction allocation");

  rcc::StackObjectDesc StackSlot;
  StackSlot.Size = 8;
  StackSlot.Align = *rcc::Alignment::create(8);
  auto FrameIndex = Function->frameInfo().createObject(StackSlot);
  if (!FrameIndex)
    return;
  auto Live = Builder.buildInstr(rcc::Opcode::fromValue(FrameOpcode));
  Context.expect(Live && Live->addFrameIndex(*FrameIndex).has_value() && Live->build(),
                 "known live frame index must build");
  Context.expect(Function->frameInfo().markObjectDead(*FrameIndex).has_value() &&
                     !rcc::MachineVerifier::verify(*Function, *InstructionSet),
                 "verifier must reject an instruction that references a dead stack object");

  auto Lifetime = createFunction(Context, "frame_lifetime", 15);
  if (!Lifetime)
    return;
  rcc::StackObjectDesc LifetimeSlot;
  LifetimeSlot.Size = 8;
  LifetimeSlot.Align = *rcc::Alignment::create(8);
  LifetimeSlot.Lifetime.push_back(
      {rcc::MachineInstrStableID::fromValue(1), rcc::MachineInstrStableID::fromValue(2)});
  Context.expect(Lifetime->frameInfo().createObject(LifetimeSlot).has_value() &&
                     !rcc::MachineVerifier::verify(*Lifetime, *InstructionSet),
                 "verifier must reject stack lifetimes that reference unknown instructions");
}

void testTableReferenceFailures(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto ConstantFunction = createFunction(Context, "constant_reference", 16);
  rcc::MachineBasicBlock *ConstantBlock =
      ConstantFunction ? createBlock(Context, *ConstantFunction) : nullptr;
  if (!InstructionSet || !ConstantFunction || ConstantBlock == nullptr)
    return;

  rcc::MachineConstantPoolEntryDesc ConstantDescription;
  ConstantDescription.Value =
      rcc::GenericConstantData{"i32", std::vector<std::byte>(4, std::byte{1})};
  ConstantDescription.Size = 4;
  ConstantDescription.Align = *rcc::Alignment::create(4);
  auto ConstantIndex = ConstantFunction->constantPool().addEntry(std::move(ConstantDescription));
  rcc::MachineIRBuilder ConstantBuilder(*ConstantFunction, *InstructionSet);
  Context.expect(ConstantIndex && ConstantBuilder.setInsertPoint(*ConstantBlock).has_value(),
                 "constant-pool verifier setup must succeed");
  if (!ConstantIndex)
    return;
  auto ConstantInstruction = ConstantBuilder.buildInstr(rcc::Opcode::fromValue(ConstantPoolOpcode));
  Context.expect(ConstantInstruction &&
                     ConstantInstruction->addConstantPool(*ConstantIndex).has_value() &&
                     ConstantInstruction->build() &&
                     ConstantFunction->constantPool().eraseEntry(*ConstantIndex).has_value() &&
                     !rcc::MachineVerifier::verify(*ConstantFunction, *InstructionSet),
                 "verifier must reject a tombstoned constant-pool reference");

  auto JumpFunction = createFunction(Context, "jump_reference", 17);
  rcc::MachineBasicBlock *JumpBlock = JumpFunction ? createBlock(Context, *JumpFunction) : nullptr;
  if (!JumpFunction || JumpBlock == nullptr)
    return;
  rcc::MachineJumpTableDesc JumpDescription;
  JumpDescription.Targets = {JumpBlock};
  auto JumpIndex = JumpFunction->jumpTableInfo().createTable(std::move(JumpDescription));
  rcc::MachineIRBuilder JumpBuilder(*JumpFunction, *InstructionSet);
  Context.expect(JumpIndex && JumpBuilder.setInsertPoint(*JumpBlock).has_value(),
                 "jump-table verifier setup must succeed");
  if (!JumpIndex)
    return;
  auto JumpInstruction = JumpBuilder.buildInstr(rcc::Opcode::fromValue(JumpTableOpcode));
  Context.expect(JumpInstruction && JumpInstruction->addJumpTable(*JumpIndex).has_value() &&
                     JumpInstruction->build() &&
                     JumpFunction->jumpTableInfo().eraseTable(*JumpIndex).has_value() &&
                     !rcc::MachineVerifier::verify(*JumpFunction, *InstructionSet),
                 "verifier must reject a tombstoned jump-table reference");
}

void testCompilationStatusReferences(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Unknown = createFunction(Context, "unknown_failure_instruction", 18);
  if (!InstructionSet || !Unknown)
    return;

  rcc::MachineCompilationFailure UnknownFailure;
  UnknownFailure.Diagnostic.Code = rcc::ErrorCode::Unsupported;
  UnknownFailure.Diagnostic.Message = "selection failed";
  UnknownFailure.FailureInstruction = rcc::MachineInstrStableID::fromValue(99);
  UnknownFailure.FailureStage = rcc::PipelineStage::Selected;
  Context.expect(
      Unknown->compilationStatus()
              .fail(rcc::MachineCompilationState::SelectionFailed, std::move(UnknownFailure))
              .has_value() &&
          !rcc::MachineVerifier::verify(*Unknown, *InstructionSet),
      "verifier must reject compilation status that references an unknown instruction");

  auto Live = createFunction(Context, "live_failure_instruction", 19);
  rcc::MachineBasicBlock *Block = Live ? createBlock(Context, *Live) : nullptr;
  if (!Live || Block == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Live, *InstructionSet);
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "live failure instruction insertion point must be valid");
  rcc::MachineInstr *Instruction = buildNoOperand(Context, Builder, NopOpcode);
  if (Instruction == nullptr)
    return;

  rcc::MachineCompilationFailure LiveFailure;
  LiveFailure.Diagnostic.Code = rcc::ErrorCode::Unsupported;
  LiveFailure.Diagnostic.Message = "finalization failed";
  LiveFailure.FailureInstruction = Instruction->stableID();
  LiveFailure.FailureStage = rcc::PipelineStage::PostRAFinalized;
  Context.expect(
      Live->compilationStatus()
              .fail(rcc::MachineCompilationState::FinalizationFailed, std::move(LiveFailure))
              .has_value() &&
          rcc::MachineVerifier::verify(*Live, *InstructionSet).has_value(),
      "verifier must accept a structurally valid failed function");
}

} // namespace

int main() {
  TestContext Context;
  testValidFunctionAndModes(Context);
  testReachableStructuralFailures(Context);
  testPropertyFailures(Context);
  testSelectedStateAndPseudoDeadline(Context);
  testFrameStateFailures(Context);
  testTableReferenceFailures(Context);
  testCompilationStatusReferences(Context);
  return Context.result();
}
