// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineCSE.h"
#include "rcc/CodeGen/MachineCombiner.h"
#include "rcc/CodeGen/MachineDCE.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLICM.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineMemoryFolding.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineSink.h"
#include "rcc/CodeGen/TargetMemoryFolding.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

struct Fixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Entry = nullptr;
};

[[nodiscard]] rcc::Opcode opcode(rcc::riscv::InstructionOpcode Value) noexcept;

class TestMemoryFoldingInfo final : public rcc::TargetMemoryFoldingInfo {
public:
  explicit TestMemoryFoldingInfo(bool DropMemory = false) noexcept : DropMemory(DropMemory) {}

  [[nodiscard]] rcc::Expected<bool>
  canFoldMemoryOperand(const rcc::MachineInstr &Consumer, std::size_t OperandIndex,
                       const rcc::MachineInstr &MemoryInstruction, const rcc::MachineFunction &,
                       const rcc::TargetDescription &) const override {
    return Consumer.opcode() == opcode(rcc::riscv::InstructionOpcode::ADDI) && OperandIndex == 1 &&
           MemoryInstruction.opcode() == opcode(rcc::riscv::InstructionOpcode::LD);
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *>
  foldMemoryOperand(rcc::MachineInstr &Consumer, std::size_t, rcc::MachineInstr &MemoryInstruction,
                    rcc::MachineFunction &Function,
                    const rcc::TargetDescription &Target) const override {
    const rcc::MachineOperand *DestinationOperand = Consumer.operandAt(0);
    const rcc::MachineOperand *BaseOperand = MemoryInstruction.operandAt(1);
    const rcc::MachineOperand *OffsetOperand = MemoryInstruction.operandAt(2);
    const rcc::Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<rcc::Register>();
    const rcc::Register *Base =
        BaseOperand == nullptr ? nullptr : BaseOperand->getIf<rcc::Register>();
    const std::int64_t *Offset =
        OffsetOperand == nullptr ? nullptr : OffsetOperand->getIf<std::int64_t>();
    rcc::MachineBasicBlock *Block = Consumer.parent();
    if (Destination == nullptr || Base == nullptr || Offset == nullptr || Block == nullptr ||
        MemoryInstruction.parent() != Block) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "test memory fold received an invalid candidate");
    }

    rcc::MachineIRBuilder Builder(Function, Target);
    if (auto Point = Builder.setInsertPointBefore(Consumer); !Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::LD));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Added = Pending->addDef(*Destination); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Pending->addUse(*Base); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Pending->addImmediate(*Offset); !Added)
      return std::unexpected(Added.error());
    for (const rcc::MachineMemOperand *Memory : MemoryInstruction.memOperands()) {
      if (Memory == nullptr)
        return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                   "test memory fold received a null MMO");
      if (auto Added = Pending->addMemOperand(*Memory); !Added)
        return std::unexpected(Added.error());
    }
    auto Folded = Pending->build();
    if (!Folded)
      return std::unexpected(Folded.error());
    if (DropMemory) {
      while (!(*Folded)->memOperands().empty()) {
        if (auto Erased = (*Folded)->eraseMemOperand(0); !Erased)
          return std::unexpected(Erased.error());
      }
    }
    if (auto Removed = Block->removeInstruction(MemoryInstruction); !Removed)
      return std::unexpected(Removed.error());
    if (auto Removed = Block->removeInstruction(Consumer); !Removed)
      return std::unexpected(Removed.error());
    return *Folded;
  }

  [[nodiscard]] rcc::Expected<bool>
  unfoldMemoryOperand(rcc::MachineInstr &, rcc::MachineFunction &,
                      const rcc::TargetDescription &) const override {
    return false;
  }

  [[nodiscard]] rcc::Expected<std::optional<rcc::MemorySize>>
  getFoldedMemorySize(const rcc::MachineInstr &, const rcc::MachineFunction &,
                      const rcc::TargetDescription &) const override {
    auto Size = rcc::MemorySize::fixed(8);
    if (!Size)
      return std::unexpected(Size.error());
    return std::optional<rcc::MemorySize>(*Size);
  }

private:
  bool DropMemory;
};

[[nodiscard]] rcc::Opcode opcode(rcc::riscv::InstructionOpcode Value) noexcept {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] Fixture makeFixture(TestContext &Context, std::string Name, std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "optimization fixture function creation must succeed");
  if (!Function)
    return {};
  auto Entry = (*Function)->createBasicBlock();
  Context.expect(Entry.has_value(), "optimization fixture block creation must succeed");
  if (!Entry || !(*Function)->appendBlock(**Entry))
    return {};
  (*Function)->properties().set(rcc::MachineFunctionProperty::IsSSA);
  (*Function)->properties().set(rcc::MachineFunctionProperty::Legalized);
  (*Function)->properties().set(rcc::MachineFunctionProperty::RegBankSelected);
  (*Function)->properties().set(rcc::MachineFunctionProperty::Selected);
  return {std::move(*Function), *Entry};
}

