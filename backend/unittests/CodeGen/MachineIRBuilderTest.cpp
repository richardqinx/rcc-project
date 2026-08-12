// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineIRBuilder.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t CopyOpcode = 1;
constexpr std::uint32_t AddOpcode = 2;
constexpr std::uint32_t LoadOpcode = 3;
constexpr std::uint32_t NopOpcode = 4;
constexpr std::uint32_t BranchOpcode = 5;
constexpr std::uint32_t DynamicImplicitOpcode = 6;
constexpr std::uint32_t ConstantPoolOpcode = 7;
constexpr std::uint32_t JumpTableOpcode = 8;

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

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  auto Copy = instructionDescriptor(CopyOpcode, "TEST_COPY");
  auto CopyDef = registerDescriptor(rcc::RegisterOperandRole::Def);
  auto CopyUse = registerDescriptor(rcc::RegisterOperandRole::Use);
  CopyDef.DefaultTiedOperand = 1;
  CopyUse.DefaultTiedOperand = 0;
  Copy.Operands.push_back(CopyDef);
  Copy.Operands.push_back(CopyUse);
  Copy.NumExplicitDefs = 1;

  auto Add = instructionDescriptor(AddOpcode, "TEST_ADD");
  Add.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Def));
  Add.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Use));
  rcc::OperandDescriptor RegisterOrImmediate;
  RegisterOrImmediate.Kinds = {
      rcc::MachineOperand::Kind::Register,
      rcc::MachineOperand::Kind::Immediate,
  };
  RegisterOrImmediate.RegisterRole = rcc::RegisterOperandRole::Use;
  Add.Operands.push_back(RegisterOrImmediate);
  Add.NumExplicitDefs = 1;
  Add.ImplicitDefs.push_back(
      rcc::ImplicitRegisterDescriptor{*rcc::PhysicalRegister::create(1), false, true});
  Add.ImplicitUses.push_back(rcc::ImplicitRegisterDescriptor{*rcc::PhysicalRegister::create(2)});

  auto Load = instructionDescriptor(LoadOpcode, "TEST_LOAD");
  Load.Flags.set(rcc::InstructionFlag::MayLoad);
  Load.RequiresMemoryOperands = true;

  auto Nop = instructionDescriptor(NopOpcode, "TEST_NOP");

  auto Branch = instructionDescriptor(BranchOpcode, "TEST_BRANCH");
  rcc::OperandDescriptor Block;
  Block.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
  Branch.Operands.push_back(Block);
  Branch.Flags.set(rcc::InstructionFlag::Branch);
  Branch.Flags.set(rcc::InstructionFlag::Terminator);

  auto Dynamic = instructionDescriptor(DynamicImplicitOpcode, "TEST_DYNAMIC_IMPLICIT");
  Dynamic.AllowExtraImplicitOperands = true;

  auto ConstantPool = instructionDescriptor(ConstantPoolOpcode, "TEST_CONSTANT_POOL");
  rcc::OperandDescriptor ConstantPoolIndex;
  ConstantPoolIndex.Kinds = {rcc::MachineOperand::Kind::ConstantPoolIndex};
  ConstantPool.Operands.push_back(ConstantPoolIndex);

  auto JumpTable = instructionDescriptor(JumpTableOpcode, "TEST_JUMP_TABLE");
  rcc::OperandDescriptor JumpTableIndex;
  JumpTableIndex.Kinds = {rcc::MachineOperand::Kind::JumpTableIndex};
  JumpTable.Operands.push_back(JumpTableIndex);

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Copy));
  Descriptors.push_back(std::move(Add));
  Descriptors.push_back(std::move(Load));
  Descriptors.push_back(std::move(Nop));
  Descriptors.push_back(std::move(Branch));
  Descriptors.push_back(std::move(Dynamic));
  Descriptors.push_back(std::move(ConstantPool));
  Descriptors.push_back(std::move(JumpTable));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
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

[[nodiscard]] rcc::MachineMemOperandDesc memoryDescription(rcc::MemoryAccessFlag Access) {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.Base = rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0));
  return Description;
}

