// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineCompilationStatus.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetFunctionState.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t NopOpcode = 1;
constexpr std::uint32_t DefOpcode = 2;
constexpr std::uint32_t BranchOpcode = 3;
constexpr std::uint32_t CopyOpcode = 4;
constexpr std::uint32_t FrameOpcode = 5;
constexpr std::uint32_t BlockAddressOpcode = 6;
constexpr std::uint32_t LoadOpcode = 7;
constexpr std::uint32_t ConstantPoolOpcode = 8;
constexpr std::uint32_t JumpTableOpcode = 9;

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

void appendLittleEndian(std::vector<std::byte> &Bytes, std::uint64_t Value, std::size_t Width) {
  for (std::size_t Index = 0; Index < Width; ++Index)
    Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8U)) & 0xffU));
}

[[nodiscard]] std::uint64_t readLittleEndian(std::span<const std::byte> Bytes, std::size_t Offset,
                                             std::size_t Width) {
  std::uint64_t Value = 0;
  for (std::size_t Index = 0; Index < Width; ++Index) {
    Value |= std::to_integer<std::uint64_t>(Bytes[Offset + Index]) << (Index * 8U);
  }
  return Value;
}

struct CloneTargetState final {
  rcc::MachineBlockStableID Block;
  rcc::MachineInstrStableID Instruction;
  rcc::Register TrackedRegister;
  rcc::FrameIndex Frame;
  rcc::ConstantPoolIndex Constant;
  rcc::JumpTableIndex JumpTable;

  [[nodiscard]] static constexpr rcc::TargetFunctionStateTypeID stableTypeID() noexcept {
    return rcc::TargetFunctionStateTypeID::fromValue(0x524343434c4f4e45ULL);
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 1;
  }

  [[nodiscard]] rcc::Expected<void> verify() const {
    if (!Block.isValid() || !Instruction.isValid() || TrackedRegister.isNoRegister()) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "clone target state has an invalid identity");
    }
    return {};
  }

  [[nodiscard]] rcc::Expected<CloneTargetState>
  clone(const rcc::MachineFunctionCloneContext &Context) const {
    auto MappedBlock = Context.mapBlockStableID(Block);
    if (!MappedBlock)
      return std::unexpected(MappedBlock.error());
    auto MappedInstruction = Context.mapInstructionStableID(Instruction);
    if (!MappedInstruction)
      return std::unexpected(MappedInstruction.error());
    auto MappedRegister = Context.mapRegister(TrackedRegister);
    if (!MappedRegister)
      return std::unexpected(MappedRegister.error());
    auto MappedFrame = Context.mapFrameIndex(Frame);
    if (!MappedFrame)
      return std::unexpected(MappedFrame.error());
    auto MappedConstant = Context.mapConstantPoolIndex(Constant);
    if (!MappedConstant)
      return std::unexpected(MappedConstant.error());
    auto MappedJumpTable = Context.mapJumpTableIndex(JumpTable);
    if (!MappedJumpTable)
      return std::unexpected(MappedJumpTable.error());
    return CloneTargetState{*MappedBlock, *MappedInstruction, *MappedRegister,
                            *MappedFrame, *MappedConstant,    *MappedJumpTable};
  }

  [[nodiscard]] std::string print() const {
    return "clone-target-state";
  }

  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    std::vector<std::byte> Payload;
    Payload.reserve(32);
    appendLittleEndian(Payload, Block.value(), 8);
    appendLittleEndian(Payload, Instruction.value(), 8);
    appendLittleEndian(Payload, TrackedRegister.opaqueValue(), 4);
    appendLittleEndian(Payload, Frame.value(), 4);
    appendLittleEndian(Payload, Constant.value(), 4);
    appendLittleEndian(Payload, JumpTable.value(), 4);
    return Payload;
  }

  [[nodiscard]] static rcc::Expected<CloneTargetState> decode(std::uint32_t Version,
                                                              std::span<const std::byte> Payload) {
    if (Version != schemaVersion() || Payload.size() != 32) {
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "clone target state encoding is invalid");
    }
    return CloneTargetState{
        rcc::MachineBlockStableID::fromValue(readLittleEndian(Payload, 0, 8)),
        rcc::MachineInstrStableID::fromValue(readLittleEndian(Payload, 8, 8)),
        rcc::Register(static_cast<std::uint32_t>(readLittleEndian(Payload, 16, 4))),
        rcc::FrameIndex::fromValue(static_cast<std::uint32_t>(readLittleEndian(Payload, 20, 4))),
        rcc::ConstantPoolIndex::fromValue(
            static_cast<std::uint32_t>(readLittleEndian(Payload, 24, 4))),
        rcc::JumpTableIndex::fromValue(
            static_cast<std::uint32_t>(readLittleEndian(Payload, 28, 4))),
    };
  }

  friend bool operator==(const CloneTargetState &Left, const CloneTargetState &Right) = default;
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
  auto Nop = instructionDescriptor(NopOpcode, "TEST_NOP");

  auto Def = instructionDescriptor(DefOpcode, "TEST_DEF");
  Def.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Def));
  Def.NumExplicitDefs = 1;

  auto Branch = instructionDescriptor(BranchOpcode, "TEST_BRANCH");
  rcc::OperandDescriptor Block;
  Block.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
  Branch.Operands.push_back(Block);
  Branch.Flags.set(rcc::InstructionFlag::Branch);
  Branch.Flags.set(rcc::InstructionFlag::Terminator);

  auto Copy = instructionDescriptor(CopyOpcode, "TEST_COPY");
  auto CopyDef = registerDescriptor(rcc::RegisterOperandRole::Def);
  auto CopyUse = registerDescriptor(rcc::RegisterOperandRole::Use);
  CopyDef.DefaultTiedOperand = 1;
  CopyUse.DefaultTiedOperand = 0;
  Copy.Operands.push_back(CopyDef);
  Copy.Operands.push_back(CopyUse);
  Copy.NumExplicitDefs = 1;

  auto Frame = instructionDescriptor(FrameOpcode, "TEST_FRAME");
  rcc::OperandDescriptor FrameIndex;
  FrameIndex.Kinds = {rcc::MachineOperand::Kind::FrameIndex};
  Frame.Operands.push_back(FrameIndex);

  auto BlockAddress = instructionDescriptor(BlockAddressOpcode, "TEST_BLOCK_ADDRESS");
  rcc::OperandDescriptor Address;
  Address.Kinds = {rcc::MachineOperand::Kind::BlockAddress};
  BlockAddress.Operands.push_back(Address);

  auto Load = instructionDescriptor(LoadOpcode, "TEST_LOAD");
  Load.Flags.set(rcc::InstructionFlag::MayLoad);
  Load.RequiresMemoryOperands = true;

  auto ConstantPool = instructionDescriptor(ConstantPoolOpcode, "TEST_CONSTANT_POOL");
  rcc::OperandDescriptor ConstantPoolIndex;
  ConstantPoolIndex.Kinds = {rcc::MachineOperand::Kind::ConstantPoolIndex};
  ConstantPool.Operands.push_back(ConstantPoolIndex);

  auto JumpTable = instructionDescriptor(JumpTableOpcode, "TEST_JUMP_TABLE");
  rcc::OperandDescriptor JumpTableIndex;
  JumpTableIndex.Kinds = {rcc::MachineOperand::Kind::JumpTableIndex};
  JumpTable.Operands.push_back(JumpTableIndex);

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Nop));
  Descriptors.push_back(std::move(Def));
  Descriptors.push_back(std::move(Branch));
  Descriptors.push_back(std::move(Copy));
  Descriptors.push_back(std::move(Frame));
  Descriptors.push_back(std::move(BlockAddress));
  Descriptors.push_back(std::move(Load));
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

