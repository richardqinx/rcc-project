// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/InstructionDescriptor.h"

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
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

[[nodiscard]] rcc::OperandDescriptor registerDescriptor(rcc::RegisterOperandRole Role) {
  rcc::OperandDescriptor Descriptor;
  Descriptor.Kinds = {rcc::MachineOperand::Kind::Register};
  Descriptor.RegisterRole = Role;
  return Descriptor;
}

[[nodiscard]] rcc::InstructionDescriptor minimalDescriptor(std::uint32_t OpcodeValue,
                                                           std::string Name) {
  rcc::InstructionDescriptor Descriptor;
  Descriptor.InstructionOpcode = rcc::Opcode::fromValue(OpcodeValue);
  Descriptor.Name = std::move(Name);
  Descriptor.Flags = {rcc::InstructionFlag::ProducesCode};
  return Descriptor;
}

[[nodiscard]] rcc::Expected<rcc::MachineOperand> registerOperand(rcc::Register Reg,
                                                                 rcc::RegisterOperandFlags Flags,
                                                                 rcc::SubRegIndex SubReg = {},
                                                                 std::uint32_t TargetFlags = 0) {
  return rcc::MachineOperand::registerOperand(Reg, Flags, SubReg, TargetFlags);
}

[[nodiscard]] rcc::MachineMemOperandDesc loadDescription() {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {rcc::MemoryAccessFlag::Load};
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.Base = rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0));
  return Description;
}

[[nodiscard]] rcc::Expected<rcc::InstructionSet> createValidInstructionSet() {
  constexpr std::uint32_t AddOpcode = 1;
  constexpr std::uint32_t TwoAddressOpcode = 2;
  constexpr std::uint32_t LoadOpcode = 3;

  rcc::InstructionDescriptor Add = minimalDescriptor(AddOpcode, "TEST_ADDri");
  auto Def = registerDescriptor(rcc::RegisterOperandRole::Def);
  Def.RegBank = rcc::RegisterBankID::fromValue(7);
  Def.RegClass = rcc::RegisterClassID::fromValue(8);
  Def.Name = rcc::NamedOperandID::fromValue(1);

  auto Source = registerDescriptor(rcc::RegisterOperandRole::Use);
  Source.Name = rcc::NamedOperandID::fromValue(2);

  rcc::OperandDescriptor RegisterOrImmediate;
  RegisterOrImmediate.Kinds = {
      rcc::MachineOperand::Kind::Register,
      rcc::MachineOperand::Kind::Immediate,
  };
  RegisterOrImmediate.RegisterRole = rcc::RegisterOperandRole::Use;
  RegisterOrImmediate.ImmediatePredicate = rcc::ImmediatePredicateID::fromValue(1);
  RegisterOrImmediate.Name = rcc::NamedOperandID::fromValue(3);
  RegisterOrImmediate.AllowedTargetFlags = 0x3;

  Add.Operands.push_back(Def);
  Add.Operands.push_back(Source);
  Add.Operands.push_back(RegisterOrImmediate);
  Add.NumExplicitDefs = 1;
  Add.ImplicitDefs.push_back(
      rcc::ImplicitRegisterDescriptor{*rcc::PhysicalRegister::create(1), false, true});
  Add.ImplicitUses.push_back(rcc::ImplicitRegisterDescriptor{*rcc::PhysicalRegister::create(2)});

  rcc::InstructionDescriptor TwoAddress = minimalDescriptor(TwoAddressOpcode, "TEST_TWOADDR");
  auto TiedDef = registerDescriptor(rcc::RegisterOperandRole::Def);
  auto TiedUse = registerDescriptor(rcc::RegisterOperandRole::Use);
  TiedDef.DefaultTiedOperand = 1;
  TiedUse.DefaultTiedOperand = 0;
  TwoAddress.Operands.push_back(TiedDef);
  TwoAddress.Operands.push_back(TiedUse);
  TwoAddress.NumExplicitDefs = 1;

  rcc::InstructionDescriptor Load = minimalDescriptor(LoadOpcode, "TEST_LOAD");
  Load.Flags.set(rcc::InstructionFlag::MayLoad);
  Load.RequiresMemoryOperands = true;

  rcc::ImmediatePredicate Signed12;
  Signed12.ID = rcc::ImmediatePredicateID::fromValue(1);
  Signed12.Name = "register-or-simm12";
  Signed12.Predicate = [](const rcc::MachineOperand &Operand) {
    if (Operand.isRegister())
      return true;
    const auto *Value = Operand.getIf<std::int64_t>();
    return Value != nullptr && *Value >= -2048 && *Value <= 2047;
  };

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Add));
  Descriptors.push_back(std::move(TwoAddress));
  Descriptors.push_back(std::move(Load));
  std::vector<rcc::ImmediatePredicate> Predicates;
  Predicates.push_back(std::move(Signed12));
  return rcc::InstructionSet::create(std::move(Descriptors), std::move(Predicates));
}