[[nodiscard]] rcc::MachineInstr *buildNop(TestContext &Context, rcc::MachineIRBuilder &Builder) {
  auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(Pending.has_value(), "NOP builder creation must succeed");
  if (!Pending)
    return nullptr;
  auto Instruction = Pending->build();
  Context.expect(Instruction.has_value(), "NOP construction must succeed");
  if (!Instruction)
    return nullptr;
  return *Instruction;
}

void testTransactionalBuild(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, "transactional", 1);
  Context.expect(Target.has_value(), "builder target description must be valid");
  if (!Target || !Function)
    return;
  rcc::MachineBasicBlock *Block = createBlock(Context, *Function);
  if (Block == nullptr)
    return;

  auto Type = rcc::LLT::makeInteger(64);
  auto Destination = Function->registerInfo().createVirtualRegister(*Type);
  auto Source = Function->registerInfo().createVirtualRegister(*Type);
  Context.expect(Destination && Source, "builder virtual registers must be allocated");
  if (!Destination || !Source)
    return;

  rcc::MachineIRBuilder Builder(*Function, *Target);
  Context.expect(!Builder.buildInstr(rcc::Opcode::fromValue(CopyOpcode)),
                 "an unset insertion point must reject instruction construction");
  Context.expect(Function->allInstructions().empty(),
                 "rejected builder setup must not allocate an instruction");
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "a linked block must be a valid insertion point");
  Context.expect(!Builder.buildInstr(rcc::Opcode::fromValue(99)),
                 "an unregistered opcode must be rejected");

  auto Incomplete = Builder.buildInstr(rcc::Opcode::fromValue(CopyOpcode));
  Context.expect(Incomplete.has_value() && Incomplete->addDef(*Destination).has_value(),
                 "incomplete instruction setup must accept its first operand");
  if (!Incomplete)
    return;
  Context.expect(!Incomplete->tieOperands(0, 1) && !Incomplete->setSourceLocation({"", 0, 0, 0}) &&
                     !Incomplete->setDebugLocation({}) &&
                     !Incomplete->setOrigin(rcc::OriginID::fromValue(0)),
                 "invalid pending metadata and tie indices must be rejected");
  Context.expect(!Incomplete->build(), "descriptor validation must reject a missing operand");
  Context.expect(Function->allInstructions().empty() && Block->instructionCount() == 0,
                 "failed validation must not allocate or publish an instruction");

  auto Complete = Builder.buildInstr(rcc::Opcode::fromValue(CopyOpcode));
  Context.expect(Complete.has_value(), "complete instruction builder must be created");
  if (!Complete)
    return;
  Context.expect(Complete->addDef(*Destination).has_value() &&
                     Complete->addUse(*Source).has_value(),
                 "complete instruction operands must be accepted");
  Context.expect(Complete->tieOperands(0, 1).has_value(),
                 "an explicit tie matching the descriptor default must be idempotent");
  Context.expect(Complete->setFlags({rcc::MachineInstrFlag::FrameSetup}).has_value(),
                 "valid instruction flags must be accepted");
  auto InlineDebugLocation = rcc::MachineDILocation::create({
      {
          .Line = 11,
          .Column = 2,
          .Scope = rcc::MachineMetadataReference::embeddedIR(3),
          .IsImplicitCode = false,
          .AtomGroup = 0,
          .AtomRank = 0,
      },
  });
  Context.expect(InlineDebugLocation.has_value(),
                 "builder debug location setup must be structurally valid");
  if (!InlineDebugLocation)
    return;
  const rcc::MachineDebugLocation DebugLocation =
      rcc::MachineDebugLocation::inlineLocation(*InlineDebugLocation);
  Context.expect(
      Complete->setSourceLocation(rcc::SourceLocation{"builder.mir", 7, 3, 1}).has_value() &&
          Complete->setDebugLocation(DebugLocation).has_value() &&
          Complete->setOrigin(rcc::OriginID::fromValue(19)).has_value(),
      "valid parser source, debug location, and origin metadata must be accepted");
  auto Built = Complete->build();
  Context.expect(Built.has_value(), "valid pending instruction must build");
  if (!Built)
    return;

  rcc::MachineInstr *Instruction = *Built;
  Context.expect(Instruction->stableID().value() == 1,
                 "failed builds must not consume instruction StableIDs");
  Context.expect(Instruction->parent() == Block && Block->firstInstruction() == Instruction,
                 "successful build must publish at the insertion point");
  Context.expect(Instruction->operandAt(0)->tiedOperandIndex() == 1 &&
                     Instruction->operandAt(1)->tiedOperandIndex() == 0,
                 "descriptor-default tie must be materialized bidirectionally");
  Context.expect(Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup) &&
                     Instruction->sourceLocation() == rcc::SourceLocation{"builder.mir", 7, 3, 1} &&
                     Instruction->debugLocation() == DebugLocation &&
                     Instruction->origin() == rcc::OriginID::fromValue(19),
                 "builder metadata must be preserved on the published instruction");
  Context.expect(Target->validateInstruction(*Instruction, Function->registerInfo()).has_value(),
                 "builder output must satisfy the shared instruction descriptor");
  Context.expect(!Complete->build(), "a successful pending builder must be single-use");
  Context.expect(!Complete->setFlags({}) && !Complete->clearSourceLocation() &&
                     !Complete->clearDebugLocation() && !Complete->clearOrigin(),
                 "all pending-builder mutators must reject use after publication");
  Context.expect(Function->registerInfo().verifyDefUseChains().has_value(),
                 "published builder operands must enter exact def-use chains");

  auto MoveSource = Builder.buildInstr(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(MoveSource.has_value(), "movable pending builder must be created");
  if (!MoveSource)
    return;
  rcc::MachineInstrBuilder MoveDestination(std::move(*MoveSource));
  Context.expect(!MoveSource->build(), "a moved-from pending builder must be consumed");
  auto MovedResult = MoveDestination.build();
  Context.expect(MovedResult && (*MovedResult)->stableID().value() == 2,
                 "a moved-to pending builder must retain its construction state");
}

