// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Nop));
  Descriptors.push_back(std::move(Def));
  Descriptors.push_back(std::move(Branch));
  Descriptors.push_back(std::move(Copy));
  Descriptors.push_back(std::move(Frame));
  Descriptors.push_back(std::move(BlockAddress));
  Descriptors.push_back(std::move(Load));
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

[[nodiscard]] rcc::MachineMemOperandDesc loadDescription(rcc::FrameIndex Index) {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Dereferenceable};
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.Base = rcc::MemoryBase::frameIndex(Index);
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
  const rcc::MachineMemOperand *LoadMMO = nullptr;
};

[[nodiscard]] bool populateSource(TestContext &Context, rcc::MachineFunction &Function,
                                  const rcc::InstructionSet &InstructionSet,
                                  SourceFixture &Fixture) {
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

  rcc::MachineIRBuilder Builder(Function, InstructionSet);
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

  auto Address = Builder.buildInstr(rcc::Opcode::fromValue(BlockAddressOpcode));
  Context.expect(
      Address && Address->addBlockAddress(std::string(Function.name()), "exit", 3).has_value() &&
          Address->build(),
      "source self block-address instruction must build");

  auto MMO = Function.createMemOperand(loadDescription(Fixture.StackSlot));
  Context.expect(MMO.has_value(), "source MMO must be created");
  if (!MMO)
    return false;
  Fixture.LoadMMO = *MMO;
  auto Load = Builder.buildInstr(rcc::Opcode::fromValue(LoadOpcode));
  Context.expect(Load && Load->addMemOperand(**MMO).has_value() && Load->build(),
                 "source load with MMO must build");

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

  Function.properties().set(rcc::MachineFunctionProperty::IsSSA);
  Function.properties().set(rcc::MachineFunctionProperty::TracksLiveness);
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  return true;
}

void testDeepClone(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Source = createFunction(Context, "source_function", 1);
  SourceFixture Fixture;
  if (!InstructionSet || !Source || !populateSource(Context, *Source, *InstructionSet, Fixture)) {
    return;
  }
  Context.expect(rcc::MachineVerifier::verify(*Source, *InstructionSet).has_value(),
                 "source function must verify before cloning");

  auto Clone = rcc::cloneMachineFunction(
      *Source, *InstructionSet,
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
                     rcc::MachineVerifier::verify(Destination, *InstructionSet).has_value() &&
                     rcc::MachineVerifier::verify(*Source, *InstructionSet).has_value(),
                 "source and destination must independently preserve valid properties");
  Context.expect(Source->allBlocks().size() == 3 && Destination.allBlocks().size() == 2 &&
                     Source->allInstructions().size() == 7 &&
                     Destination.allInstructions().size() == 6 &&
                     Source->frameInfo().objects().size() == 2 &&
                     Destination.frameInfo().objects().size() == 1,
                 "clone must omit detached arena history");
  Context.expect(Clone->BlockStableIDs.size() == 2 && Clone->InstructionStableIDs.size() == 6 &&
                     Clone->VirtualRegisters.size() == 2,
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
  rcc::MachineInstr *DestinationAddress = findInstruction(*DestinationExit, BlockAddressOpcode);
  rcc::MachineInstr *DestinationLoad = findInstruction(*DestinationExit, LoadOpcode);
  Context.expect(DestinationDef && DestinationBranch && DestinationCopy && DestinationFrame &&
                     DestinationAddress && DestinationLoad,
                 "clone must preserve every live instruction");
  if (!DestinationDef || !DestinationBranch || !DestinationCopy || !DestinationFrame ||
      !DestinationAddress || !DestinationLoad) {
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
  Context.expect(
      DestinationAddress->operandAt(0)->getIf<rcc::BlockAddressOperand>()->FunctionName ==
          "cloned_function",
      "clone must rewrite self block-address function names");

  rcc::MachineMemOperandDesc ExpectedMMO = Fixture.LoadMMO->description();
  if (FramePosition != Clone->FrameIndices.end())
    ExpectedMMO.Base = rcc::MemoryBase::frameIndex(FramePosition->second);
  Context.expect(DestinationLoad->memOperands().size() == 1 &&
                     DestinationLoad->memOperands()[0] != Fixture.LoadMMO &&
                     DestinationLoad->memOperands()[0]->description() == ExpectedMMO &&
                     Destination.ownsMemOperand(*DestinationLoad->memOperands()[0]) &&
                     !Source->ownsMemOperand(*DestinationLoad->memOperands()[0]),
                 "clone must deep-copy MMO ownership while remapping frame semantics");

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
}

void testCloneFailures(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Source = createFunction(Context, "failure_source", 2);
  if (!InstructionSet || !Source)
    return;

  Context.expect(
      !rcc::cloneMachineFunction(*Source, *InstructionSet,
                                 rcc::MachineFunctionCloneOptions{
                                     rcc::MachineFunctionStableID::fromValue(0), std::nullopt}),
      "clone must reject an invalid destination StableID");
  Context.expect(
      !rcc::cloneMachineFunction(*Source, *InstructionSet,
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
        !rcc::cloneMachineFunction(*Invalid, *InstructionSet,
                                   rcc::MachineFunctionCloneOptions{
                                       rcc::MachineFunctionStableID::fromValue(5), std::nullopt}),
        "clone must verify and reject an invalid source function");
  }
}

} // namespace

int main() {
  TestContext Context;
  testDeepClone(Context);
  testCloneFailures(Context);
  return Context.result();
}