void setPostRAProperties(rcc::MachineFunction &Function) {
  Function.properties().clear(rcc::MachineFunctionProperty::IsSSA);
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  Function.properties().set(rcc::MachineFunctionProperty::TiedOpsRewritten);
  Function.properties().set(rcc::MachineFunctionProperty::NoVRegs);
  Function.properties().set(rcc::MachineFunctionProperty::TracksLiveness);
}

[[nodiscard]] rcc::Register makeGPR(TestContext &Context, rcc::MachineFunction &Function) {
  auto Type = rcc::LowLevelType::makeCategoryNeutralScalar(64);
  auto Reg = Type ? Function.registerInfo().createVirtualRegister(*Type)
                  : rcc::Expected<rcc::Register>(std::unexpected(rcc::Error(
                        rcc::ErrorCode::InternalError, "test LowLevelType creation failed")));
  Context.expect(Reg.has_value(), "optimization virtual register creation must succeed");
  if (!Reg)
    return {};
  const auto Bank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto Class =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  Context.expect(Function.registerInfo().setRegisterBank(*Reg, Bank).has_value() &&
                     Function.registerInfo().setRegisterClass(*Reg, Class).has_value(),
                 "optimization register constraints must be assigned");
  return *Reg;
}

[[nodiscard]] rcc::PhysicalRegister physical(TestContext &Context,
                                             rcc::riscv::PhysicalRegisterID ID) {
  auto Reg = rcc::riscv::physicalRegister(ID);
  Context.expect(Reg.has_value(), "optimization physical register lookup must succeed");
  return Reg.value_or(rcc::PhysicalRegister{});
}

