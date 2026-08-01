// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t DefGPR64Opcode = 1;
constexpr std::uint32_t DefGPR32Opcode = 2;
constexpr std::uint32_t EarlyClobberOpcode = 3;
constexpr std::uint32_t TiedAnyOpcode = 4;
constexpr std::uint32_t CallOpcode = 5;
constexpr std::uint32_t RenamableImplicitOpcode = 6;

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

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Block = nullptr;
};

[[nodiscard]] rcc::PhysicalRegister phys(std::uint32_t Number) {
  return *rcc::PhysicalRegister::create(Number);
}

[[nodiscard]] rcc::OperandDescriptor
registerOperand(rcc::RegisterOperandRole Role,
                std::optional<rcc::RegisterClassID> Class = std::nullopt,
                std::optional<rcc::RegisterBankID> Bank = std::nullopt, bool AllowSubReg = false,
                bool EarlyClobber = false) {
  rcc::OperandDescriptor Operand;
  Operand.Kinds = {rcc::MachineOperand::Kind::Register};
  Operand.RegisterRole = Role;
  Operand.RegClass = Class;
  Operand.RegBank = Bank;
  Operand.AllowSubReg = AllowSubReg;
  Operand.EarlyClobber = EarlyClobber;
  return Operand;
}

[[nodiscard]] rcc::InstructionDescriptor instruction(std::uint32_t OpcodeValue, std::string Name) {
  rcc::InstructionDescriptor Instruction;
  Instruction.InstructionOpcode = rcc::Opcode::fromValue(OpcodeValue);
  Instruction.Name = std::move(Name);
  Instruction.Flags = {rcc::InstructionFlag::ProducesCode};
  return Instruction;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  auto DefGPR64 = instruction(DefGPR64Opcode, "TEST_DEF_GPR64");
  DefGPR64.Operands.push_back(registerOperand(rcc::RegisterOperandRole::Def,
                                              rcc::test::TestRegisterClass::GPR64,
                                              rcc::test::TestRegisterBank::GPR, true));
  DefGPR64.NumExplicitDefs = 1;

  auto DefGPR32 = instruction(DefGPR32Opcode, "TEST_DEF_GPR32");
  DefGPR32.Operands.push_back(registerOperand(rcc::RegisterOperandRole::Def,
                                              rcc::test::TestRegisterClass::GPR32,
                                              rcc::test::TestRegisterBank::GPR, true));
  DefGPR32.NumExplicitDefs = 1;

  auto EarlyClobber = instruction(EarlyClobberOpcode, "TEST_EARLY_CLOBBER");
  EarlyClobber.Operands.push_back(registerOperand(rcc::RegisterOperandRole::Def,
                                                  rcc::test::TestRegisterClass::GPR64,
                                                  rcc::test::TestRegisterBank::GPR, false, true));
  EarlyClobber.Operands.push_back(registerOperand(rcc::RegisterOperandRole::Use,
                                                  rcc::test::TestRegisterClass::GPR64,
                                                  rcc::test::TestRegisterBank::GPR));
  EarlyClobber.NumExplicitDefs = 1;

  auto TiedAny = instruction(TiedAnyOpcode, "TEST_TIED_ANY");
  auto TiedDef = registerOperand(rcc::RegisterOperandRole::Def);
  auto TiedUse = registerOperand(rcc::RegisterOperandRole::Use);
  TiedDef.DefaultTiedOperand = 1;
  TiedUse.DefaultTiedOperand = 0;
  TiedAny.Operands.push_back(TiedDef);
  TiedAny.Operands.push_back(TiedUse);
  TiedAny.NumExplicitDefs = 1;

  auto Call = instruction(CallOpcode, "TEST_CALL");
  rcc::OperandDescriptor Mask;
  Mask.Kinds = {rcc::MachineOperand::Kind::RegisterMask};
  Call.Operands.push_back(Mask);
  Call.Flags.set(rcc::InstructionFlag::Call);
  Call.Flags.set(rcc::InstructionFlag::HasSideEffects);

  auto RenamableImplicit = instruction(RenamableImplicitOpcode, "TEST_RENAMABLE_IMPLICIT");
  RenamableImplicit.ImplicitDefs.push_back({phys(rcc::test::TestRegister::G0), false, true});

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(DefGPR64));
  Descriptors.push_back(std::move(DefGPR32));
  Descriptors.push_back(std::move(EarlyClobber));
  Descriptors.push_back(std::move(TiedAny));
  Descriptors.push_back(std::move(Call));
  Descriptors.push_back(std::move(RenamableImplicit));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] FunctionFixture createFunction(TestContext &Context, std::string Name,
                                             std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "target verifier function must be created");
  if (!Function)
    return {};

  auto Block = (*Function)->createBasicBlock();
  Context.expect(Block && (*Function)->appendBlock(**Block).has_value(),
                 "target verifier block must be linked");
  if (!Block)
    return {};
  return FunctionFixture{std::move(*Function), *Block};
}