void testInsertionOrdering(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, "ordering", 2);
  if (!Target || !Function)
    return;
  rcc::MachineBasicBlock *Block = createBlock(Context, *Function);
  if (Block == nullptr)
    return;

  rcc::MachineIRBuilder Builder(*Function, *Target);
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "ordering builder insertion point must be valid");
  rcc::MachineInstr *First = buildNop(Context, Builder);
  rcc::MachineInstr *Tail = buildNop(Context, Builder);
  if (First == nullptr || Tail == nullptr)
    return;

  Context.expect(Builder.setInsertPointAfter(*First).has_value(),
                 "after-instruction insertion point must be accepted");
  rcc::MachineInstr *MiddleA = buildNop(Context, Builder);
  rcc::MachineInstr *MiddleB = buildNop(Context, Builder);
  Context.expect(MiddleA != nullptr && MiddleB != nullptr && First->next() == MiddleA &&
                     MiddleA->next() == MiddleB && MiddleB->next() == Tail,
                 "repeated insertion after a cursor must preserve build order");

  Context.expect(Builder.setInsertPointBefore(*First).has_value(),
                 "before-instruction insertion point must be accepted");
  rcc::MachineInstr *PrefixA = buildNop(Context, Builder);
  rcc::MachineInstr *PrefixB = buildNop(Context, Builder);
  Context.expect(PrefixA != nullptr && PrefixB != nullptr && Block->firstInstruction() == PrefixA &&
                     PrefixA->next() == PrefixB && PrefixB->next() == First,
                 "repeated insertion before a cursor must preserve build order");
}