[[nodiscard]] rcc::MachineInstr *buildADDI(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                           rcc::Register Destination, rcc::Register Source,
                                           std::int64_t Immediate) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ADDI));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Source) ||
      !Pending->addImmediate(Immediate)) {
    Context.expect(false, "ADDI construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "ADDI insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] rcc::MachineInstr *buildADD(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                          rcc::Register Destination, rcc::Register Left,
                                          rcc::Register Right) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ADD));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Left) ||
      !Pending->addUse(Right)) {
    Context.expect(false, "ADD construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "ADD insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] const rcc::MachineMemOperand *
makeMemory(TestContext &Context, rcc::MachineFunction &Function, bool Store, bool Invariant) {
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  Context.expect(Size.has_value() && Align.has_value(), "memory shape creation must succeed");
  if (!Size || !Align)
    return nullptr;
  rcc::MemoryAccessFlags Flags{Store ? rcc::MemoryAccessFlag::Store : rcc::MemoryAccessFlag::Load};
  if (Invariant) {
    Flags.set(rcc::MemoryAccessFlag::Invariant);
    Flags.set(rcc::MemoryAccessFlag::Dereferenceable);
  }
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = Flags;
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = Invariant ? rcc::MemoryBase::constantPoolPseudo() : rcc::MemoryBase::stack();
  auto Memory = Function.createMemOperand(std::move(Description));
  Context.expect(Memory.has_value(), "memory operand creation must succeed");
  return Memory.value_or(nullptr);
}

[[nodiscard]] rcc::MachineInstr *buildStore(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                            rcc::Register Value, rcc::PhysicalRegister StackPointer,
                                            const rcc::MachineMemOperand &Memory) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::SD));
  if (!Pending || !Pending->addUse(Value) ||
      !Pending->addUse(rcc::Register::fromPhysical(StackPointer)) || !Pending->addImmediate(0) ||
      !Pending->addMemOperand(Memory)) {
    Context.expect(false, "store construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "store insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] rcc::MachineInstr *buildLoad(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                           rcc::Register Destination, rcc::Register Address,
                                           const rcc::MachineMemOperand &Memory) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::LD));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Address) ||
      !Pending->addImmediate(0) || !Pending->addMemOperand(Memory)) {
    Context.expect(false, "load construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "load insertion must succeed");
  return Built.value_or(nullptr);
}

void testDCEFixedPointAndDebugUses(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "dce", 1);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "DCE insert point must be established");
  const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::PhysicalRegister X2 = physical(Context, rcc::riscv::PhysicalRegisterID::X2);

  const rcc::Register LiveRoot = makeGPR(Context, *Fixture.Function);
  const rcc::Register LiveValue = makeGPR(Context, *Fixture.Function);
  const rcc::Register DeadRoot = makeGPR(Context, *Fixture.Function);
  const rcc::Register DeadValue = makeGPR(Context, *Fixture.Function);
  (void)buildADDI(Context, Builder, LiveRoot, rcc::Register::fromPhysical(X0), 1);
  (void)buildADDI(Context, Builder, LiveValue, LiveRoot, 2);
  (void)buildADDI(Context, Builder, DeadRoot, rcc::Register::fromPhysical(X0), 3);
  (void)buildADDI(Context, Builder, DeadValue, DeadRoot, 4);

  auto Debug = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::DBG_VALUE));
  Context.expect(Debug.has_value() &&
                     Debug->addUse(DeadValue, {rcc::RegisterOperandFlag::Debug}).has_value() &&
                     Debug->build().has_value(),
                 "debug-only use construction must succeed");
  const rcc::MachineMemOperand *Memory = makeMemory(Context, *Fixture.Function, true, false);
  if (Memory == nullptr)
    return;
  rcc::MachineInstr *Store = buildStore(Context, Builder, LiveValue, X2, *Memory);
  rcc::MachineInstr *FrameSetup = buildADDI(Context, Builder, rcc::Register::fromPhysical(X2),
                                            rcc::Register::fromPhysical(X2), -16);
  rcc::MachineInstr *FrameDestroy = buildADDI(Context, Builder, rcc::Register::fromPhysical(X2),
                                              rcc::Register::fromPhysical(X2), 16);
  Context.expect(FrameSetup != nullptr && FrameDestroy != nullptr,
                 "DCE frame instruction construction must succeed");
  if (FrameSetup == nullptr || FrameDestroy == nullptr)
    return;
  FrameSetup->setFlags({rcc::MachineInstrFlag::FrameSetup});
  FrameDestroy->setFlags({rcc::MachineInstrFlag::FrameDestroy});
  for (rcc::MachineInstr *Instruction : {FrameSetup, FrameDestroy}) {
    rcc::MachineOperand *Definition = Instruction->operandAt(0);
    if (Definition == nullptr)
      return;
    rcc::RegisterOperandFlags Flags = Definition->registerFlags();
    Flags.set(rcc::RegisterOperandFlag::Dead);
    Context.expect(Definition->setRegisterFlags(Flags).has_value(),
                   "DCE frame definition must accept a dead marker");
  }
  const std::size_t Before = Fixture.Entry->instructionCount();

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineDCEPass Pass;
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine DCE failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "machine DCE must remove the debug-only dead chain");
  Context.expect(Fixture.Entry->instructionCount() + 2 == Before,
                 "machine DCE must reach a fixed point across the dead chain");
  Context.expect(Store != nullptr && Store->isLinked(), "machine DCE must preserve a store");
  Context.expect(FrameSetup->isLinked() && FrameDestroy->isLinked(),
                 "machine DCE must preserve frame setup and destroy instructions");
  Context.expect(!Fixture.Function->registerInfo().isKnownVirtualRegister(DeadRoot) &&
                     !Fixture.Function->registerInfo().isKnownVirtualRegister(DeadValue),
                 "machine DCE must retire removed virtual registers");
  rcc::MachineInstr *DebugInstruction = nullptr;
  for (rcc::MachineInstr *Instruction = Fixture.Entry->firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->opcode() == rcc::opcode(rcc::GenericOpcode::DBG_VALUE))
      DebugInstruction = Instruction;
  }
  const rcc::MachineOperand *DebugOperand =
      DebugInstruction == nullptr ? nullptr : DebugInstruction->operandAt(0);
  Context.expect(DebugOperand != nullptr && DebugOperand->registerValue().has_value() &&
                     DebugOperand->registerValue()->isNoRegister(),
                 "machine DCE must clear a dangling debug register use");
}