[[nodiscard]] rcc::Expected<void> classify(rcc::MachineRegisterInfo &RegInfo, rcc::Register Reg,
                                           rcc::RegisterBankID Bank, rcc::RegisterClassID Class) {
  auto BankResult = RegInfo.setRegisterBank(Reg, Bank);
  if (!BankResult)
    return std::unexpected(BankResult.error());
  return RegInfo.setRegisterClass(Reg, Class);
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
appendInstruction(FunctionFixture &Fixture, std::uint32_t OpcodeValue,
                  std::vector<rcc::MachineOperand> Operands,
                  std::optional<rcc::MachineOperandTie> Tie = std::nullopt) {
  auto Instruction = Fixture.Function->createInstruction(rcc::Opcode::fromValue(OpcodeValue));
  if (!Instruction)
    return std::unexpected(Instruction.error());
  for (rcc::MachineOperand &Operand : Operands) {
    auto AddResult = (*Instruction)->addOperand(std::move(Operand));
    if (!AddResult)
      return std::unexpected(AddResult.error());
  }
  if (Tie) {
    auto TieResult = (*Instruction)->tieOperands(Tie->DefOperand, Tie->UseOperand);
    if (!TieResult)
      return std::unexpected(TieResult.error());
  }
  auto AppendResult = Fixture.Block->appendInstruction(**Instruction);
  if (!AppendResult)
    return std::unexpected(AppendResult.error());
  return *Instruction;
}

void testValidTargetFacts(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction(Context, "valid_target_facts", 1);
  if (!Fixture.Function)
    return;

  auto Type = rcc::LLT::makeInteger(64);
  auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
  if (!Reg)
    return;
  Context.expect(classify(Fixture.Function->registerInfo(), *Reg, rcc::test::TestRegisterBank::GPR,
                          rcc::test::TestRegisterClass::GPR64Alloc)
                     .has_value(),
                 "valid target verifier vreg must be classified");

  const auto G0 = phys(rcc::test::TestRegister::G0);
  const auto G1 = phys(rcc::test::TestRegister::G1);
  Context.expect(Fixture.Function->registerInfo().addLiveIn(G0, *Reg).has_value() &&
                     Fixture.Function->registerInfo()
                         .reservePhysicalRegister(phys(rcc::test::TestRegister::StackPointer))
                         .has_value() &&
                     Fixture.Block->addLiveIn(G0, rcc::LaneMask::fromRaw(0x3)).has_value(),
                 "valid target live-in and reserved register facts must be recorded");

  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                 "valid target builder insertion point must be set");
  auto Def = Builder.buildInstr(rcc::Opcode::fromValue(DefGPR64Opcode));
  Context.expect(Def && Def->addDef(*Reg).has_value() && Def->build(),
                 "register subclass must satisfy a target operand superclass");

  auto Early = Builder.buildInstr(rcc::Opcode::fromValue(EarlyClobberOpcode));
  Context.expect(
      Early &&
          Early->addDef(rcc::Register::fromPhysical(G0), {rcc::RegisterOperandFlag::EarlyClobber})
              .has_value() &&
          Early->addUse(rcc::Register::fromPhysical(G1)).has_value() && Early->build(),
      "disjoint physical registers must satisfy early-clobber");

  auto Call = Builder.buildInstr(rcc::Opcode::fromValue(CallOpcode));
  Context.expect(Call &&
                     Call->addRegisterMask(rcc::test::createTestCallPreservedMask()).has_value() &&
                     Call->build(),
                 "valid target call mask must build");
  Context.expect(rcc::MachineVerifier::verify(*Fixture.Function, Target).has_value(),
                 "complete valid target facts must pass MachineVerifier");
}