void testImplicitAndMemoryValidation(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, "implicit_memory", 3);
  auto OtherFunction = createFunction(Context, "foreign", 4);
  if (!Target || !Function || !OtherFunction)
    return;
  rcc::MachineBasicBlock *Block = createBlock(Context, *Function);
  rcc::MachineBasicBlock *OtherBlock = createBlock(Context, *OtherFunction);
  if (Block == nullptr || OtherBlock == nullptr)
    return;

  auto Type = rcc::LLT::makeInteger(64);
  auto Destination = Function->registerInfo().createVirtualRegister(*Type);
  auto Source = Function->registerInfo().createVirtualRegister(*Type);
  Context.expect(Destination && Source, "implicit test virtual registers must be allocated");
  if (!Destination || !Source)
    return;

  rcc::MachineIRBuilder Builder(*Function, *Target);
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "implicit test insertion point must be accepted");

  auto Add = Builder.buildInstr(rcc::Opcode::fromValue(AddOpcode));
  Context.expect(Add && Add->addDef(*Destination).has_value() && Add->addUse(*Source).has_value() &&
                     Add->addImmediate(7).has_value(),
                 "ADD operands must be accepted by the pending builder");
  if (!Add)
    return;
  auto BuiltAdd = Add->build();
  Context.expect(BuiltAdd.has_value(), "ADD with default implicits must build");
  if (!BuiltAdd)
    return;
  Context.expect(
      (*BuiltAdd)->operandCount() == 5 && (*BuiltAdd)->operandAt(3)->isImplicit() &&
          (*BuiltAdd)->operandAt(3)->registerFlags().has(rcc::RegisterOperandFlag::Renamable) &&
          (*BuiltAdd)->operandAt(4)->isImplicit(),
      "default implicit defs and uses must follow explicit operands");
  Context.expect(Target->validateInstruction(**BuiltAdd, Function->registerInfo()).has_value(),
                 "default implicit builder output must pass shared validation");

  const std::size_t BeforeRejectedImplicit = Function->allInstructions().size();
  auto ExtraImplicit = Builder.buildInstr(rcc::Opcode::fromValue(AddOpcode));
  Context.expect(
      ExtraImplicit && ExtraImplicit->addDef(*Destination).has_value() &&
          ExtraImplicit->addUse(*Source).has_value() &&
          ExtraImplicit->addImmediate(1).has_value() &&
          ExtraImplicit
              ->addImplicitUse(rcc::Register::fromPhysical(*rcc::PhysicalRegister::create(3)))
              .has_value(),
      "dynamic implicit negative test must be assembled");
  Context.expect(ExtraImplicit && !ExtraImplicit->build() &&
                     Function->allInstructions().size() == BeforeRejectedImplicit,
                 "descriptor-rejected dynamic implicits must not allocate instructions");

  auto Dynamic = Builder.buildInstr(rcc::Opcode::fromValue(DynamicImplicitOpcode));
  Context.expect(
      Dynamic &&
          Dynamic->addImplicitUse(rcc::Register::fromPhysical(*rcc::PhysicalRegister::create(3)))
              .has_value(),
      "descriptor-permitted dynamic implicit operand must be accepted");
  if (!Dynamic)
    return;
  auto BuiltDynamic = Dynamic->build();
  Context.expect(
      BuiltDynamic.has_value() &&
          Target->validateInstruction(**BuiltDynamic, Function->registerInfo()).has_value(),
      "dynamic implicit builder output must validate");

  const std::size_t BeforeMissingMemory = Function->allInstructions().size();
  auto MissingMemory = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  Context.expect(MissingMemory && !MissingMemory->build() &&
                     Function->allInstructions().size() == BeforeMissingMemory,
                 "missing required MMO must fail before instruction allocation");

  rcc::StackObjectDesc StackSlot;
  StackSlot.Size = 8;
  StackSlot.Align = *rcc::Alignment::create(8);
  auto FrameIndex = Function->frameInfo().createObject(StackSlot);
  auto ForeignFrameIndex = OtherFunction->frameInfo().createObject(StackSlot);
  Context.expect(FrameIndex && ForeignFrameIndex,
                 "memory validation stack objects must be created");
  if (!FrameIndex || !ForeignFrameIndex)
    return;

  auto LoadMemory = Function->createMemOperand(memoryDescription(rcc::MemoryAccessFlag::Load));
  auto StoreMemory = Function->createMemOperand(memoryDescription(rcc::MemoryAccessFlag::Store));
  auto ForeignMemory =
      OtherFunction->createMemOperand(memoryDescription(rcc::MemoryAccessFlag::Load));
  Context.expect(LoadMemory && StoreMemory && ForeignMemory,
                 "memory validation operands must be created");
  if (!LoadMemory || !StoreMemory || !ForeignMemory)
    return;

  const std::size_t BeforeUnknownTableReferences = Function->allInstructions().size();
  auto UnknownConstant = Builder.buildInstr(rcc::Opcode::fromValue(ConstantPoolOpcode));
  auto UnknownJumpTable = Builder.buildInstr(rcc::Opcode::fromValue(JumpTableOpcode));
  Context.expect(
      UnknownConstant &&
          UnknownConstant->addConstantPool(rcc::ConstantPoolIndex::fromValue(99)).has_value() &&
          !UnknownConstant->build() && UnknownJumpTable &&
          UnknownJumpTable->addJumpTable(rcc::JumpTableIndex::fromValue(99)).has_value() &&
          !UnknownJumpTable->build() &&
          Function->allInstructions().size() == BeforeUnknownTableReferences,
      "builder must reject unknown constant-pool and jump-table indices before allocation");

  rcc::MachineConstantPoolEntryDesc ConstantDescription;
  auto Integer = rcc::APInt::fromUnsigned(64, 0x2a2a2a2a2a2a2a2aULL);
  ConstantDescription.Value = std::move(*rcc::GenericConstantData::integer(std::move(*Integer)));
  ConstantDescription.Size = 8;
  ConstantDescription.Align = *rcc::Alignment::create(8);
  auto ConstantIndex = Function->constantPool().addEntry(std::move(ConstantDescription));
  rcc::MachineJumpTableDesc JumpDescription;
  JumpDescription.Targets = {Block};
  auto JumpIndex = Function->jumpTableInfo().createTable(std::move(JumpDescription));
  Context.expect(ConstantIndex && JumpIndex, "builder table references must have owned entries");
  if (!ConstantIndex || !JumpIndex)
    return;

  auto ConstantInstruction = Builder.buildInstr(rcc::Opcode::fromValue(ConstantPoolOpcode));
  auto JumpInstruction = Builder.buildInstr(rcc::Opcode::fromValue(JumpTableOpcode));
  Context.expect(
      ConstantInstruction && ConstantInstruction->addConstantPool(*ConstantIndex, 4).has_value() &&
          ConstantInstruction->build() && JumpInstruction &&
          JumpInstruction->addJumpTable(*JumpIndex).has_value() && JumpInstruction->build(),
      "builder must publish valid owned table references");

  auto ConstantMemoryDescription = memoryDescription(rcc::MemoryAccessFlag::Load);
  ConstantMemoryDescription.Base = rcc::MemoryBase::constantPool(*ConstantIndex);
  auto JumpMemoryDescription = memoryDescription(rcc::MemoryAccessFlag::Load);
  JumpMemoryDescription.Base = rcc::MemoryBase::jumpTable(*JumpIndex);
  auto ConstantMemory = Function->createMemOperand(std::move(ConstantMemoryDescription));
  auto JumpMemory = Function->createMemOperand(std::move(JumpMemoryDescription));
  auto ConstantLoad = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  auto JumpLoad = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  Context.expect(ConstantMemory && JumpMemory && ConstantLoad &&
                     ConstantLoad->addMemOperand(**ConstantMemory).has_value() &&
                     ConstantLoad->build() && JumpLoad &&
                     JumpLoad->addMemOperand(**JumpMemory).has_value() && JumpLoad->build(),
                 "builder must validate constant-pool and jump-table MMO bases");

  auto Load = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  Context.expect(Load && Load->addMemOperand(**LoadMemory).has_value(),
                 "function-owned load MMO must be accepted");
  if (!Load)
    return;
  auto BuiltLoad = Load->build();
  Context.expect(BuiltLoad.has_value() &&
                     Target->validateInstruction(**BuiltLoad, Function->registerInfo()).has_value(),
                 "matching load MMO must build and validate");

  const std::size_t BeforeStoreMismatch = Function->allInstructions().size();
  auto StoreMismatch = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  Context.expect(StoreMismatch && StoreMismatch->addMemOperand(**StoreMemory).has_value() &&
                     !StoreMismatch->build() &&
                     Function->allInstructions().size() == BeforeStoreMismatch,
                 "MMO access mismatch must fail before instruction allocation");
  auto ForeignMMO = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  Context.expect(ForeignMMO && !ForeignMMO->addMemOperand(**ForeignMemory),
                 "foreign-function MMO must be rejected immediately");

  const std::size_t BeforeForeignBlock = Function->allInstructions().size();
  auto ForeignBranch = Builder.buildInstr(rcc::Opcode::fromValue(BranchOpcode));
  Context.expect(ForeignBranch && ForeignBranch->addBasicBlock(*OtherBlock).has_value() &&
                     !ForeignBranch->build() &&
                     Function->allInstructions().size() == BeforeForeignBlock,
                 "foreign basic block operand must fail before instruction allocation");
}