void testMachineCombinerIdentityRules(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "combiner", 6);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "combiner insert point must be established");
  const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::PhysicalRegister X2 = physical(Context, rcc::riscv::PhysicalRegisterID::X2);
  const rcc::Register Seed = makeGPR(Context, *Fixture.Function);
  const rcc::Register Identity = makeGPR(Context, *Fixture.Function);
  const rcc::Register Consumer = makeGPR(Context, *Fixture.Function);
  (void)buildADDI(Context, Builder, Seed, rcc::Register::fromPhysical(X0), 7);
  rcc::MachineInstr *IdentityInstruction = buildADDI(Context, Builder, Identity, Seed, 0);
  rcc::MachineInstr *ConsumerInstruction = buildADD(Context, Builder, Consumer, Identity, Seed);
  const rcc::MachineMemOperand *Memory = makeMemory(Context, *Fixture.Function, true, false);
  if (Memory == nullptr)
    return;
  (void)buildStore(Context, Builder, Consumer, X2, *Memory);

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineCombinerPass Pass(rcc::riscv::machineCombinerInfo(),
                                rcc::MachineCombinerStage::SelectedSSA);
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine combiner failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "machine combiner must apply a registered RV64GC identity rule");
  Context.expect(IdentityInstruction != nullptr && !IdentityInstruction->isLinked() &&
                     !Fixture.Function->registerInfo().isKnownVirtualRegister(Identity),
                 "machine combiner must erase the identity and retire its result");
  const rcc::MachineOperand *ConsumerUse =
      ConsumerInstruction == nullptr ? nullptr : ConsumerInstruction->operandAt(1);
  Context.expect(ConsumerUse != nullptr && ConsumerUse->registerValue() == Seed,
                 "machine combiner executor must rewrite all result uses");
}

void testPostRAMachineCombinerIdentity(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "post_ra_combiner", 7);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  Fixture.Function->properties().clear(rcc::MachineFunctionProperty::IsSSA);
  Fixture.Function->properties().set(rcc::MachineFunctionProperty::NoPHIs);
  Fixture.Function->properties().set(rcc::MachineFunctionProperty::TiedOpsRewritten);
  Fixture.Function->properties().set(rcc::MachineFunctionProperty::NoVRegs);
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "post-RA combiner insert point must be established");
  const rcc::PhysicalRegister X10 = physical(Context, rcc::riscv::PhysicalRegisterID::X10);
  rcc::MachineInstr *Identity = buildADDI(Context, Builder, rcc::Register::fromPhysical(X10),
                                          rcc::Register::fromPhysical(X10), 0);
  rcc::MachineInstr *FrameIdentity = buildADDI(Context, Builder, rcc::Register::fromPhysical(X10),
                                               rcc::Register::fromPhysical(X10), 0);
  if (FrameIdentity != nullptr)
    FrameIdentity->setFlags({rcc::MachineInstrFlag::FrameDestroy});
  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineCombinerPass Pass(rcc::riscv::machineCombinerInfo(),
                                rcc::MachineCombinerStage::PostRegisterAllocation);
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  Context.expect(
      Result.has_value() && Result->Changed && Identity != nullptr && !Identity->isLinked(),
      "post-RA combiner must erase a physical-register no-op only when identities match");
  Context.expect(FrameIdentity != nullptr && FrameIdentity->isLinked(),
                 "post-frame semantics must protect an otherwise removable identity");
}

void testCSERegisterAndMemoryRules(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "cse", 2);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "CSE insert point must be established");
  const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);

  const rcc::Register Seed = makeGPR(Context, *Fixture.Function);
  const rcc::Register First = makeGPR(Context, *Fixture.Function);
  const rcc::Register Redundant = makeGPR(Context, *Fixture.Function);
  const rcc::Register Consumer = makeGPR(Context, *Fixture.Function);
  (void)buildADDI(Context, Builder, Seed, rcc::Register::fromPhysical(X0), 9);
  rcc::MachineInstr *FirstInstruction = buildADDI(Context, Builder, First, Seed, 7);
  rcc::MachineInstr *RedundantInstruction = buildADDI(Context, Builder, Redundant, Seed, 7);
  rcc::MachineInstr *ConsumerInstruction = buildADD(Context, Builder, Consumer, Redundant, Seed);

  const rcc::Register MutableLoadOne = makeGPR(Context, *Fixture.Function);
  const rcc::Register MutableLoadTwo = makeGPR(Context, *Fixture.Function);
  const rcc::Register InvariantLoadOne = makeGPR(Context, *Fixture.Function);
  const rcc::Register InvariantLoadTwo = makeGPR(Context, *Fixture.Function);
  const rcc::MachineMemOperand *MutableMemory =
      makeMemory(Context, *Fixture.Function, false, false);
  const rcc::MachineMemOperand *InvariantMemory =
      makeMemory(Context, *Fixture.Function, false, true);
  if (MutableMemory == nullptr || InvariantMemory == nullptr)
    return;
  rcc::MachineInstr *MutableOne = buildLoad(Context, Builder, MutableLoadOne, Seed, *MutableMemory);
  rcc::MachineInstr *MutableTwo = buildLoad(Context, Builder, MutableLoadTwo, Seed, *MutableMemory);
  rcc::MachineInstr *InvariantOne =
      buildLoad(Context, Builder, InvariantLoadOne, Seed, *InvariantMemory);
  rcc::MachineInstr *InvariantTwo =
      buildLoad(Context, Builder, InvariantLoadTwo, Seed, *InvariantMemory);

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineCSEPass Pass;
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine CSE failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "machine CSE must eliminate safe redundant expressions");
  Context.expect(FirstInstruction != nullptr && FirstInstruction->isLinked() &&
                     RedundantInstruction != nullptr && !RedundantInstruction->isLinked(),
                 "machine CSE must keep the dominating expression only");
  const rcc::MachineOperand *ConsumerUse =
      ConsumerInstruction == nullptr ? nullptr : ConsumerInstruction->operandAt(1);
  Context.expect(ConsumerUse != nullptr && ConsumerUse->registerValue() == First,
                 "machine CSE must rewrite uses to the dominating result");
  Context.expect(MutableOne != nullptr && MutableOne->isLinked() && MutableTwo != nullptr &&
                     MutableTwo->isLinked(),
                 "machine CSE must not merge mutable loads");
  Context.expect(InvariantOne != nullptr && InvariantOne->isLinked() && InvariantTwo != nullptr &&
                     !InvariantTwo->isLinked(),
                 "machine CSE must merge invariant dereferenceable loads");
  Context.expect(!Fixture.Function->registerInfo().isKnownVirtualRegister(Redundant) &&
                     !Fixture.Function->registerInfo().isKnownVirtualRegister(InvariantLoadTwo),
                 "machine CSE must retire redundant result registers");
}

