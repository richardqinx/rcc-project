// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegisterMask.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
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
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFunction(TestContext &Context, std::string Name, std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "valid machine function construction must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] rcc::MachineBasicBlock *createLinkedBlock(TestContext &Context,
                                                        rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  Context.expect(Block.has_value(), "basic block allocation must succeed");
  if (!Block)
    return nullptr;
  auto LinkResult = Function.appendBlock(**Block);
  Context.expect(LinkResult.has_value(), "basic block insertion must succeed");
  if (!LinkResult)
    return nullptr;
  return *Block;
}

[[nodiscard]] bool addRegisterOperand(TestContext &Context, rcc::MachineInstr &Instruction,
                                      rcc::Register Reg, rcc::RegisterOperandFlags Flags) {
  auto Operand = rcc::MachineOperand::registerOperand(Reg, Flags);
  Context.expect(Operand.has_value(), "valid register operand construction must succeed");
  if (!Operand)
    return false;
  auto AddResult = Instruction.addOperand(std::move(*Operand));
  Context.expect(AddResult.has_value(), "register operand insertion must succeed");
  return AddResult.has_value();
}

[[nodiscard]] std::size_t countOperands(rcc::RegOperandRange Range) {
  std::size_t Count = 0;
  for ([[maybe_unused]] const rcc::MachineOperand &Operand : Range)
    ++Count;
  return Count;
}

rcc::MachineMemOperandDesc makeLoadDescription() {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = rcc::MemoryAccessFlags{
      rcc::MemoryAccessFlag::Load,
      rcc::MemoryAccessFlag::Dereferenceable,
  };
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.Base = rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0));
  return Description;
}