void testDescriptorStructure(TestContext &Context) {
  Context.expect(!rcc::InstructionSet::create({}), "an empty instruction set must be rejected");

  auto DefAfterUses = minimalDescriptor(10, "BAD_DEF_ORDER");
  DefAfterUses.Operands.push_back(registerDescriptor(rcc::RegisterOperandRole::Def));
  Context.expect(!rcc::InstructionSet::create({DefAfterUses}),
                 "a definition outside the explicit def prefix must be rejected");

  auto AmbiguousVariadic = minimalDescriptor(11, "BAD_VARIADIC");
  auto Optional = registerDescriptor(rcc::RegisterOperandRole::Use);
  Optional.Optional = true;
  auto Variadic = registerDescriptor(rcc::RegisterOperandRole::Use);
  Variadic.VariadicGroup = true;
  AmbiguousVariadic.Operands.push_back(Optional);
  AmbiguousVariadic.Operands.push_back(Variadic);
  AmbiguousVariadic.Variadic = true;
  Context.expect(!rcc::InstructionSet::create({AmbiguousVariadic}),
                 "optional and variadic operand tails must not be combined");

  auto BadTie = minimalDescriptor(12, "BAD_TIE");
  auto Def = registerDescriptor(rcc::RegisterOperandRole::Def);
  auto Use = registerDescriptor(rcc::RegisterOperandRole::Use);
  Def.DefaultTiedOperand = 1;
  BadTie.Operands.push_back(Def);
  BadTie.Operands.push_back(Use);
  BadTie.NumExplicitDefs = 1;
  Context.expect(!rcc::InstructionSet::create({BadTie}), "default operand ties must be reciprocal");

  auto DuplicateNames = minimalDescriptor(13, "BAD_NAMES");
  auto FirstUse = registerDescriptor(rcc::RegisterOperandRole::Use);
  auto SecondUse = registerDescriptor(rcc::RegisterOperandRole::Use);
  FirstUse.Name = rcc::NamedOperandID::fromValue(9);
  SecondUse.Name = rcc::NamedOperandID::fromValue(9);
  DuplicateNames.Operands.push_back(FirstUse);
  DuplicateNames.Operands.push_back(SecondUse);
  Context.expect(!rcc::InstructionSet::create({DuplicateNames}),
                 "named operand IDs must be unique within an instruction");

  auto BadBranch = minimalDescriptor(14, "BAD_BRANCH");
  BadBranch.Flags.set(rcc::InstructionFlag::Branch);
  Context.expect(!rcc::InstructionSet::create({BadBranch}),
                 "branch descriptors must also be terminators");

  auto UnknownPredicate = minimalDescriptor(15, "BAD_PREDICATE");
  rcc::OperandDescriptor Immediate;
  Immediate.Kinds = {rcc::MachineOperand::Kind::Immediate};
  Immediate.ImmediatePredicate = rcc::ImmediatePredicateID::fromValue(99);
  UnknownPredicate.Operands.push_back(Immediate);
  Context.expect(!rcc::InstructionSet::create({UnknownPredicate}),
                 "operand descriptors must not reference unknown predicates");
}