void testCSEBudgetPreservesLegalMIR(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "cse_budget", 3);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "CSE budget insert point must be established");
  const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::Register Seed = makeGPR(Context, *Fixture.Function);
  const rcc::Register First = makeGPR(Context, *Fixture.Function);
  const rcc::Register Second = makeGPR(Context, *Fixture.Function);
  (void)buildADDI(Context, Builder, Seed, rcc::Register::fromPhysical(X0), 1);
  (void)buildADDI(Context, Builder, First, Seed, 2);
  rcc::MachineInstr *SecondInstruction = buildADDI(Context, Builder, Second, Seed, 2);

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineCSEPass Pass({.MaximumComparisons = 0});
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine CSE budget failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && !Result->Changed && SecondInstruction != nullptr &&
                     SecondInstruction->isLinked(),
                 "machine CSE budget exhaustion must stop without rejecting or mutating MIR");
}

void testLICMHoistsOnlyInvariantInstructions(TestContext &Context,
                                             const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "licm", 4);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  auto Header = Fixture.Function->createBasicBlock();
  auto Body = Fixture.Function->createBasicBlock();
  auto Exit = Fixture.Function->createBasicBlock();
  Context.expect(Header.has_value() && Body.has_value() && Exit.has_value() &&
                     Fixture.Function->appendBlock(**Header).has_value() &&
                     Fixture.Function->appendBlock(**Body).has_value() &&
                     Fixture.Function->appendBlock(**Exit).has_value(),
                 "LICM loop blocks must be created");
  if (!Header || !Body || !Exit)
    return;
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  Context.expect(Half.has_value(), "LICM branch probability must be created");
  if (!Half)
    return;
  auto Updater = Fixture.Function->cfgUpdater();
  Context.expect(
      Updater.addSuccessor(*Fixture.Entry, **Header, rcc::BranchProbability::one()).has_value() &&
          Updater.addSuccessor(**Header, **Body, *Half).has_value() &&
          Updater.addSuccessor(**Header, **Exit, *Half).has_value() &&
          Updater.addSuccessor(**Body, **Header, rcc::BranchProbability::one()).has_value(),
      "LICM loop CFG must be connected");

  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "LICM preheader insertion point must be established");
  const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::Register Seed = makeGPR(Context, *Fixture.Function);
  (void)buildADDI(Context, Builder, Seed, rcc::Register::fromPhysical(X0), 1);

  Context.expect(Builder.setInsertPoint(**Body).has_value(),
                 "LICM body insertion point must be established");
  const rcc::Register HoistedValue = makeGPR(Context, *Fixture.Function);
  const rcc::Register DependentValue = makeGPR(Context, *Fixture.Function);
  rcc::MachineInstr *Hoisted = buildADDI(Context, Builder, HoistedValue, Seed, 8);
  rcc::MachineInstr *Dependent = buildADDI(Context, Builder, DependentValue, HoistedValue, 9);
  const rcc::Register MutableLoad = makeGPR(Context, *Fixture.Function);
  const rcc::MachineMemOperand *Memory = makeMemory(Context, *Fixture.Function, false, false);
  rcc::MachineInstr *Load =
      Memory == nullptr ? nullptr : buildLoad(Context, Builder, MutableLoad, Seed, *Memory);

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineLICMPass Pass;
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine LICM failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "machine LICM must hoist loop-invariant instructions");
  Context.expect(Hoisted != nullptr && Hoisted->parent() == Fixture.Entry && Dependent != nullptr &&
                     Dependent->parent() == Fixture.Entry,
                 "machine LICM must reach a fixed point for dependent invariants");
  Context.expect(Load != nullptr && Load->parent() == *Body,
                 "machine LICM must keep mutable loads in the loop");
}