void testOperandKinds(TestContext &Context) {
  auto Function = createFunction(Context, "operands", 1);
  if (!Function)
    return;
  auto Block = Function->createBasicBlock();
  Context.expect(Block.has_value(), "operand test block allocation must succeed");
  if (!Block)
    return;

  const std::array<std::uint64_t, 1> HalfWords{0x3c00};
  auto HalfBits = rcc::APFloat::create(rcc::FloatSemantics::IEEEHalf, HalfWords);
  Context.expect(HalfBits.has_value(), "well-sized floating immediate must be accepted");
  if (!HalfBits)
    return;
  Context.expect(HalfBits->bitWidth() == 16 && HalfBits->words()[0] == 0x3c00,
                 "floating immediate must preserve semantics and raw bits");

  const std::array<std::uint64_t, 1> OverflowHalfWords{0x10000};
  Context.expect(!rcc::APFloat::create(rcc::FloatSemantics::IEEEHalf, OverflowHalfWords),
                 "floating immediate bits above semantics width must be rejected");
  const std::array<std::uint64_t, 2> TooManyHalfWords{0, 0};
  Context.expect(!rcc::APFloat::create(rcc::FloatSemantics::IEEEHalf, TooManyHalfWords),
                 "floating immediate word-count mismatch must be rejected");

  auto Virtual = Function->registerInfo().createVirtualRegister(
      *rcc::LowLevelType::makeCategoryNeutralScalar(32));
  Context.expect(Virtual.has_value(), "operand test virtual register must be created");
  if (!Virtual)
    return;

  auto RegOperand = rcc::MachineOperand::registerOperand(
      *Virtual, rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Use});
  Context.expect(RegOperand && RegOperand->kind() == rcc::MachineOperand::Kind::Register,
                 "register operand must preserve its kind");
  Context.expect(!rcc::MachineOperand::registerOperand(*Virtual, {}),
                 "register operand without def/use role must be rejected");
  Context.expect(!rcc::MachineOperand::registerOperand(
                     *Virtual, rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Define,
                                                         rcc::RegisterOperandFlag::Kill}),
                 "kill flag on register definition must be rejected");

  auto Immediate = rcc::MachineOperand::immediate(-9, 17);
  Context.expect(Immediate.kind() == rcc::MachineOperand::Kind::Immediate &&
                     *Immediate.getIf<std::int64_t>() == -9 && Immediate.targetFlags() == 17,
                 "immediate operand must preserve value and target flags");

  auto Floating = rcc::MachineOperand::floatingImmediate(*HalfBits);
  Context.expect(Floating.kind() == rcc::MachineOperand::Kind::FloatingImmediate,
                 "floating operand must preserve its kind");
  auto BlockRef = rcc::MachineOperand::basicBlock(**Block);
  Context.expect(BlockRef.getIf<rcc::BasicBlockOperand>()->Block == *Block,
                 "basic block operand must preserve its target");
  auto Frame = rcc::MachineOperand::frameIndex(rcc::FrameIndex::fromValue(4), -8);
  Context.expect(Frame.getIf<rcc::FrameIndexOperand>()->Offset == -8,
                 "frame index operand must preserve its offset");
  auto Constant = rcc::MachineOperand::constantPool(rcc::ConstantPoolIndex::fromValue(2), 3);
  Context.expect(Constant.kind() == rcc::MachineOperand::Kind::ConstantPoolIndex,
                 "constant pool operand must preserve its kind");
  auto Jump = rcc::MachineOperand::jumpTable(rcc::JumpTableIndex::fromValue(5));
  Context.expect(Jump.kind() == rcc::MachineOperand::Kind::JumpTableIndex,
                 "jump table operand must preserve its kind");

  auto Global = rcc::MachineOperand::globalSymbol("global", 12);
  auto External = rcc::MachineOperand::externalSymbol("external", -4);
  auto BlockAddress = rcc::MachineOperand::blockAddress("function", "block", 2);
  Context.expect(Global && External && BlockAddress, "valid symbol operands must be accepted");
  Context.expect(!rcc::MachineOperand::globalSymbol(""),
                 "empty global symbol operand must be rejected");
  Context.expect(!rcc::MachineOperand::blockAddress("", "block"),
                 "incomplete block address operand must be rejected");

  auto Mask = std::make_shared<const rcc::RegisterMask>(std::vector<std::uint64_t>{0xffff});
  auto MaskOperand = rcc::MachineOperand::registerMask(Mask);
  Context.expect(MaskOperand && MaskOperand->kind() == rcc::MachineOperand::Kind::RegisterMask,
                 "register mask operand must preserve shared immutable mask");
  Context.expect(!rcc::MachineOperand::registerMask(nullptr),
                 "null register mask operand must be rejected");

  auto MetadataReference =
      rcc::MachineMetadataValue::reference(rcc::MachineMetadataReference::embeddedIR(0));
  Context.expect(MetadataReference.has_value(),
                 "a valid scoped metadata reference must form an operand value");
  if (!MetadataReference)
    return;
  auto Metadata = rcc::MachineOperand::metadata(std::move(*MetadataReference));
  Context.expect(Metadata && Metadata->kind() == rcc::MachineOperand::Kind::Metadata &&
                     Metadata->getIf<rcc::MetadataOperand>()->Value.reference() != nullptr,
                 "metadata operand must preserve its scoped semantic value");
  Context.expect(!rcc::MachineOperand::metadata({}),
                 "an unrepresented metadata operand value must be rejected");

  auto Target = rcc::MachineOperand::targetIndex(rcc::TargetIndex::fromValue(8), 9);
  Context.expect(Target.getIf<rcc::TargetIndexOperand>()->Offset == 9,
                 "target index operand must preserve its offset");
  auto MCSymbol = rcc::MachineOperand::mcSymbol(".Ltmp0", 1);
  Context.expect(MCSymbol && MCSymbol->kind() == rcc::MachineOperand::Kind::MCSymbol,
                 "MC symbol precursor must preserve its kind");

  const auto Clone = Immediate.cloneDetached();
  Context.expect(Clone.parent() == nullptr &&
                     *Clone.getIf<std::int64_t>() == *Immediate.getIf<std::int64_t>(),
                 "detached operand clone must preserve payload without ownership");
  Context.expect(!Immediate.setRegister(*Virtual),
                 "register mutation on non-register operand must be rejected");
}