void testOperandValidation(TestContext &Context) {
  auto InstructionSet = createValidInstructionSet();
  Context.expect(InstructionSet.has_value(), "the complete test instruction set must be valid");
  if (!InstructionSet)
    return;

  auto Function = rcc::MachineFunction::create("descriptor_operands",
                                               rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Function.has_value(), "descriptor test function must be valid");
  if (!Function)
    return;

  auto I64 = rcc::LLT::makeInteger(64);
  auto Destination = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto Source = (*Function)->registerInfo().createVirtualRegister(*I64);
  Context.expect(Destination && Source, "descriptor test virtual registers must be allocated");
  if (!Destination || !Source)
    return;

  auto &RegInfo = (*Function)->registerInfo();
  Context.expect(
      RegInfo.setRegisterBank(*Destination, rcc::RegisterBankID::fromValue(7)).has_value() &&
          RegInfo.setRegisterClass(*Destination, rcc::RegisterClassID::fromValue(8)).has_value(),
      "descriptor test destination constraints must be recorded");

  std::vector<rcc::MachineOperand> ValidOperands;
  ValidOperands.push_back(*registerOperand(*Destination, {rcc::RegisterOperandFlag::Define}));
  ValidOperands.push_back(*registerOperand(*Source, {rcc::RegisterOperandFlag::Use}));
  ValidOperands.push_back(rcc::MachineOperand::immediate(42, 0x1));
  Context.expect(
      InstructionSet->validateExplicitOperands(rcc::Opcode::fromValue(1), ValidOperands, RegInfo)
          .has_value(),
      "matching roles, constraints, flags, and predicate must validate");

  std::vector<rcc::MachineOperand> WrongRole;
  WrongRole.push_back(*registerOperand(*Destination, {rcc::RegisterOperandFlag::Use}));
  WrongRole.push_back(*registerOperand(*Source, {rcc::RegisterOperandFlag::Use}));
  WrongRole.push_back(rcc::MachineOperand::immediate(1));
  Context.expect(
      !InstructionSet->validateExplicitOperands(rcc::Opcode::fromValue(1), WrongRole, RegInfo),
      "a register role mismatch must be rejected");

  std::vector<rcc::MachineOperand> BadImmediate;
  BadImmediate.push_back(*registerOperand(*Destination, {rcc::RegisterOperandFlag::Define}));
  BadImmediate.push_back(*registerOperand(*Source, {rcc::RegisterOperandFlag::Use}));
  BadImmediate.push_back(rcc::MachineOperand::immediate(4096));
  Context.expect(
      !InstructionSet->validateExplicitOperands(rcc::Opcode::fromValue(1), BadImmediate, RegInfo),
      "an operand outside its immediate predicate must be rejected");

  std::vector<rcc::MachineOperand> BadTargetFlags;
  BadTargetFlags.push_back(*registerOperand(*Destination, {rcc::RegisterOperandFlag::Define}));
  BadTargetFlags.push_back(*registerOperand(*Source, {rcc::RegisterOperandFlag::Use}));
  BadTargetFlags.push_back(rcc::MachineOperand::immediate(1, 0x4));
  Context.expect(
      !InstructionSet->validateExplicitOperands(rcc::Opcode::fromValue(1), BadTargetFlags, RegInfo),
      "unregistered target operand flags must be rejected");
}