void testStaleCursorsAndOwnedOperands(TestContext &Context) {
  auto Target = createTarget();
  auto Function = createFunction(Context, "ownership", 5);
  if (!Target || !Function)
    return;
  rcc::MachineBasicBlock *Block = createBlock(Context, *Function);
  if (Block == nullptr)
    return;

  auto Type = rcc::LLT::makeInteger(64);
  auto Destination = Function->registerInfo().createVirtualRegister(*Type);
  auto Source = Function->registerInfo().createVirtualRegister(*Type);
  if (!Destination || !Source)
    return;

  rcc::MachineIRBuilder Builder(*Function, *Target);
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "ownership test insertion point must be valid");
  auto Copy = Builder.buildInstr(rcc::Opcode::fromValue(CopyOpcode));
  Context.expect(Copy && Copy->addDef(*Destination).has_value() &&
                     Copy->addUse(*Source).has_value(),
                 "ownership source instruction must be assembled");
  if (!Copy)
    return;
  auto BuiltCopy = Copy->build();
  if (!BuiltCopy)
    return;

  auto DetachedTarget = Function->createInstruction(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(DetachedTarget.has_value(), "detached target instruction must be allocated");
  if (!DetachedTarget)
    return;
  rcc::MachineOperand *OwnedOperand = (*BuiltCopy)->operandAt(0);
  Context.expect(
      OwnedOperand != nullptr && !(*DetachedTarget)->addOperand(std::move(*OwnedOperand)) &&
          OwnedOperand->parent() == *BuiltCopy && OwnedOperand->registerValue() == *Destination,
      "controlled instruction API must reject an owned operand without moving it");

  auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(Pending && OwnedOperand != nullptr &&
                     !Pending->addOperand(std::move(*OwnedOperand)) &&
                     OwnedOperand->parent() == *BuiltCopy,
                 "controlled builder API must reject an owned operand without moving it");

  rcc::MachineInstr *Cursor = buildNop(Context, Builder);
  if (Cursor == nullptr)
    return;
  Context.expect(Builder.setInsertPointBefore(*Cursor).has_value(), "cursor setup must succeed");
  auto Stale = Builder.buildInstr(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(Stale.has_value(), "pending stale-cursor test must be created");
  const std::size_t BeforeStaleBuild = Function->allInstructions().size();
  Context.expect(Block->removeInstruction(*Cursor).has_value(), "cursor removal must succeed");
  Context.expect(Stale && !Stale->build() && Function->allInstructions().size() == BeforeStaleBuild,
                 "a stale cursor must fail before instruction allocation");
}

} // namespace

int main() {
  TestContext Context;
  testTransactionalBuild(Context);
  testInsertionOrdering(Context);
  testImplicitAndMemoryValidation(Context);
  testStaleCursorsAndOwnedOperands(Context);
  return Context.result();
}