void testMachineSinkUsesColdUniqueSuccessor(TestContext &Context,
                                            const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "sink", 5);
  if (!Fixture.Function || Fixture.Entry == nullptr)
    return;
  auto Cold = Fixture.Function->createBasicBlock();
  auto Hot = Fixture.Function->createBasicBlock();
  Context.expect(Cold.has_value() && Hot.has_value() &&
                     Fixture.Function->appendBlock(**Cold).has_value() &&
                     Fixture.Function->appendBlock(**Hot).has_value(),
                 "machine sink successor blocks must be created");
  if (!Cold || !Hot)
    return;
  auto ColdProbability = rcc::BranchProbability::fromFraction(1, 10);
  auto HotProbability = rcc::BranchProbability::fromFraction(9, 10);
  Context.expect(ColdProbability.has_value() && HotProbability.has_value(),
                 "machine sink probabilities must be created");
  if (!ColdProbability || !HotProbability)
    return;
  auto Updater = Fixture.Function->cfgUpdater();
  Context.expect(Updater.addSuccessor(*Fixture.Entry, **Cold, *ColdProbability).has_value() &&
                     Updater.addSuccessor(*Fixture.Entry, **Hot, *HotProbability).has_value(),
                 "machine sink CFG must be connected");

  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "machine sink source insertion point must be established");
  const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::Register Seed = makeGPR(Context, *Fixture.Function);
  const rcc::Register SunkValue = makeGPR(Context, *Fixture.Function);
  (void)buildADDI(Context, Builder, Seed, rcc::Register::fromPhysical(X0), 1);
  rcc::MachineInstr *Sunk = buildADDI(Context, Builder, SunkValue, Seed, 4);

  Context.expect(Builder.setInsertPoint(**Cold).has_value(),
                 "machine sink destination insertion point must be established");
  const rcc::Register Consumer = makeGPR(Context, *Fixture.Function);
  rcc::MachineInstr *Use = buildADD(Context, Builder, Consumer, SunkValue, Seed);

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineSinkPass Pass;
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine sink failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "machine sink must move a cold-path-only expression");
  Context.expect(Sunk != nullptr && Sunk->parent() == *Cold && Use != nullptr &&
                     Sunk->next() == Use,
                 "machine sink must place the definition immediately before its first use");
}