void testInstructionValidation(TestContext &Context) {
  auto InstructionSet = createValidInstructionSet();
  auto Function = rcc::MachineFunction::create("descriptor_instructions",
                                               rcc::MachineFunctionStableID::fromValue(2));
  Context.expect(InstructionSet && Function, "instruction validation setup must succeed");
  if (!InstructionSet || !Function)
    return;

  auto I64 = rcc::LLT::makeInteger(64);
  auto Reg = (*Function)->registerInfo().createVirtualRegister(*I64);
  Context.expect(Reg.has_value(), "instruction validation register must be allocated");
  if (!Reg)
    return;

  auto Tied = (*Function)->createInstruction(rcc::Opcode::fromValue(2));
  Context.expect(Tied.has_value(), "tied instruction must be allocated");
  if (!Tied)
    return;
  Context.expect(
      (*Tied)->addOperand(*registerOperand(*Reg, {rcc::RegisterOperandFlag::Define})).has_value() &&
          (*Tied)
              ->addOperand(*registerOperand(*Reg, {rcc::RegisterOperandFlag::Use}))
              .has_value() &&
          (*Tied)->tieOperands(0, 1).has_value(),
      "tied instruction operands must be materialized");
  Context.expect(
      InstructionSet->validateInstruction(**Tied, (*Function)->registerInfo()).has_value(),
      "a reciprocal descriptor-required tie must validate");

  auto Untied = (*Function)->createInstruction(rcc::Opcode::fromValue(2));
  Context.expect(Untied.has_value(), "untied instruction must be allocated");
  if (Untied) {
    Context.expect((*Untied)
                           ->addOperand(*registerOperand(*Reg, {rcc::RegisterOperandFlag::Define}))
                           .has_value() &&
                       (*Untied)
                           ->addOperand(*registerOperand(*Reg, {rcc::RegisterOperandFlag::Use}))
                           .has_value(),
                   "untied instruction operands must be added");
    Context.expect(!InstructionSet->validateInstruction(**Untied, (*Function)->registerInfo()),
                   "a missing descriptor-required tie must be rejected");
  }

  auto Destination = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto Source = (*Function)->registerInfo().createVirtualRegister(*I64);
  Context.expect(Destination && Source, "implicit operand validation registers must be allocated");
  if (!Destination || !Source)
    return;
  auto &RegInfo = (*Function)->registerInfo();
  Context.expect(
      RegInfo.setRegisterBank(*Destination, rcc::RegisterBankID::fromValue(7)).has_value() &&
          RegInfo.setRegisterClass(*Destination, rcc::RegisterClassID::fromValue(8)).has_value(),
      "implicit validation destination constraints must be recorded");

  auto WithImplicit = (*Function)->createInstruction(rcc::Opcode::fromValue(1));
  const rcc::InstructionDescriptor *AddDescriptor =
      InstructionSet->descriptor(rcc::Opcode::fromValue(1));
  Context.expect(WithImplicit && AddDescriptor != nullptr,
                 "implicit instruction setup must find its descriptor");
  if (!WithImplicit || AddDescriptor == nullptr)
    return;
  Context.expect(
      (*WithImplicit)
              ->addOperand(*registerOperand(*Destination, {rcc::RegisterOperandFlag::Define}))
              .has_value() &&
          (*WithImplicit)
              ->addOperand(*registerOperand(*Source, {rcc::RegisterOperandFlag::Use}))
              .has_value() &&
          (*WithImplicit)->addOperand(rcc::MachineOperand::immediate(7)).has_value(),
      "explicit operands for implicit validation must be added");
  auto DefaultImplicit = InstructionSet->createDefaultImplicitOperands(*AddDescriptor);
  Context.expect(DefaultImplicit.has_value(), "default implicit operands must be constructible");
  if (!DefaultImplicit)
    return;
  for (rcc::MachineOperand &Operand : *DefaultImplicit) {
    Context.expect((*WithImplicit)->addOperand(std::move(Operand)).has_value(),
                   "default implicit operand insertion must succeed");
  }
  Context.expect(InstructionSet->validateInstruction(**WithImplicit, RegInfo).has_value(),
                 "exact default implicit operands must validate");

  rcc::MachineOperand *ImplicitDef = (*WithImplicit)->operandAt(3);
  Context.expect(ImplicitDef != nullptr &&
                     ImplicitDef
                         ->setRegisterFlags(
                             {rcc::RegisterOperandFlag::Define, rcc::RegisterOperandFlag::Implicit})
                         .has_value(),
                 "implicit definition flags must be mutable for a negative test");
  Context.expect(!InstructionSet->validateInstruction(**WithImplicit, RegInfo),
                 "implicit renamable state must match the descriptor exactly");

  auto Load = (*Function)->createInstruction(rcc::Opcode::fromValue(3));
  Context.expect(Load.has_value(), "load instruction must be allocated");
  if (!Load)
    return;
  Context.expect(!InstructionSet->validateInstruction(**Load, RegInfo),
                 "an MMO-required load without an MMO must be rejected");
  auto MemOperand = (*Function)->createMemOperand(loadDescription());
  Context.expect(MemOperand.has_value() && (*Load)->addMemOperand(**MemOperand).has_value(),
                 "a function-owned load MMO must attach");
  Context.expect(InstructionSet->validateInstruction(**Load, RegInfo).has_value(),
                 "an MMO matching mayLoad must validate");
}

} // namespace

int main() {
  TestContext Context;
  testDescriptorStructure(Context);
  testOperandValidation(Context);
  testInstructionValidation(Context);
  return Context.result();
}