[[nodiscard]] rcc::MachineMemOperandDesc loadDescription(rcc::MemoryBase Base) {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Dereferenceable};
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.Base = std::move(Base);
  Description.Offset = -8;
  return Description;
}

[[nodiscard]] rcc::MachineInstr *findInstruction(rcc::MachineBasicBlock &Block,
                                                 std::uint32_t OpcodeValue) {
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->opcode() == rcc::Opcode::fromValue(OpcodeValue))
      return Instruction;
  }
  return nullptr;
}

struct SourceFixture final {
  rcc::MachineBasicBlock *Entry = nullptr;
  rcc::MachineBasicBlock *Exit = nullptr;
  rcc::Register Value;
  rcc::Register CopyValue;
  rcc::FrameIndex StackSlot;
  rcc::ConstantPoolIndex Constant;
  rcc::JumpTableIndex JumpTable;
  std::vector<const rcc::MachineMemOperand *> LoadMMOs;
};

[[nodiscard]] bool populateSource(TestContext &Context, rcc::MachineFunction &Function,
                                  const rcc::TargetDescription &Target, SourceFixture &Fixture) {
  auto HistoricalBlock = Function.createBasicBlock();
  auto HistoricalInstruction = Function.createInstruction(rcc::Opcode::fromValue(NopOpcode));
  Context.expect(HistoricalBlock && HistoricalInstruction, "source arena history must be created");

  auto Type = rcc::LLT::makeInteger(64);
  auto Tombstone = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Tombstone && Function.registerInfo().eraseVirtualRegister(*Tombstone).has_value(),
                 "source vreg history must contain a tombstone");

  rcc::StackObjectDesc HistoricalFrame;
  HistoricalFrame.Size = 4;
  HistoricalFrame.Align = *rcc::Alignment::create(4);
  auto HistoricalFrameIndex = Function.frameInfo().createObject(HistoricalFrame);
  Context.expect(HistoricalFrameIndex &&
                     Function.frameInfo().eraseObject(*HistoricalFrameIndex).has_value(),
                 "source frame history must contain a tombstone");

  rcc::StackObjectDesc StackSlot;
  StackSlot.Kind = rcc::StackObjectKind::SpillSlot;
  StackSlot.Size = 8;
  StackSlot.Align = *rcc::Alignment::create(8);
  StackSlot.Offset = -16;
  auto StackSlotIndex = Function.frameInfo().createObject(StackSlot);
  Context.expect(StackSlotIndex.has_value(), "source live stack object must be created");
  if (!StackSlotIndex)
    return false;
  Fixture.StackSlot = *StackSlotIndex;

  Fixture.Entry = createBlock(Context, Function);
  Fixture.Exit = createBlock(Context, Function);
  if (Fixture.Entry == nullptr || Fixture.Exit == nullptr)
    return false;

  rcc::MachineConstantPoolEntryDesc HistoricalConstant;
  HistoricalConstant.Value =
      rcc::GenericConstantData{"i32", std::vector<std::byte>(4, std::byte{1})};
  HistoricalConstant.Size = 4;
  HistoricalConstant.Align = *rcc::Alignment::create(4);
  auto HistoricalConstantIndex = Function.constantPool().addEntry(std::move(HistoricalConstant));
  Context.expect(HistoricalConstantIndex &&
                     Function.constantPool().eraseEntry(*HistoricalConstantIndex).has_value(),
                 "source constant pool history must contain a tombstone");

  rcc::MachineConstantPoolEntryDesc Constant;
  Constant.Value = rcc::BlockAddressConstant{std::string(Function.name()), "exit", 5};
  Constant.Size = 8;
  Constant.Align = *rcc::Alignment::create(8);
  Constant.Relocation = rcc::ConstantPoolRelocationMode::Static;
  auto ConstantIndex = Function.constantPool().addEntry(std::move(Constant));
  Context.expect(ConstantIndex && Function.constantPool().freeze().has_value(),
                 "source live constant pool entry must be frozen");
  if (!ConstantIndex)
    return false;
  Fixture.Constant = *ConstantIndex;

  rcc::MachineJumpTableDesc HistoricalJumpTable;
  HistoricalJumpTable.EntrySize = 4;
  HistoricalJumpTable.Align = *rcc::Alignment::create(4);
  HistoricalJumpTable.Targets = {Fixture.Entry};
  auto HistoricalJumpTableIndex =
      Function.jumpTableInfo().createTable(std::move(HistoricalJumpTable));
  Context.expect(HistoricalJumpTableIndex &&
                     Function.jumpTableInfo().eraseTable(*HistoricalJumpTableIndex).has_value(),
                 "source jump table history must contain a tombstone");

  rcc::MachineJumpTableDesc JumpTable;
  JumpTable.Encoding = rcc::JumpTableEntryEncoding::RelativeToTableBase;
  JumpTable.EntrySize = 4;
  JumpTable.Align = *rcc::Alignment::create(4);
  JumpTable.Targets = {Fixture.Exit, Fixture.Entry, Fixture.Exit};
  auto JumpTableIndex = Function.jumpTableInfo().createTable(std::move(JumpTable));
  Context.expect(JumpTableIndex && Function.jumpTableInfo().freeze().has_value(),
                 "source live jump table must be frozen");
  if (!JumpTableIndex)
    return false;
  Fixture.JumpTable = *JumpTableIndex;

  auto Value = Function.registerInfo().createVirtualRegister(*Type);
  auto CopyValue = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Value && CopyValue, "source live vregs must be created");
  if (!Value || !CopyValue)
    return false;
  Fixture.Value = *Value;
  Fixture.CopyValue = *CopyValue;

  const auto Bank = rcc::RegisterBankID::fromValue(1);
  const auto RegClass = rcc::RegisterClassID::fromValue(2);
  for (const rcc::Register Reg : {Fixture.Value, Fixture.CopyValue}) {
    Context.expect(Function.registerInfo().setRegisterBank(Reg, Bank).has_value() &&
                       Function.registerInfo().setRegisterClass(Reg, RegClass).has_value(),
                   "source vreg constraints must be recorded");
  }

  const auto LiveIn = *rcc::PhysicalRegister::create(1);
  const auto Reserved = *rcc::PhysicalRegister::create(2);
  const auto Hint = *rcc::PhysicalRegister::create(5);
  Context.expect(Function.registerInfo().addAllocationHint(Fixture.Value, Hint).has_value() &&
                     Function.registerInfo().addLiveIn(LiveIn, Fixture.Value).has_value() &&
                     Function.registerInfo().reservePhysicalRegister(Reserved).has_value() &&
                     Fixture.Entry->addLiveIn(LiveIn).has_value(),
                 "source register configuration must be recorded");

  rcc::MachineIRBuilder Builder(Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "source entry insertion point must be valid");
  auto Def = Builder.buildInstr(rcc::Opcode::fromValue(DefOpcode));
  Context.expect(Def && Def->addDef(Fixture.Value).has_value(),
                 "source definition must be assembled");
  if (!Def)
    return false;
  Context.expect(Def->setFlags({rcc::MachineInstrFlag::NoMerge}).has_value() &&
                     Def->setSourceLocation({"source.mir", 9, 4, 2}).has_value() &&
                     Def->setOrigin(rcc::OriginID::fromValue(77)).has_value() && Def->build(),
                 "source definition metadata must build");

  auto Branch = Builder.buildInstr(rcc::Opcode::fromValue(BranchOpcode));
  Context.expect(Branch && Branch->addBasicBlock(*Fixture.Exit).has_value() && Branch->build(),
                 "source branch must build");
  Context.expect(Function.cfgUpdater()
                     .addSuccessor(*Fixture.Entry, *Fixture.Exit, rcc::BranchProbability::one())
                     .has_value(),
                 "source CFG edge must build");

  Context.expect(Builder.setInsertPoint(*Fixture.Exit).has_value(),
                 "source exit insertion point must be valid");
  auto Copy = Builder.buildInstr(rcc::Opcode::fromValue(CopyOpcode));
  Context.expect(Copy && Copy->addDef(Fixture.CopyValue).has_value() &&
                     Copy->addUse(Fixture.Value).has_value() && Copy->build(),
                 "source tied copy must build");

  auto Frame = Builder.buildInstr(rcc::Opcode::fromValue(FrameOpcode));
  Context.expect(Frame && Frame->addFrameIndex(Fixture.StackSlot, -8).has_value() && Frame->build(),
                 "source frame-index instruction must build");

  auto ConstantPool = Builder.buildInstr(rcc::Opcode::fromValue(ConstantPoolOpcode));
  Context.expect(ConstantPool && ConstantPool->addConstantPool(Fixture.Constant, 8).has_value() &&
                     ConstantPool->build(),
                 "source constant-pool instruction must build");

  auto JumpTableInstruction = Builder.buildInstr(rcc::Opcode::fromValue(JumpTableOpcode));
  Context.expect(JumpTableInstruction &&
                     JumpTableInstruction->addJumpTable(Fixture.JumpTable).has_value() &&
                     JumpTableInstruction->build(),
                 "source jump-table instruction must build");

  auto Address = Builder.buildInstr(rcc::Opcode::fromValue(BlockAddressOpcode));
  Context.expect(
      Address && Address->addBlockAddress(std::string(Function.name()), "exit", 3).has_value() &&
          Address->build(),
      "source self block-address instruction must build");

  for (rcc::MemoryBase Base : {rcc::MemoryBase::frameIndex(Fixture.StackSlot),
                               rcc::MemoryBase::constantPool(Fixture.Constant),
                               rcc::MemoryBase::jumpTable(Fixture.JumpTable)}) {
    auto MMO = Function.createMemOperand(loadDescription(std::move(Base)));
    Context.expect(MMO.has_value(), "source MMO must be created");
    if (!MMO)
      return false;
    Fixture.LoadMMOs.push_back(*MMO);
  }
  auto Load = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  if (!Load)
    return false;
  bool AddedAllMMOs = true;
  for (const rcc::MachineMemOperand *MMO : Fixture.LoadMMOs)
    AddedAllMMOs &= Load->addMemOperand(*MMO).has_value();
  Context.expect(AddedAllMMOs && Load->build(), "source load with MMOs must build");

  rcc::MachineInstr *Definition = findInstruction(*Fixture.Entry, DefOpcode);
  rcc::MachineInstr *LoadInstruction = findInstruction(*Fixture.Exit, LoadOpcode);
  const rcc::StackObject *Object = Function.frameInfo().object(Fixture.StackSlot);
  if (Definition == nullptr || LoadInstruction == nullptr || Object == nullptr)
    return false;
  rcc::StackObjectDesc UpdatedStackSlot = *Object->description();
  UpdatedStackSlot.Lifetime.push_back({Definition->stableID(), LoadInstruction->stableID()});
  Context.expect(Function.frameInfo()
                         .updateObject(Fixture.StackSlot, std::move(UpdatedStackSlot))
                         .has_value() &&
                     Function.frameInfo().setMaxCallFrameSize(16).has_value() &&
                     Function.frameInfo().freeze(32, *rcc::Alignment::create(16)).has_value(),
                 "source frame metadata must be finalized");

  auto TargetState = rcc::TargetFunctionState::create(
      CloneTargetState{Fixture.Entry->stableID(), LoadInstruction->stableID(), Fixture.Value,
                       Fixture.StackSlot, Fixture.Constant, Fixture.JumpTable});
  Context.expect(TargetState.has_value(), "source target function state must be created");
  if (!TargetState)
    return false;
  Context.expect(Function.setTargetState(std::move(*TargetState)).has_value(),
                 "source target function state must be installed");

  rcc::MachineCompilationFailure Failure;
  Failure.Diagnostic.Code = rcc::ErrorCode::Unsupported;
  Failure.Diagnostic.Message = "test instruction selection failed";
  Failure.Diagnostic.Location = rcc::SourceLocation{"source.mir", 12, 5, 0};
  Failure.FailurePass = "test-instruction-select";
  Failure.FailureInstruction = LoadInstruction->stableID();
  Failure.FailureStage = rcc::PipelineStage::Selected;
  Failure.FailureRule = "test-load-pattern";
  Failure.Target = "test64";
  Failure.Subtarget = "test64+feature";
  Context.expect(Function.compilationStatus()
                     .fail(rcc::MachineCompilationState::SelectionFailed, std::move(Failure))
                     .has_value(),
                 "source structured compilation failure must be recorded");

  Function.properties().set(rcc::MachineFunctionProperty::IsSSA);
  Function.properties().set(rcc::MachineFunctionProperty::TracksLiveness);
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  return true;
}