void testPostRACodeMotion(TestContext &Context, const rcc::TargetDescription &Target) {
  auto LICMFixture = makeFixture(Context, "post-ra-licm", 10);
  if (!LICMFixture.Function || LICMFixture.Entry == nullptr)
    return;
  auto Header = LICMFixture.Function->createBasicBlock();
  auto Body = LICMFixture.Function->createBasicBlock();
  auto Exit = LICMFixture.Function->createBasicBlock();
  if (!Header || !Body || !Exit || !LICMFixture.Function->appendBlock(**Header) ||
      !LICMFixture.Function->appendBlock(**Body) || !LICMFixture.Function->appendBlock(**Exit)) {
    Context.expect(false, "post-RA LICM blocks must be created");
    return;
  }
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  if (!Half)
    return;
  auto LICMUpdater = LICMFixture.Function->cfgUpdater();
  if (!LICMUpdater.addSuccessor(*LICMFixture.Entry, **Header, rcc::BranchProbability::one()) ||
      !LICMUpdater.addSuccessor(**Header, **Body, *Half) ||
      !LICMUpdater.addSuccessor(**Header, **Exit, *Half) ||
      !LICMUpdater.addSuccessor(**Body, **Header, rcc::BranchProbability::one())) {
    Context.expect(false, "post-RA LICM CFG must be connected");
    return;
  }
  rcc::MachineIRBuilder LICMBuilder(*LICMFixture.Function, Target);
  if (!LICMBuilder.setInsertPoint(*LICMFixture.Entry))
    return;
  const rcc::Register X0 =
      rcc::Register::fromPhysical(physical(Context, rcc::riscv::PhysicalRegisterID::X0));
  const rcc::PhysicalRegister X2Physical = physical(Context, rcc::riscv::PhysicalRegisterID::X2);
  const rcc::Register X10 =
      rcc::Register::fromPhysical(physical(Context, rcc::riscv::PhysicalRegisterID::X10));
  const rcc::Register X12 =
      rcc::Register::fromPhysical(physical(Context, rcc::riscv::PhysicalRegisterID::X12));
  const rcc::Register X13 =
      rcc::Register::fromPhysical(physical(Context, rcc::riscv::PhysicalRegisterID::X13));
  (void)buildADDI(Context, LICMBuilder, X10, X0, 1);
  if (!LICMBuilder.setInsertPoint(**Header))
    return;
  rcc::MachineInstr *Hoisted = buildADDI(Context, LICMBuilder, X12, X10, 4);
  if (Hoisted == nullptr)
    return;
  Hoisted->setFlags({rcc::MachineInstrFlag::NoFPExcept});
  if (!LICMBuilder.setInsertPoint(**Body))
    return;
  (void)buildADDI(Context, LICMBuilder, X13, X12, 1);
  if (!LICMBuilder.setInsertPoint(**Exit))
    return;
  const rcc::MachineMemOperand *LICMMemory =
      makeMemory(Context, *LICMFixture.Function, true, false);
  if (LICMMemory == nullptr)
    return;
  (void)buildStore(Context, LICMBuilder, X12, X2Physical, *LICMMemory);
  setPostRAProperties(*LICMFixture.Function);
  Context.expect(rcc::rebuildMachineLiveness(*LICMFixture.Function, Target).has_value(),
                 "post-RA LICM fixture liveness must be built");
  rcc::MachineFunctionAnalysisManager LICMAnalyses;
  rcc::MachineLICMPass PostRALICM(rcc::MachineCodeMotionStage::PostRegisterAllocation);
  auto LICMResult = PostRALICM.run(*LICMFixture.Function, Target, LICMAnalyses);
  Context.expect(LICMResult && LICMResult->Changed && Hoisted->parent() == LICMFixture.Entry &&
                     rcc::verifyTrackedMachineLiveness(*LICMFixture.Function, Target).has_value(),
                 "post-RA LICM must hoist a safe physical-register invariant and repair liveness");

  auto SinkFixture = makeFixture(Context, "post-ra-sink", 11);
  if (!SinkFixture.Function || SinkFixture.Entry == nullptr)
    return;
  auto Cold = SinkFixture.Function->createBasicBlock();
  auto Hot = SinkFixture.Function->createBasicBlock();
  if (!Cold || !Hot || !SinkFixture.Function->appendBlock(**Cold) ||
      !SinkFixture.Function->appendBlock(**Hot)) {
    return;
  }
  auto ColdProbability = rcc::BranchProbability::fromFraction(1, 10);
  auto HotProbability = rcc::BranchProbability::fromFraction(9, 10);
  if (!ColdProbability || !HotProbability)
    return;
  auto SinkUpdater = SinkFixture.Function->cfgUpdater();
  if (!SinkUpdater.addSuccessor(*SinkFixture.Entry, **Cold, *ColdProbability) ||
      !SinkUpdater.addSuccessor(*SinkFixture.Entry, **Hot, *HotProbability)) {
    return;
  }
  rcc::MachineIRBuilder SinkBuilder(*SinkFixture.Function, Target);
  if (!SinkBuilder.setInsertPoint(*SinkFixture.Entry))
    return;
  (void)buildADDI(Context, SinkBuilder, X10, X0, 2);
  rcc::MachineInstr *Sunk = buildADDI(Context, SinkBuilder, X12, X10, 8);
  if (Sunk == nullptr)
    return;
  Sunk->setFlags({rcc::MachineInstrFlag::NoFPExcept});
  if (!SinkBuilder.setInsertPoint(**Cold))
    return;
  rcc::MachineInstr *ColdUse = buildADDI(Context, SinkBuilder, X13, X12, 1);
  setPostRAProperties(*SinkFixture.Function);
  Context.expect(rcc::rebuildMachineLiveness(*SinkFixture.Function, Target).has_value(),
                 "post-RA sink fixture liveness must be built");
  rcc::MachineFunctionAnalysisManager SinkAnalyses;
  rcc::MachineSinkPass PostRASink(rcc::MachineCodeMotionStage::PostRegisterAllocation);
  auto SinkResult = PostRASink.run(*SinkFixture.Function, Target, SinkAnalyses);
  Context.expect(SinkResult && SinkResult->Changed && Sunk->parent() == *Cold &&
                     Sunk->next() == ColdUse &&
                     rcc::verifyTrackedMachineLiveness(*SinkFixture.Function, Target).has_value(),
                 "post-RA sinking must move a physical definition to its sole cold use");
}