void testSubRegisterProjection(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction(Context, "subregister_projection", 2);
  if (!Fixture.Function)
    return;

  auto Type = rcc::LLT::makeInteger(64);
  auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
  if (!Reg)
    return;
  Context.expect(classify(Fixture.Function->registerInfo(), *Reg, rcc::test::TestRegisterBank::GPR,
                          rcc::test::TestRegisterClass::GPR64)
                     .has_value(),
                 "subregister source vreg must be classified");

  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                 "subregister builder insertion point must be set");
  auto Def = Builder.buildInstr(rcc::Opcode::fromValue(DefGPR32Opcode));
  Context.expect(Def && Def->addDef(*Reg, {}, rcc::test::TestSubReg::Low32).has_value() &&
                     Def->build(),
                 "projected GPR32Low class must satisfy the GPR32 operand class");
  Context.expect(rcc::MachineVerifier::verify(*Fixture.Function, Target).has_value(),
                 "valid virtual subregister projection must verify");
}

void testUnknownAndMalformedTargetFacts(TestContext &Context,
                                        const rcc::TargetDescription &Target) {
  {
    auto Fixture = createFunction(Context, "unknown_physreg", 3);
    auto Operand = rcc::MachineOperand::registerOperand(rcc::Register::fromPhysical(phys(99)),
                                                        {rcc::RegisterOperandFlag::Define});
    if (Fixture.Function && Operand) {
      std::vector<rcc::MachineOperand> Operands;
      Operands.push_back(std::move(*Operand));
      Context.expect(appendInstruction(Fixture, DefGPR64Opcode, std::move(Operands)) &&
                         !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                     "MachineVerifier must reject unknown physical registers");
    }
  }
  {
    auto Fixture = createFunction(Context, "physical_subreg", 4);
    auto Operand = rcc::MachineOperand::registerOperand(
        rcc::Register::fromPhysical(phys(rcc::test::TestRegister::G0)),
        {rcc::RegisterOperandFlag::Define}, rcc::test::TestSubReg::Low32);
    if (Fixture.Function && Operand) {
      std::vector<rcc::MachineOperand> Operands;
      Operands.push_back(std::move(*Operand));
      Context.expect(appendInstruction(Fixture, DefGPR64Opcode, std::move(Operands)) &&
                         !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                     "MachineVerifier must reject physical operands with SubRegIndex");
    }
  }
  {
    auto Fixture = createFunction(Context, "unknown_bank", 5);
    auto Type = rcc::LLT::makeInteger(64);
    if (!Fixture.Function || !Type)
      return;
    auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    Context.expect(Reg &&
                       Fixture.Function->registerInfo()
                           .setRegisterBank(*Reg, rcc::RegisterBankID::fromValue(99))
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                   "MachineVerifier must reject unknown virtual register bank IDs");
  }
  {
    auto Fixture = createFunction(Context, "wide_bank_value", 6);
    auto Type = rcc::LLT::makeInteger(128);
    if (!Fixture.Function || !Type)
      return;
    auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    Context.expect(Reg &&
                       Fixture.Function->registerInfo()
                           .setRegisterBank(*Reg, rcc::test::TestRegisterBank::GPR)
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                   "MachineVerifier must reject LLTs wider than their register bank");
  }
  {
    auto Fixture = createFunction(Context, "bad_function_livein", 7);
    Context.expect(Fixture.Function &&
                       Fixture.Function->registerInfo().addLiveIn(phys(99)).has_value() &&
                       !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                   "MachineVerifier must reject unknown function physical live-ins");
  }
  {
    auto Fixture = createFunction(Context, "bad_block_lanes", 8);
    Context.expect(
        Fixture.Function &&
            Fixture.Block
                ->addLiveIn(phys(rcc::test::TestRegister::G0), rcc::LaneMask::fromRaw(0x10))
                .has_value() &&
            !rcc::MachineVerifier::verify(*Fixture.Function, Target),
        "MachineVerifier must reject target-invalid block live-in lanes");
  }
  {
    auto Fixture = createFunction(Context, "bad_allocation_hint", 9);
    auto Type = rcc::LLT::makeInteger(64);
    auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    Context.expect(Reg &&
                       classify(Fixture.Function->registerInfo(), *Reg,
                                rcc::test::TestRegisterBank::FPR,
                                rcc::test::TestRegisterClass::FPR64)
                           .has_value() &&
                       Fixture.Function->registerInfo()
                           .addAllocationHint(*Reg, phys(rcc::test::TestRegister::G0))
                           .has_value() &&
                       !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                   "MachineVerifier must reject allocation hints outside a vreg class");
  }
  {
    auto Fixture = createFunction(Context, "reserved_allocation_hint", 14);
    auto Type = rcc::LLT::makeInteger(64);
    auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    const auto G0 = phys(rcc::test::TestRegister::G0);
    Context.expect(Reg &&
                       classify(Fixture.Function->registerInfo(), *Reg,
                                rcc::test::TestRegisterBank::GPR,
                                rcc::test::TestRegisterClass::GPR64)
                           .has_value() &&
                       Fixture.Function->registerInfo().addAllocationHint(*Reg, G0).has_value() &&
                       Fixture.Function->registerInfo().reservePhysicalRegister(G0).has_value() &&
                       !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                   "MachineVerifier must reject allocation hints that overlap reserved registers");
  }
  {
    auto Fixture = createFunction(Context, "incompatible_function_livein", 15);
    auto Type = rcc::LLT::makeInteger(64);
    auto Reg = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    Context.expect(
        Reg &&
            classify(Fixture.Function->registerInfo(), *Reg, rcc::test::TestRegisterBank::FPR,
                     rcc::test::TestRegisterClass::FPR64)
                .has_value() &&
            Fixture.Function->registerInfo()
                .addLiveIn(phys(rcc::test::TestRegister::G0), *Reg)
                .has_value() &&
            !rcc::MachineVerifier::verify(*Fixture.Function, Target),
        "MachineVerifier must reject a function live-in outside its initial vreg constraints");
  }
}