void testDeepClone(TestContext &Context) {
  auto Target = createTarget();
  auto Source = createFunction(Context, "source_function", 1);
  SourceFixture Fixture;
  if (!Target || !Source || !populateSource(Context, *Source, *Target, Fixture)) {
    return;
  }
  Context.expect(rcc::MachineVerifier::verify(*Source, *Target).has_value(),
                 "source function must verify before cloning");

  auto Clone = rcc::cloneMachineFunction(
      *Source, *Target,
      rcc::MachineFunctionCloneOptions{rcc::MachineFunctionStableID::fromValue(99),
                                       std::string("cloned_function")});
  Context.expect(Clone.has_value(), "valid source function must deep-clone");
  if (!Clone)
    return;
  rcc::MachineFunction &Destination = *Clone->ClonedFunction;

  Context.expect(Destination.name() == "cloned_function" &&
                     Destination.stableID() == rcc::MachineFunctionStableID::fromValue(99),
                 "clone must use its explicit function identity");
  Context.expect(Destination.properties() == Source->properties() &&
                     rcc::MachineVerifier::verify(Destination, *Target).has_value() &&
                     rcc::MachineVerifier::verify(*Source, *Target).has_value(),
                 "source and destination must independently preserve valid properties");
  Context.expect(Source->allBlocks().size() == 3 && Destination.allBlocks().size() == 2 &&
                     Source->allInstructions().size() == 9 &&
                     Destination.allInstructions().size() == 8 &&
                     Source->frameInfo().objects().size() == 2 &&
                     Destination.frameInfo().objects().size() == 1 &&
                     Source->constantPool().entries().size() == 2 &&
                     Destination.constantPool().entries().size() == 1 &&
                     Source->jumpTableInfo().tables().size() == 2 &&
                     Destination.jumpTableInfo().tables().size() == 1,
                 "clone must omit detached arena history");
  Context.expect(Clone->BlockStableIDs.size() == 2 && Clone->InstructionStableIDs.size() == 8 &&
                     Clone->VirtualRegisters.size() == 2 && Clone->FrameIndices.size() == 1 &&
                     Clone->ConstantPoolIndices.size() == 1 && Clone->JumpTableIndices.size() == 1,
                 "clone must publish complete live identity mappings");

  rcc::MachineBasicBlock *DestinationEntry = Destination.firstBlock();
  rcc::MachineBasicBlock *DestinationExit =
      DestinationEntry == nullptr ? nullptr : DestinationEntry->next();
  Context.expect(DestinationEntry != nullptr && DestinationExit != nullptr &&
                     DestinationExit->next() == nullptr && DestinationEntry != Fixture.Entry &&
                     DestinationExit != Fixture.Exit,
                 "clone blocks must be distinct and preserve layout");
  if (DestinationEntry == nullptr || DestinationExit == nullptr)
    return;

  const auto EntryID = Clone->BlockStableIDs.find(Fixture.Entry->stableID());
  const auto ExitID = Clone->BlockStableIDs.find(Fixture.Exit->stableID());
  Context.expect(EntryID != Clone->BlockStableIDs.end() && ExitID != Clone->BlockStableIDs.end() &&
                     EntryID->second == DestinationEntry->stableID() &&
                     ExitID->second == DestinationExit->stableID(),
                 "block StableID mapping must identify cloned layout objects");
  Context.expect(DestinationEntry->successors().size() == 1 &&
                     DestinationEntry->successors()[0].Block == DestinationExit &&
                     DestinationExit->predecessors().size() == 1 &&
                     DestinationExit->predecessors()[0] == DestinationEntry,
                 "clone must rebuild reciprocal CFG edges");
  Context.expect(std::ranges::equal(DestinationEntry->liveIns(), Fixture.Entry->liveIns()),
                 "clone must preserve block live-ins");

  const auto ValuePosition = Clone->VirtualRegisters.find(Fixture.Value);
  const auto CopyValuePosition = Clone->VirtualRegisters.find(Fixture.CopyValue);
  Context.expect(ValuePosition != Clone->VirtualRegisters.end() &&
                     CopyValuePosition != Clone->VirtualRegisters.end(),
                 "clone must map every live source vreg");
  if (ValuePosition == Clone->VirtualRegisters.end() ||
      CopyValuePosition == Clone->VirtualRegisters.end()) {
    return;
  }
  const rcc::Register NewValue = ValuePosition->second;
  const rcc::Register NewCopyValue = CopyValuePosition->second;
  Context.expect(NewValue.virtualIndex() == 0 && NewCopyValue.virtualIndex() == 1,
                 "clone must deterministically compact source vreg tombstones");

  auto SourceInfo = Source->registerInfo().virtualRegisterInfo(Fixture.Value);
  auto DestinationInfo = Destination.registerInfo().virtualRegisterInfo(NewValue);
  Context.expect(SourceInfo && DestinationInfo &&
                     SourceInfo->get().Type == DestinationInfo->get().Type &&
                     SourceInfo->get().Bank == DestinationInfo->get().Bank &&
                     SourceInfo->get().RegClass == DestinationInfo->get().RegClass &&
                     SourceInfo->get().AllocationHints == DestinationInfo->get().AllocationHints,
                 "clone must preserve vreg type and allocation constraints");
  Context.expect(Destination.registerInfo().liveIns().size() == 1 &&
                     Destination.registerInfo().liveIns()[0].InitialVirtualRegister == NewValue &&
                     std::ranges::equal(Destination.registerInfo().reservedRegisters(),
                                        Source->registerInfo().reservedRegisters()),
                 "clone must remap function live-ins and preserve reserved registers");

  rcc::MachineInstr *DestinationDef = findInstruction(*DestinationEntry, DefOpcode);
  rcc::MachineInstr *DestinationBranch = findInstruction(*DestinationEntry, BranchOpcode);
  rcc::MachineInstr *DestinationCopy = findInstruction(*DestinationExit, CopyOpcode);
  rcc::MachineInstr *DestinationFrame = findInstruction(*DestinationExit, FrameOpcode);
  rcc::MachineInstr *DestinationConstantPool =
      findInstruction(*DestinationExit, ConstantPoolOpcode);
  rcc::MachineInstr *DestinationJumpTable = findInstruction(*DestinationExit, JumpTableOpcode);
  rcc::MachineInstr *DestinationAddress = findInstruction(*DestinationExit, BlockAddressOpcode);
  rcc::MachineInstr *DestinationLoad = findInstruction(*DestinationExit, LoadOpcode);
  Context.expect(DestinationDef && DestinationBranch && DestinationCopy && DestinationFrame &&
                     DestinationConstantPool && DestinationJumpTable && DestinationAddress &&
                     DestinationLoad,
                 "clone must preserve every live instruction");
  if (!DestinationDef || !DestinationBranch || !DestinationCopy || !DestinationFrame ||
      !DestinationConstantPool || !DestinationJumpTable || !DestinationAddress ||
      !DestinationLoad) {
    return;
  }

  Context.expect(DestinationDef->operandAt(0)->registerValue() == NewValue &&
                     DestinationDef->flags().has(rcc::MachineInstrFlag::NoMerge) &&
                     DestinationDef->sourceLocation() ==
                         rcc::SourceLocation{"source.mir", 9, 4, 2} &&
                     DestinationDef->origin() == rcc::OriginID::fromValue(77),
                 "clone must remap register operands and preserve instruction metadata");
  Context.expect(DestinationBranch->operandAt(0)->getIf<rcc::BasicBlockOperand>()->Block ==
                     DestinationExit,
                 "clone must remap basic-block operands");
  Context.expect(DestinationCopy->operandAt(0)->registerValue() == NewCopyValue &&
                     DestinationCopy->operandAt(1)->registerValue() == NewValue &&
                     DestinationCopy->operandAt(0)->tiedOperandIndex() == 1 &&
                     DestinationCopy->operandAt(1)->tiedOperandIndex() == 0,
                 "clone must remap tied registers and rebuild ties");
  const auto FramePosition = Clone->FrameIndices.find(Fixture.StackSlot);
  Context.expect(FramePosition != Clone->FrameIndices.end() &&
                     FramePosition->second == rcc::FrameIndex::fromValue(0) &&
                     DestinationFrame->operandAt(0)->getIf<rcc::FrameIndexOperand>()->Index ==
                         FramePosition->second,
                 "clone must compact and remap frame indices");

  const auto ConstantPosition = Clone->ConstantPoolIndices.find(Fixture.Constant);
  const auto JumpTablePosition = Clone->JumpTableIndices.find(Fixture.JumpTable);
  Context.expect(
      ConstantPosition != Clone->ConstantPoolIndices.end() &&
          JumpTablePosition != Clone->JumpTableIndices.end() &&
          ConstantPosition->second == rcc::ConstantPoolIndex::fromValue(0) &&
          JumpTablePosition->second == rcc::JumpTableIndex::fromValue(0) &&
          DestinationConstantPool->operandAt(0)->getIf<rcc::ConstantPoolOperand>()->Index ==
              ConstantPosition->second &&
          DestinationJumpTable->operandAt(0)->getIf<rcc::JumpTableOperand>()->Index ==
              JumpTablePosition->second,
      "clone must compact and remap constant-pool and jump-table indices");

  const rcc::MachineConstantPoolEntry *DestinationConstantEntry =
      ConstantPosition == Clone->ConstantPoolIndices.end()
          ? nullptr
          : Destination.constantPool().entry(ConstantPosition->second);
  const auto *DestinationPoolAddress =
      DestinationConstantEntry == nullptr
          ? nullptr
          : std::get_if<rcc::BlockAddressConstant>(&DestinationConstantEntry->description()->Value);
  Context.expect(Destination.constantPool().isFrozen() && DestinationPoolAddress != nullptr &&
                     DestinationPoolAddress->FunctionName == "cloned_function" &&
                     DestinationPoolAddress->BlockName == "exit",
                 "clone must preserve frozen constant pool state and rewrite self addresses");

  const rcc::MachineJumpTableEntry *DestinationJumpTableEntry =
      JumpTablePosition == Clone->JumpTableIndices.end()
          ? nullptr
          : Destination.jumpTableInfo().table(JumpTablePosition->second);
  const rcc::MachineJumpTableDesc *DestinationJumpDescription =
      DestinationJumpTableEntry == nullptr ? nullptr : DestinationJumpTableEntry->description();
  Context.expect(Destination.jumpTableInfo().isFrozen() && DestinationJumpDescription != nullptr &&
                     DestinationJumpDescription->Targets.size() == 3 &&
                     DestinationJumpDescription->Targets[0] == DestinationExit &&
                     DestinationJumpDescription->Targets[1] == DestinationEntry &&
                     DestinationJumpDescription->Targets[2] == DestinationExit,
                 "clone must preserve jump-table order while remapping block targets");

  Context.expect(
      DestinationAddress->operandAt(0)->getIf<rcc::BlockAddressOperand>()->FunctionName ==
          "cloned_function",
      "clone must rewrite self block-address function names");

  bool MMOsMatch = DestinationLoad->memOperands().size() == Fixture.LoadMMOs.size();
  for (std::size_t Index = 0; MMOsMatch && Index < DestinationLoad->memOperands().size(); ++Index) {
    rcc::MachineMemOperandDesc ExpectedMMO = Fixture.LoadMMOs[Index]->description();
    if (const auto Frame = ExpectedMMO.Base.frameIndex();
        Frame && FramePosition != Clone->FrameIndices.end()) {
      ExpectedMMO.Base = rcc::MemoryBase::frameIndex(FramePosition->second);
    }
    if (const auto Constant = ExpectedMMO.Base.constantPoolIndex();
        Constant && ConstantPosition != Clone->ConstantPoolIndices.end()) {
      ExpectedMMO.Base = rcc::MemoryBase::constantPool(ConstantPosition->second);
    }
    if (const auto JumpTable = ExpectedMMO.Base.jumpTableIndex();
        JumpTable && JumpTablePosition != Clone->JumpTableIndices.end()) {
      ExpectedMMO.Base = rcc::MemoryBase::jumpTable(JumpTablePosition->second);
    }

    const rcc::MachineMemOperand *DestinationMMO = DestinationLoad->memOperands()[Index];
    MMOsMatch =
        DestinationMMO != Fixture.LoadMMOs[Index] && DestinationMMO->description() == ExpectedMMO &&
        Destination.ownsMemOperand(*DestinationMMO) && !Source->ownsMemOperand(*DestinationMMO);
  }
  Context.expect(MMOsMatch, "clone must deep-copy MMO ownership while remapping every table base");

  const rcc::StackObject *DestinationStackSlot =
      FramePosition == Clone->FrameIndices.end()
          ? nullptr
          : Destination.frameInfo().object(FramePosition->second);
  Context.expect(
      DestinationStackSlot != nullptr && Destination.frameInfo().isFrozen() &&
          Destination.frameInfo().staticStackSize() == 32 &&
          Destination.frameInfo().maxCallFrameSize() == 16 &&
          DestinationStackSlot->description()->Lifetime.size() == 1 &&
          DestinationStackSlot->description()->Lifetime[0].Begin == DestinationDef->stableID() &&
          DestinationStackSlot->description()->Lifetime[0].End == DestinationLoad->stableID(),
      "clone must preserve frozen frame layout and remap instruction lifetimes");

  rcc::MachineInstr *SourceDef = findInstruction(*Fixture.Entry, DefOpcode);
  const auto InstructionID = SourceDef == nullptr
                                 ? Clone->InstructionStableIDs.end()
                                 : Clone->InstructionStableIDs.find(SourceDef->stableID());
  Context.expect(SourceDef != nullptr && InstructionID != Clone->InstructionStableIDs.end() &&
                     InstructionID->second == DestinationDef->stableID(),
                 "instruction StableID mapping must identify cloned instructions");

  rcc::MachineInstr *SourceLoad = findInstruction(*Fixture.Exit, LoadOpcode);
  const auto LoadID = SourceLoad == nullptr
                          ? Clone->InstructionStableIDs.end()
                          : Clone->InstructionStableIDs.find(SourceLoad->stableID());
  const CloneTargetState *SourceTarget = Source->getTargetInfo<CloneTargetState>();
  const CloneTargetState *DestinationTarget = Destination.getTargetInfo<CloneTargetState>();
  Context.expect(SourceLoad != nullptr && LoadID != Clone->InstructionStableIDs.end() &&
                     SourceTarget != nullptr && DestinationTarget != nullptr &&
                     SourceTarget != DestinationTarget && EntryID != Clone->BlockStableIDs.end() &&
                     FramePosition != Clone->FrameIndices.end() &&
                     ConstantPosition != Clone->ConstantPoolIndices.end() &&
                     JumpTablePosition != Clone->JumpTableIndices.end() &&
                     DestinationTarget->Block == EntryID->second &&
                     DestinationTarget->Instruction == LoadID->second &&
                     DestinationTarget->TrackedRegister == NewValue &&
                     DestinationTarget->Frame == FramePosition->second &&
                     DestinationTarget->Constant == ConstantPosition->second &&
                     DestinationTarget->JumpTable == JumpTablePosition->second,
                 "clone must strongly clone target state through every identity mapping");

  const rcc::MachineCompilationFailure *SourceFailure = Source->compilationStatus().failure();
  const rcc::MachineCompilationFailure *DestinationFailure =
      Destination.compilationStatus().failure();
  Context.expect(SourceFailure != nullptr && DestinationFailure != nullptr &&
                     Destination.compilationStatus().state() ==
                         rcc::MachineCompilationState::SelectionFailed &&
                     DestinationFailure->FailureInstruction == DestinationLoad->stableID() &&
                     DestinationFailure->Diagnostic == SourceFailure->Diagnostic &&
                     DestinationFailure->FailurePass == SourceFailure->FailurePass &&
                     DestinationFailure->FailureStage == SourceFailure->FailureStage &&
                     DestinationFailure->FailureRule == SourceFailure->FailureRule &&
                     DestinationFailure->Target == SourceFailure->Target &&
                     DestinationFailure->Subtarget == SourceFailure->Subtarget,
                 "clone must preserve compilation failure details and remap its instruction");
}