void testRegisterInfoAndOperandMovement(TestContext &Context) {
  auto Function = createFunction(Context, "def_use", 2);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = createLinkedBlock(Context, *Function);
  if (Block == nullptr)
    return;

  auto I32 = rcc::LowLevelType::makeCategoryNeutralScalar(32);
  Context.expect(I32.has_value(), "i32 construction for MRI test must succeed");
  if (!I32)
    return;
  auto V0 = Function->registerInfo().createVirtualRegister(*I32);
  auto V1 = Function->registerInfo().createVirtualRegister(*I32);
  Context.expect(V0 && V1, "MRI must allocate monotonic virtual registers");
  if (!V0 || !V1)
    return;
  Context.expect(V0->virtualIndex() == 0 && V1->virtualIndex() == 1,
                 "MRI virtual register indices must start at zero and increase");

  auto PhysReg = rcc::PhysicalRegister::create(7);
  Context.expect(PhysReg.has_value(), "MRI test physical register must be valid");
  if (!PhysReg)
    return;
  const rcc::Register Physical = rcc::Register::fromPhysical(*PhysReg);

  auto Instruction = Function->createInstruction(rcc::Opcode::fromValue(1));
  Context.expect(Instruction.has_value(), "machine instruction allocation must succeed");
  if (!Instruction)
    return;
  rcc::MachineInstr *Instr = *Instruction;

  if (!addRegisterOperand(Context, *Instr, *V0,
                          rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Define}) ||
      !addRegisterOperand(Context, *Instr, *V0,
                          rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Use}) ||
      !addRegisterOperand(Context, *Instr, *V0,
                          rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Use,
                                                    rcc::RegisterOperandFlag::Debug}) ||
      !addRegisterOperand(Context, *Instr, Physical,
                          rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Use})) {
    return;
  }

  Context.expect(Function->registerInfo().useEmpty(*V0),
                 "detached instruction operands must not enter def-use chains");
  auto LinkInstruction = Block->appendInstruction(*Instr);
  Context.expect(LinkInstruction.has_value(), "instruction insertion must succeed");
  if (!LinkInstruction)
    return;

  Context.expect(countOperands(Function->registerInfo().regOperands(*V0)) == 3,
                 "def-use chain must include definition, use, and debug use");
  Context.expect(countOperands(Function->registerInfo().useOperands(*V0)) == 2,
                 "use iterator must include ordinary and debug uses");
  Context.expect(countOperands(Function->registerInfo().useOperands(*V0, false)) == 1,
                 "nodbg use iterator must filter debug operands");
  Context.expect(Function->registerInfo().getUniqueVRegDef(*V0) == Instr->operandAt(0),
                 "unique virtual register definition must be discoverable");
  Context.expect(countOperands(Function->registerInfo().regOperands(Physical)) == 1,
                 "physical registers must participate in exact def-use chains");

  rcc::MachineOperand *const OldOperandAddress = Instr->operandAt(0);
  Instr->reserveOperands(128);
  Context.expect(Instr->operandAt(0) != nullptr && Instr->operandAt(0)->parent() == Instr,
                 "operand relocation must preserve instruction ownership");
  Context.expect(Function->registerInfo().verifyDefUseChains().has_value(),
                 "operand relocation must repair intrusive def-use links");
  Context.expect(OldOperandAddress != Instr->operandAt(0) ||
                     Function->registerInfo().getUniqueVRegDef(*V0) == Instr->operandAt(0),
                 "def-use head must follow an operand when its address changes");

  auto TieResult = Instr->tieOperands(0, 1);
  Context.expect(TieResult.has_value(), "valid def-use operand tie must succeed");
  const auto DetachedTieClone = Instr->operandAt(0)->cloneDetached();
  Context.expect(!DetachedTieClone.tiedOperandIndex(),
                 "single detached operand clone must not retain an instruction-local tie");
  auto InsertResult = Instr->insertOperand(0, rcc::MachineOperand::immediate(5));
  Context.expect(InsertResult.has_value(), "operand insertion before a tie must succeed");
  Context.expect(Instr->operandAt(1)->tiedOperandIndex() == 2 &&
                     Instr->operandAt(2)->tiedOperandIndex() == 1,
                 "operand insertion must rewrite both tie indices");
  auto EraseResult = Instr->eraseOperand(0);
  Context.expect(EraseResult.has_value(), "temporary operand erasure must succeed");
  Context.expect(Instr->operandAt(0)->tiedOperandIndex() == 1 &&
                     Instr->operandAt(1)->tiedOperandIndex() == 0,
                 "operand erasure must rewrite both tie indices");
  Context.expect(Instr->untieOperand(0).has_value(), "reciprocal operand tie must be removable");

  auto SetRegisterResult = Instr->operandAt(1)->setRegister(*V1);
  Context.expect(SetRegisterResult.has_value(), "register operand rebinding must succeed");
  Context.expect(Function->registerInfo().useEmpty(*V0, false),
                 "register rebinding must unlink the old ordinary use");
  Context.expect(Function->registerInfo().hasOneUse(*V1),
                 "register rebinding must link the new use");

  auto ReplaceResult = Function->registerInfo().replaceRegWith(*V1, *V0);
  Context.expect(ReplaceResult && *ReplaceResult == 1,
                 "replaceRegWith must rewrite every linked operand exactly once");
  Context.expect(Function->registerInfo().verifyDefUseChains().has_value(),
                 "register replacement must preserve exact def-use chains");

  for (std::int64_t Value = 0; Value < 64; ++Value) {
    auto Result = Instr->addOperand(rcc::MachineOperand::immediate(Value));
    Context.expect(Result.has_value(), "variadic immediate operand insertion must succeed");
  }
  Context.expect(Function->registerInfo().verifyDefUseChains().has_value(),
                 "repeated operand growth must preserve def-use links");

  auto &MRI = Function->registerInfo();
  Context.expect(MRI.setRegisterBank(*V0, rcc::RegisterBankID::fromValue(3)).has_value(),
                 "MRI must record a register bank");
  Context.expect(MRI.setRegisterClass(*V0, rcc::RegisterClassID::fromValue(4)).has_value(),
                 "MRI must record a register class");
  Context.expect(MRI.addAllocationHint(*V0, *PhysReg).has_value(),
                 "MRI must record a physical allocation hint");
  auto V0Info = MRI.virtualRegisterInfo(*V0);
  Context.expect(V0Info && V0Info->get().Type == I32 &&
                     V0Info->get().Bank == rcc::RegisterBankID::fromValue(3) &&
                     V0Info->get().RegClass == rcc::RegisterClassID::fromValue(4),
                 "MRI virtual register properties must evolve without losing LowLevelType");

  Context.expect(MRI.addLiveIn(*PhysReg, *V0).has_value(),
                 "MRI must record function physical live-ins");
  Context.expect(!MRI.addLiveIn(*PhysReg, *V0),
                 "duplicate function physical live-ins must be rejected");
  Context.expect(MRI.reservePhysicalRegister(*PhysReg).has_value(),
                 "MRI must record reserved physical registers");
  Context.expect(MRI.isReserved(*PhysReg),
                 "MRI reserved-register query must find recorded register");

  auto DeadVReg = MRI.createVirtualRegister(*I32);
  Context.expect(DeadVReg && MRI.eraseVirtualRegister(*DeadVReg).has_value(),
                 "unused virtual register must become a tombstone");
  auto LaterVReg = MRI.createVirtualRegister(*I32);
  Context.expect(LaterVReg && LaterVReg->virtualIndex() != DeadVReg->virtualIndex(),
                 "MRI must not reuse tombstoned virtual register indices");
  Context.expect(!MRI.eraseVirtualRegister(*V0),
                 "virtual register with linked operands must not be erased");

  auto ZombieVReg = MRI.createVirtualRegister(*I32);
  auto ZombieInstr = Function->createInstruction(rcc::Opcode::fromValue(2));
  Context.expect(ZombieVReg && ZombieInstr,
                 "detached tombstone validation setup must allocate objects");
  if (ZombieVReg && ZombieInstr) {
    Context.expect(addRegisterOperand(Context, **ZombieInstr, *ZombieVReg,
                                      rcc::RegisterOperandFlags{rcc::RegisterOperandFlag::Use}),
                   "detached instruction must initially accept its live virtual register");
    Context.expect(MRI.eraseVirtualRegister(*ZombieVReg).has_value(),
                   "detached operand must not make an unpublished vreg live");
    const std::size_t OriginalInstructionCount = Block->instructionCount();
    Context.expect(!Block->appendInstruction(**ZombieInstr),
                   "instruction insertion must reject a tombstoned virtual register");
    Context.expect((*ZombieInstr)->parent() == nullptr &&
                       Block->instructionCount() == OriginalInstructionCount,
                   "failed instruction insertion must not mutate block layout");
    Context.expect(!(*ZombieInstr)->operandAt(0)->setRegister(*ZombieVReg),
                   "detached register mutation must reject a tombstoned vreg");
  }

  auto UnlinkResult = Block->removeInstruction(*Instr);
  Context.expect(UnlinkResult.has_value(), "instruction removal must succeed");
  Context.expect(MRI.useEmpty(*V0) && MRI.getUniqueVRegDef(*V0) == nullptr,
                 "instruction removal must unlink every register operand");
  Context.expect(MRI.verifyDefUseChains().has_value(),
                 "empty def-use lists after removal must remain structurally valid");
  Context.expect(Block->appendInstruction(*Instr).has_value(),
                 "detached instruction must be reinsertable");
  Context.expect(MRI.getUniqueVRegDef(*V0) == Instr->operandAt(0),
                 "instruction reinsertion must rebuild register links");
}