void testRegMaskEarlyClobberAndTies(TestContext &Context, const rcc::TargetDescription &Target) {
  {
    auto Fixture = createFunction(Context, "bad_regmask", 10);
    auto Mask = std::make_shared<const rcc::RegisterMask>(
        std::vector<std::uint64_t>{std::uint64_t{1} << 63U});
    auto Operand = rcc::MachineOperand::registerMask(std::move(Mask));
    if (Fixture.Function && Operand) {
      std::vector<rcc::MachineOperand> Operands;
      Operands.push_back(std::move(*Operand));
      Context.expect(appendInstruction(Fixture, CallOpcode, std::move(Operands)) &&
                         !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                     "MachineVerifier must reject RegMask bits for unknown registers");
    }
  }
  {
    auto Fixture = createFunction(Context, "early_overlap", 11);
    const auto G0 = phys(rcc::test::TestRegister::G0);
    rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
    Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                   "early-clobber builder insertion point must be set");
    auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(EarlyClobberOpcode));
    Context.expect(
        Pending &&
            Pending
                ->addDef(rcc::Register::fromPhysical(G0), {rcc::RegisterOperandFlag::EarlyClobber})
                .has_value() &&
            Pending->addUse(rcc::Register::fromPhysical(G0)).has_value() && !Pending->build() &&
            Fixture.Function->allInstructions().empty(),
        "Builder must reject overlapping early-clobber before publication");

    auto Def = rcc::MachineOperand::registerOperand(
        rcc::Register::fromPhysical(G0),
        {rcc::RegisterOperandFlag::Define, rcc::RegisterOperandFlag::EarlyClobber});
    auto Use = rcc::MachineOperand::registerOperand(rcc::Register::fromPhysical(G0),
                                                    {rcc::RegisterOperandFlag::Use});
    if (Def && Use) {
      std::vector<rcc::MachineOperand> Operands;
      Operands.push_back(std::move(*Def));
      Operands.push_back(std::move(*Use));
      Context.expect(appendInstruction(Fixture, EarlyClobberOpcode, std::move(Operands)) &&
                         !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                     "MachineVerifier must reject overlapping early-clobber");
    }
  }
  {
    auto Fixture = createFunction(Context, "incompatible_tie", 12);
    auto Type = rcc::LLT::makeInteger(64);
    auto GPR = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    auto FPR = Fixture.Function->registerInfo().createVirtualRegister(*Type);
    if (!GPR || !FPR)
      return;
    Context.expect(classify(Fixture.Function->registerInfo(), *GPR,
                            rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64)
                           .has_value() &&
                       classify(Fixture.Function->registerInfo(), *FPR,
                                rcc::test::TestRegisterBank::FPR,
                                rcc::test::TestRegisterClass::FPR64)
                           .has_value(),
                   "incompatible tied vregs must have individually valid classes");

    auto Def = rcc::MachineOperand::registerOperand(*GPR, {rcc::RegisterOperandFlag::Define});
    auto Use = rcc::MachineOperand::registerOperand(*FPR, {rcc::RegisterOperandFlag::Use});
    if (Def && Use) {
      std::vector<rcc::MachineOperand> Operands;
      Operands.push_back(std::move(*Def));
      Operands.push_back(std::move(*Use));
      Context.expect(appendInstruction(Fixture, TiedAnyOpcode, std::move(Operands),
                                       rcc::MachineOperandTie{0, 1}) &&
                         !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                     "MachineVerifier must reject tied classes without a physical intersection");
    }
  }
  {
    auto Fixture = createFunction(Context, "reserved_renamable", 13);
    const auto G0 = phys(rcc::test::TestRegister::G0);
    Context.expect(Fixture.Function->registerInfo().reservePhysicalRegister(G0).has_value(),
                   "reserved renamable test register must be reserved");

    rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
    Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                   "reserved renamable builder insertion point must be set");
    auto Pending = Builder.buildInstr(rcc::Opcode::fromValue(RenamableImplicitOpcode));
    Context.expect(Pending && !Pending->build(),
                   "Builder must reject a renamable reserved implicit register");

    const rcc::InstructionDescriptor *Descriptor =
        Target.instructionDescriptor(rcc::Opcode::fromValue(RenamableImplicitOpcode));
    if (Descriptor == nullptr)
      return;
    auto Implicits = Target.createDefaultImplicitOperands(*Descriptor);
    if (Implicits) {
      Context.expect(appendInstruction(Fixture, RenamableImplicitOpcode, std::move(*Implicits)) &&
                         !rcc::MachineVerifier::verify(*Fixture.Function, Target),
                     "MachineVerifier must reject a renamable reserved implicit register");
    }
  }
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "target-aware verifier test target must be valid");
  if (!Target)
    return Context.result();

  testValidTargetFacts(Context, *Target);
  testSubRegisterProjection(Context, *Target);
  testUnknownAndMalformedTargetFacts(Context, *Target);
  testRegMaskEarlyClobberAndTies(Context, *Target);
  return Context.result();
}