void testCloneFailures(TestContext &Context) {
  auto Target = createTarget();
  auto Source = createFunction(Context, "failure_source", 2);
  if (!Target || !Source)
    return;

  Context.expect(
      !rcc::cloneMachineFunction(*Source, *Target,
                                 rcc::MachineFunctionCloneOptions{
                                     rcc::MachineFunctionStableID::fromValue(0), std::nullopt}),
      "clone must reject an invalid destination StableID");
  Context.expect(
      !rcc::cloneMachineFunction(*Source, *Target,
                                 rcc::MachineFunctionCloneOptions{
                                     rcc::MachineFunctionStableID::fromValue(3), std::string()}),
      "clone must reject an explicitly empty destination name");

  auto Invalid = createFunction(Context, "invalid_source", 4);
  rcc::MachineBasicBlock *Block = Invalid ? createBlock(Context, *Invalid) : nullptr;
  if (Invalid && Block != nullptr) {
    auto Unknown = Invalid->createInstruction(rcc::Opcode::fromValue(999));
    Context.expect(Unknown && Block->appendInstruction(**Unknown).has_value(),
                   "invalid clone source must contain an unknown opcode");
    Context.expect(
        !rcc::cloneMachineFunction(*Invalid, *Target,
                                   rcc::MachineFunctionCloneOptions{
                                       rcc::MachineFunctionStableID::fromValue(5), std::nullopt}),
        "clone must verify and reject an invalid source function");
  }

  auto UnmappedState = createFunction(Context, "unmapped_target_state", 6);
  auto Physical = rcc::Register::makePhysical(1);
  if (UnmappedState && Physical) {
    auto State = rcc::TargetFunctionState::create(CloneTargetState{
        rcc::MachineBlockStableID::fromValue(99), rcc::MachineInstrStableID::fromValue(99),
        *Physical, rcc::FrameIndex::fromValue(0), rcc::ConstantPoolIndex::fromValue(0),
        rcc::JumpTableIndex::fromValue(0)});
    Context.expect(State && UnmappedState->setTargetState(std::move(*State)).has_value(),
                   "unmapped target state negative test must be installed");
    Context.expect(
        !rcc::cloneMachineFunction(*UnmappedState, *Target,
                                   rcc::MachineFunctionCloneOptions{
                                       rcc::MachineFunctionStableID::fromValue(7), std::nullopt}),
        "clone must reject target state that does not map its identities");
  }
}

} // namespace

int main() {
  TestContext Context;
  testDeepClone(Context);
  testCloneFailures(Context);
  return Context.result();
}