void testMemoryFoldingContract(TestContext &Context, const rcc::TargetDescription &Target) {
  auto BuildCandidate = [&](std::string Name, std::uint64_t ID) {
    Fixture Candidate = makeFixture(Context, std::move(Name), ID);
    if (!Candidate.Function || Candidate.Entry == nullptr)
      return Candidate;
    rcc::MachineIRBuilder Builder(*Candidate.Function, Target);
    if (!Builder.setInsertPoint(*Candidate.Entry))
      return Candidate;
    const rcc::PhysicalRegister X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
    const rcc::PhysicalRegister X2 = physical(Context, rcc::riscv::PhysicalRegisterID::X2);
    const rcc::Register Address = makeGPR(Context, *Candidate.Function);
    const rcc::Register Loaded = makeGPR(Context, *Candidate.Function);
    const rcc::Register FoldedResult = makeGPR(Context, *Candidate.Function);
    (void)buildADDI(Context, Builder, Address, rcc::Register::fromPhysical(X0), 0);
    const rcc::MachineMemOperand *LoadMemory =
        makeMemory(Context, *Candidate.Function, false, false);
    const rcc::MachineMemOperand *StoreMemory =
        makeMemory(Context, *Candidate.Function, true, false);
    if (LoadMemory == nullptr || StoreMemory == nullptr)
      return Candidate;
    (void)buildLoad(Context, Builder, Loaded, Address, *LoadMemory);
    (void)buildADDI(Context, Builder, FoldedResult, Loaded, 0);
    (void)buildStore(Context, Builder, FoldedResult, X2, *StoreMemory);
    return Candidate;
  };

  Fixture Folded = BuildCandidate("memory-fold", 8);
  if (!Folded.Function || Folded.Entry == nullptr)
    return;
  TestMemoryFoldingInfo GoodInfo;
  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineMemoryFoldingPass Pass(GoodInfo);
  auto Result = Pass.run(*Folded.Function, Target, Analyses);
  if (!Result)
    std::cerr << "machine memory folding failure: " << Result.error().message() << '\n';
  std::size_t LoadCount = 0;
  rcc::MachineInstr *FoldedLoad = nullptr;
  for (rcc::MachineInstr *Instruction = Folded.Entry->firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::LD)) {
      ++LoadCount;
      FoldedLoad = Instruction;
    }
  }
  Context.expect(Result && Result->Changed && LoadCount == 1 && FoldedLoad != nullptr &&
                     FoldedLoad->memOperands().size() == 1,
                 "memory folding must replace an adjacent load-use pair and migrate its MMO");

  Fixture Rejected = BuildCandidate("memory-fold-missing-mmo", 9);
  if (!Rejected.Function || Rejected.Entry == nullptr)
    return;
  rcc::MachineInstr *OriginalLoad = Rejected.Entry->firstInstruction();
  OriginalLoad = OriginalLoad == nullptr ? nullptr : OriginalLoad->next();
  rcc::MachineInstr *OriginalConsumer = OriginalLoad == nullptr ? nullptr : OriginalLoad->next();
  const std::size_t OriginalCount = Rejected.Entry->instructionCount();
  TestMemoryFoldingInfo BadInfo(true);
  rcc::MachineMemoryFoldingPass BadPass(BadInfo);
  auto Failure = BadPass.run(*Rejected.Function, Target, Analyses);
  Context.expect(!Failure && Failure.error().code() == rcc::ErrorCode::VerificationError &&
                     Rejected.Entry->instructionCount() == OriginalCount &&
                     OriginalLoad != nullptr && OriginalLoad->isLinked() &&
                     OriginalConsumer != nullptr && OriginalConsumer->isLinked(),
                 "memory folding must reject missing MMO migration without mutating the function");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "optimization target construction must succeed");
  if (!Target)
    return Context.result();
  testDCEFixedPointAndDebugUses(Context, *Target);
  testMachineCombinerIdentityRules(Context, *Target);
  testPostRAMachineCombinerIdentity(Context, *Target);
  testCSERegisterAndMemoryRules(Context, *Target);
  testCSEBudgetPreservesLegalMIR(Context, *Target);
  testLICMHoistsOnlyInvariantInstructions(Context, *Target);
  testMachineSinkUsesColdUniqueSuccessor(Context, *Target);
  testPostRACodeMotion(Context, *Target);
  testMemoryFoldingContract(Context, *Target);
  return Context.result();
}