void testOwnershipCFGAndMemory(TestContext &Context) {
  auto Function = createFunction(Context, "cfg", 3);
  auto OtherFunction = createFunction(Context, "other", 4);
  if (!Function || !OtherFunction)
    return;

  rcc::MachineBasicBlock *Entry = createLinkedBlock(Context, *Function);
  rcc::MachineBasicBlock *Left = createLinkedBlock(Context, *Function);
  rcc::MachineBasicBlock *Right = createLinkedBlock(Context, *Function);
  rcc::MachineBasicBlock *Foreign = createLinkedBlock(Context, *OtherFunction);
  if (Entry == nullptr || Left == nullptr || Right == nullptr || Foreign == nullptr)
    return;

  Context.expect(Function->blockCount() == 3,
                 "machine function block list must preserve layout count");
  Context.expect(Entry->layoutIndex() == 0 && Left->layoutIndex() == 1 && Right->layoutIndex() == 2,
                 "block layout indices must follow intrusive layout order");

  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  Context.expect(Half.has_value(), "CFG test probability must be valid");
  if (!Half)
    return;
  auto CFG = Function->cfgUpdater();
  Context.expect(CFG.addSuccessor(*Entry, *Left, *Half).has_value() &&
                     CFG.addSuccessor(*Entry, *Left, *Half).has_value(),
                 "duplicate semantic branches must merge into one CFG successor");
  Context.expect(Entry->successors().size() == 1 && Left->predecessors().size() == 1,
                 "CFG must not contain duplicate successor or predecessor entries");
  Context.expect(CFG.addSuccessor(*Entry, *Right, *Half).has_value(),
                 "second CFG successor must be insertable");
  Context.expect(!CFG.addSuccessor(*Entry, *Foreign, *Half),
                 "cross-function CFG edge must be rejected");
  Context.expect(!CFG.replaceSuccessor(*Entry, *Left, *Foreign) &&
                     Entry->successors().size() == 2 && Left->predecessors().size() == 1,
                 "cross-function CFG replacement must fail without mutating existing edges");

  Context.expect(CFG.replaceSuccessor(*Entry, *Left, *Right).has_value(),
                 "CFG successor replacement must succeed");
  Context.expect(Entry->successors().size() == 1 && Left->predecessors().empty() &&
                     Right->predecessors().size() == 1,
                 "CFG replacement must merge duplicate target and repair predecessors");
  Context.expect(CFG.transferSuccessors(*Entry, *Left).has_value(),
                 "CFG successor transfer must succeed");
  Context.expect(Entry->successors().empty() && Left->successors().size() == 1 &&
                     Right->predecessors()[0] == Left,
                 "CFG successor transfer must update both directions");
  Context.expect(CFG.disconnectBlock(*Left).has_value(),
                 "CFG block disconnection must remove incoming and outgoing edges");
  Context.expect(Left->successors().empty() && Right->predecessors().empty(),
                 "CFG disconnection must leave no stale reciprocal edges");

  auto Instr = Function->createInstruction(rcc::Opcode::fromValue(9));
  auto NextInstr = Function->createInstruction(rcc::Opcode::fromValue(10));
  Context.expect(Instr && NextInstr, "instruction arena allocation must succeed");
  if (!Instr || !NextInstr)
    return;
  Context.expect(Entry->appendInstruction(**Instr).has_value(),
                 "first block instruction must be inserted");
  Context.expect(Entry->insertInstructionBefore(**Instr, **NextInstr).has_value(),
                 "instruction insertion before a stable node must succeed");
  Context.expect(Entry->firstInstruction() == *NextInstr && Entry->lastInstruction() == *Instr &&
                     Entry->instructionCount() == 2,
                 "intrusive instruction list must preserve order and endpoints");

  auto CrossBlockOperand = rcc::MachineOperand::basicBlock(*Foreign);
  Context.expect(!(*Instr)->addOperand(std::move(CrossBlockOperand)),
                 "instruction must reject a block operand from another function");

  auto MemOperand = Function->createMemOperand(makeLoadDescription());
  auto ReplacementMemOperand = Function->createMemOperand(makeLoadDescription());
  auto ForeignMemOperand = OtherFunction->createMemOperand(makeLoadDescription());
  Context.expect(MemOperand && ReplacementMemOperand && ForeignMemOperand,
                 "function arena must allocate immutable memory operands");
  if (MemOperand && ReplacementMemOperand && ForeignMemOperand) {
    Context.expect((*Instr)->addMemOperand(**MemOperand).has_value(),
                   "instruction must accept a memory operand owned by its function");
    Context.expect((*Instr)->replaceMemOperand(0, **ReplacementMemOperand).has_value() &&
                       (*Instr)->memOperands().front() == *ReplacementMemOperand,
                   "instruction must replace an owned memory operand in place");
    Context.expect(!(*Instr)->replaceMemOperand(1, **MemOperand) &&
                       !(*Instr)->replaceMemOperand(0, **ForeignMemOperand) &&
                       (*Instr)->memOperands().front() == *ReplacementMemOperand,
                   "invalid memory replacement must fail without mutation");
    Context.expect(!(*Instr)->addMemOperand(**ForeignMemOperand),
                   "instruction must reject a memory operand from another function");
  }

  const rcc::MachineBasicBlock *StableEntryAddress = Entry;
  const rcc::MachineInstr *StableInstrAddress = *Instr;
  for (std::uint32_t Index = 0; Index < 128; ++Index) {
    auto NewBlock = Function->createBasicBlock();
    auto NewInstr = Function->createInstruction(rcc::Opcode::fromValue(Index + 100));
    Context.expect(NewBlock && NewInstr, "arena growth must continue to allocate objects");
  }
  Context.expect(StableEntryAddress == Entry && StableInstrAddress == *Instr,
                 "arena growth must preserve block and instruction addresses");

  Context.expect(Function->removeBlock(*Right).has_value(),
                 "disconnected empty block must be removable from layout");
  Context.expect(!Right->isLinked() && Function->blockCount() == 2,
                 "removed block must leave layout while retaining stable storage");
}

} // namespace

int main() {
  TestContext Context;
  testOperandKinds(Context);
  testRegisterInfoAndOperandMovement(Context);
  testOwnershipCFGAndMemory(Context);
  return Context.result();
}
