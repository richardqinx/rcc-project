// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVCodeGen.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
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

struct InstructionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineInstr *Instruction = nullptr;
  rcc::Register Result;
};

[[nodiscard]] InstructionFixture createBinary(TestContext &Context,
                                              const rcc::TargetDescription &Target,
                                              rcc::GenericOpcode Generic, std::uint32_t Bits) {
  auto Function =
      rcc::MachineFunction::create("riscv_codegen", rcc::MachineFunctionStableID::fromValue(Bits));
  Context.expect(Function.has_value(), "RISC-V codegen function creation must succeed");
  if (!Function)
    return {};
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return {};
  auto Type = rcc::LLT::makeInteger(Bits);
  if (!Type)
    return {};
  auto Left = (*Function)->registerInfo().createVirtualRegister(*Type);
  auto Right = (*Function)->registerInfo().createVirtualRegister(*Type);
  auto Result = (*Function)->registerInfo().createVirtualRegister(*Type);
  if (!Left || !Right || !Result)
    return {};
  rcc::MachineIRBuilder Builder(**Function, Target);
  if (!Builder.setInsertPoint(**Block))
    return {};
  auto Pending = Builder.buildInstr(rcc::opcode(Generic));
  if (!Pending || !Pending->addDef(*Result) || !Pending->addUse(*Left) || !Pending->addUse(*Right))
    return {};
  auto Built = Pending->build();
  if (!Built)
    return {};
  return {std::move(*Function), *Built, *Result};
}

[[nodiscard]] bool constrainRegister(rcc::MachineFunction &Function, rcc::Register Reg,
                                     rcc::LLT Type) {
  const auto Bank = Type.isFloat() ? rcc::RegisterBankID::fromValue(
                                         static_cast<std::uint32_t>(rcc::riscv::RegisterBank::FPR))
                                   : rcc::RegisterBankID::fromValue(
                                         static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto Class = Type.isFloat()
                         ? rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(
                               Type.scalarBitWidth() == 32 ? rcc::riscv::RegisterClass::FPR32
                                                           : rcc::riscv::RegisterClass::FPR64))
                         : rcc::RegisterClassID::fromValue(
                               static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  return Function.registerInfo().setRegisterBank(Reg, Bank).has_value() &&
         Function.registerInfo().setRegisterClass(Reg, Class).has_value();
}

[[nodiscard]] bool constrainVirtualRegisters(rcc::MachineFunction &Function) {
  const auto Registers = Function.registerInfo().virtualRegisters();
  for (std::size_t Index = 0; Index < Registers.size(); ++Index) {
    const rcc::VirtualRegisterInfo &Info = Registers[Index];
    if (Info.IsTombstone)
      continue;
    auto Reg = rcc::Register::makeVirtual(Index);
    if (!Reg || !Info.Type || !constrainRegister(Function, *Reg, *Info.Type))
      return false;
  }
  return true;
}

[[nodiscard]] bool validatesSelectedFunction(const rcc::TargetDescription &Target,
                                             const rcc::MachineFunction &Function) {
  for (const rcc::MachineInstr *Instruction : Function.allInstructions()) {
    if (rcc::isGenericOpcode(Instruction->opcode()) ||
        !Target.validateInstruction(*Instruction, Function.registerInfo())) {
      return false;
    }
  }
  return Target.validateRegisterInfo(Function.registerInfo()).has_value();
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createUnaryFunction(TestContext &Context, const rcc::TargetDescription &Target,
                    rcc::GenericOpcode Generic, rcc::LLT DestinationType, rcc::LLT SourceType,
                    rcc::Register *DestinationOut = nullptr, rcc::Register *SourceOut = nullptr) {
  auto Function = rcc::MachineFunction::create(
      "riscv_unary",
      rcc::MachineFunctionStableID::fromValue(200 + static_cast<std::uint32_t>(Generic)));
  Context.expect(Function.has_value(), "unary test function creation must succeed");
  if (!Function)
    return nullptr;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return nullptr;
  auto Destination = (*Function)->registerInfo().createVirtualRegister(DestinationType);
  auto Source = (*Function)->registerInfo().createVirtualRegister(SourceType);
  if (!Destination || !Source)
    return nullptr;
  if (!constrainRegister(**Function, *Destination, DestinationType) ||
      !constrainRegister(**Function, *Source, SourceType))
    return nullptr;
  rcc::MachineIRBuilder Builder(**Function, Target);
  if (!Builder.setInsertPoint(**Block))
    return nullptr;
  auto Pending = Builder.buildInstr(rcc::opcode(Generic));
  if (!Pending || !Pending->addDef(*Destination) || !Pending->addUse(*Source) || !Pending->build())
    return nullptr;
  if (DestinationOut != nullptr)
    *DestinationOut = *Destination;
  if (SourceOut != nullptr)
    *SourceOut = *Source;
  return std::move(*Function);
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createCompareFunction(TestContext &Context, const rcc::TargetDescription &Target,
                      rcc::GenericOpcode Generic, rcc::LLT ValueType, bool Floating,
                      rcc::IntegerPredicate IntegerCondition,
                      rcc::FloatingPredicate FloatingCondition) {
  auto Function = rcc::MachineFunction::create(
      "riscv_compare",
      rcc::MachineFunctionStableID::fromValue(500 + static_cast<std::uint32_t>(Generic)));
  Context.expect(Function.has_value(), "comparison test function creation must succeed");
  if (!Function)
    return nullptr;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return nullptr;
  auto ResultType = rcc::LLT::makeInteger(1);
  if (!ResultType)
    return nullptr;
  auto Result = (*Function)->registerInfo().createVirtualRegister(*ResultType);
  auto Left = (*Function)->registerInfo().createVirtualRegister(ValueType);
  auto Right = (*Function)->registerInfo().createVirtualRegister(ValueType);
  if (!Result || !Left || !Right || !constrainRegister(**Function, *Result, *ResultType) ||
      !constrainRegister(**Function, *Left, ValueType) ||
      !constrainRegister(**Function, *Right, ValueType))
    return nullptr;
  rcc::MachineIRBuilder Builder(**Function, Target);
  if (!Builder.setInsertPoint(**Block))
    return nullptr;
  auto Pending = Builder.buildInstr(rcc::opcode(Generic));
  if (!Pending || !Pending->addDef(*Result))
    return nullptr;
  if (Floating) {
    if (!Pending->addFloatingPredicate(FloatingCondition))
      return nullptr;
  } else if (!Pending->addIntegerPredicate(IntegerCondition)) {
    return nullptr;
  }
  if (!Pending->addUse(*Left) || !Pending->addUse(*Right) || !Pending->build())
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFloatBinaryFunction(TestContext &Context, const rcc::TargetDescription &Target,
                          rcc::GenericOpcode Generic, rcc::LLT Type) {
  auto Function = rcc::MachineFunction::create(
      "riscv_float_binary",
      rcc::MachineFunctionStableID::fromValue(700 + static_cast<std::uint32_t>(Generic)));
  Context.expect(Function.has_value(), "floating binary test function creation must succeed");
  if (!Function)
    return nullptr;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return nullptr;
  auto Left = (*Function)->registerInfo().createVirtualRegister(Type);
  auto Right = (*Function)->registerInfo().createVirtualRegister(Type);
  auto Result = (*Function)->registerInfo().createVirtualRegister(Type);
  if (!Left || !Right || !Result || !constrainRegister(**Function, *Left, Type) ||
      !constrainRegister(**Function, *Right, Type) || !constrainRegister(**Function, *Result, Type))
    return nullptr;
  rcc::MachineIRBuilder Builder(**Function, Target);
  if (!Builder.setInsertPoint(**Block))
    return nullptr;
  auto Pending = Builder.buildInstr(rcc::opcode(Generic));
  if (!Pending || !Pending->addDef(*Result) || !Pending->addUse(*Left) ||
      !Pending->addUse(*Right) || !Pending->build())
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction> createRegisterOnlyFunction(
    TestContext &Context, const rcc::TargetDescription &Target, rcc::GenericOpcode Generic,
    std::initializer_list<rcc::LLT> DefinitionTypes, std::initializer_list<rcc::LLT> UseTypes) {
  auto Function = rcc::MachineFunction::create(
      "riscv_register_only",
      rcc::MachineFunctionStableID::fromValue(1000 + static_cast<std::uint32_t>(Generic)));
  Context.expect(Function.has_value(), "register-only test function creation must succeed");
  if (!Function)
    return nullptr;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return nullptr;
  rcc::MachineIRBuilder Builder(**Function, Target);
  if (!Builder.setInsertPoint(**Block))
    return nullptr;
  auto Pending = Builder.buildInstr(rcc::opcode(Generic));
  if (!Pending)
    return nullptr;
  for (rcc::LLT Type : DefinitionTypes) {
    auto Reg = (*Function)->registerInfo().createVirtualRegister(Type);
    if (!Reg || !Pending->addDef(*Reg))
      return nullptr;
  }
  for (rcc::LLT Type : UseTypes) {
    auto Reg = (*Function)->registerInfo().createVirtualRegister(Type);
    if (!Reg || !Pending->addUse(*Reg))
      return nullptr;
  }
  if (!Pending->build() || !constrainVirtualRegisters(**Function))
    return nullptr;
  return std::move(*Function);
}

void testClassification(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be available for codegen tests");
  if (!Target)
    return;
  auto Add32 = createBinary(Context, *Target, rcc::GenericOpcode::G_ADD, 32);
  auto Add64 = createBinary(Context, *Target, rcc::GenericOpcode::G_ADD, 64);
  Context.expect(Add32.Instruction != nullptr && Add64.Instruction != nullptr,
                 "RISC-V integer test instructions must be constructible");
  if (Add32.Instruction == nullptr || Add64.Instruction == nullptr)
    return;
  auto Add32Action =
      rcc::riscv::legalizerInfo().classify(*Add32.Instruction, *Add32.Function, *Target);
  auto Add64Action =
      rcc::riscv::legalizerInfo().classify(*Add64.Instruction, *Add64.Function, *Target);
  Context.expect(Add32Action && *Add32Action == rcc::LegalizationAction::Legal,
                 "RV64GC legalizer must accept i32 arithmetic");
  Context.expect(Add64Action && *Add64Action == rcc::LegalizationAction::Legal,
                 "RV64GC legalizer must accept i64 arithmetic");
  Context.expect(rcc::riscv::instructionSelector()
                     .canSelect(*Add32.Instruction, *Add32.Function, *Target)
                     .has_value(),
                 "RV64GC selector must pre-match i32 arithmetic");
  Context.expect(rcc::riscv::instructionSelector()
                     .canSelect(*Add64.Instruction, *Add64.Function, *Target)
                     .has_value(),
                 "RV64GC selector must pre-match i64 arithmetic");
  Context.expect(rcc::riscv::instructionSelector()
                     .select(*Add32.Instruction, *Add32.Function, *Target)
                     .has_value(),
                 "RV64GC selector must lower i32 arithmetic");
  Context.expect(Add32.Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                    rcc::riscv::InstructionOpcode::ADDW)),
                 "i32 G_ADD must select ADDW");
  Context.expect(rcc::riscv::instructionSelector()
                     .select(*Add64.Instruction, *Add64.Function, *Target)
                     .has_value(),
                 "RV64GC selector must lower i64 arithmetic");
  Context.expect(Add64.Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                    rcc::riscv::InstructionOpcode::ADD)),
                 "i64 G_ADD must select ADD");

  auto FloatFunction = rcc::MachineFunction::create("riscv_float_constants",
                                                    rcc::MachineFunctionStableID::fromValue(3));
  Context.expect(FloatFunction.has_value(), "floating constant function creation must succeed");
  if (!FloatFunction)
    return;
  auto FloatBlock = (*FloatFunction)->createBasicBlock();
  Context.expect(FloatBlock.has_value(), "floating constant block creation must succeed");
  if (!FloatBlock || !(*FloatFunction)->appendBlock(**FloatBlock))
    return;
  auto F32 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEESingle);
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  Context.expect(F32.has_value() && F64.has_value(),
                 "RV64GC floating constant types must be constructible");
  if (!F32 || !F64)
    return;
  auto F32Result = (*FloatFunction)->registerInfo().createVirtualRegister(*F32);
  auto F64Result = (*FloatFunction)->registerInfo().createVirtualRegister(*F64);
  Context.expect(F32Result.has_value() && F64Result.has_value(),
                 "RV64GC floating constant registers must be constructible");
  if (!F32Result || !F64Result)
    return;
  Context.expect((*FloatFunction)
                         ->registerInfo()
                         .setRegisterBank(*F32Result,
                                          rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(
                                              rcc::riscv::RegisterBank::FPR)))
                         .has_value() &&
                     (*FloatFunction)
                         ->registerInfo()
                         .setRegisterClass(
                             *F32Result, rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(
                                             rcc::riscv::RegisterClass::FPR32)))
                         .has_value(),
                 "f32 destination must have an FPR32 bank and class");
  Context.expect((*FloatFunction)
                         ->registerInfo()
                         .setRegisterBank(*F64Result,
                                          rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(
                                              rcc::riscv::RegisterBank::FPR)))
                         .has_value() &&
                     (*FloatFunction)
                         ->registerInfo()
                         .setRegisterClass(
                             *F64Result, rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(
                                             rcc::riscv::RegisterClass::FPR64)))
                         .has_value(),
                 "f64 destination must have an FPR64 bank and class");
  rcc::MachineIRBuilder FloatBuilder(**FloatFunction, *Target);
  if (!FloatBuilder.setInsertPoint(**FloatBlock))
    return;
  const std::array<std::uint64_t, 1> SingleWords{0x3f800000U};
  const std::array<std::uint64_t, 1> DoubleWords{0x3ff0000000000000ULL};
  auto SingleBits = rcc::FloatingPointBits::create(rcc::FloatSemantics::IEEESingle, SingleWords);
  auto DoubleBits = rcc::FloatingPointBits::create(rcc::FloatSemantics::IEEEDouble, DoubleWords);
  Context.expect(SingleBits.has_value() && DoubleBits.has_value(),
                 "floating constant payloads must be constructible");
  if (!SingleBits || !DoubleBits)
    return;
  auto SinglePending = FloatBuilder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_FCONSTANT));
  auto DoublePending = FloatBuilder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_FCONSTANT));
  Context.expect(SinglePending.has_value() && DoublePending.has_value(),
                 "floating constant instructions must be constructible");
  if (!SinglePending || !DoublePending)
    return;
  Context.expect(SinglePending->addDef(*F32Result).has_value() &&
                     SinglePending->addFloatingImmediate(*SingleBits).has_value() &&
                     SinglePending->build().has_value(),
                 "f32 floating constant must be buildable");
  Context.expect(DoublePending->addDef(*F64Result).has_value() &&
                     DoublePending->addFloatingImmediate(*DoubleBits).has_value() &&
                     DoublePending->build().has_value(),
                 "f64 floating constant must be buildable");
  if (FloatFunction->get()->firstBlock()->firstInstruction() == nullptr)
    return;
  auto Instructions = FloatFunction->get()->allInstructions();
  auto It = Instructions.begin();
  rcc::MachineInstr *F32Instruction = It == Instructions.end() ? nullptr : *It;
  if (It != Instructions.end())
    ++It;
  rcc::MachineInstr *F64Instruction = It == Instructions.end() ? nullptr : *It;
  Context.expect(F32Instruction != nullptr && F64Instruction != nullptr,
                 "floating constant instructions must remain in layout order");
  if (F32Instruction == nullptr || F64Instruction == nullptr)
    return;
  auto F32Action = rcc::riscv::legalizerInfo().classify(*F32Instruction, **FloatFunction, *Target);
  auto F64Action = rcc::riscv::legalizerInfo().classify(*F64Instruction, **FloatFunction, *Target);
  Context.expect(F32Action && *F32Action == rcc::LegalizationAction::Legal,
                 "RV64GC legalizer must accept f32 constants");
  Context.expect(F64Action && *F64Action == rcc::LegalizationAction::Legal,
                 "RV64GC legalizer must accept f64 constants");
  auto F32Selection =
      rcc::riscv::instructionSelector().select(*F32Instruction, **FloatFunction, *Target);
  auto F64Selection =
      rcc::riscv::instructionSelector().select(*F64Instruction, **FloatFunction, *Target);
  Context.expect(F32Selection.has_value(), "RV64GC selector must lower f32 constants");
  Context.expect(F64Selection.has_value(), "RV64GC selector must lower f64 constants");
  bool HasFMVWX = false;
  bool HasFMVDX = false;
  for (const rcc::MachineInstr *Instruction : FloatFunction->get()->allInstructions()) {
    HasFMVWX |=
        Instruction->opcode() ==
        rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::FMV_W_X));
    HasFMVDX |=
        Instruction->opcode() ==
        rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::FMV_D_X));
  }
  Context.expect(HasFMVWX, "f32 constants must use FMV.W.X materialization");
  Context.expect(HasFMVDX, "f64 constants must use FMV.D.X materialization");
}

void testNarrowIntegerSelection(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return;
  struct BinaryCase final {
    rcc::GenericOpcode Generic;
    std::uint32_t Bits;
    rcc::riscv::InstructionOpcode RequiredOpcode;
  };
  const std::array Cases{
      BinaryCase{rcc::GenericOpcode::G_ADD, 1, rcc::riscv::InstructionOpcode::ADDW},
      BinaryCase{rcc::GenericOpcode::G_UDIV, 8, rcc::riscv::InstructionOpcode::DIVUW},
      BinaryCase{rcc::GenericOpcode::G_ASHR, 16, rcc::riscv::InstructionOpcode::SRAW},
      BinaryCase{rcc::GenericOpcode::G_SMIN, 24, rcc::riscv::InstructionOpcode::SLT},
  };
  for (const BinaryCase &TestCase : Cases) {
    auto Fixture = createBinary(Context, *Target, TestCase.Generic, TestCase.Bits);
    if (!Fixture.Function || Fixture.Instruction == nullptr ||
        !constrainVirtualRegisters(*Fixture.Function)) {
      Context.expect(false, "narrow integer fixture must be constructible");
      continue;
    }
    auto Action =
        rcc::riscv::legalizerInfo().classify(*Fixture.Instruction, *Fixture.Function, *Target);
    Context.expect(Action && *Action == rcc::LegalizationAction::Legal,
                   "i1-i24 scalar operations must be legal on RV64GC GPR containers");
    auto Selection =
        rcc::riscv::instructionSelector().select(*Fixture.Instruction, *Fixture.Function, *Target);
    Context.expect(Selection.has_value(), "narrow integer operation must select");
    bool FoundRequiredOpcode = false;
    for (const rcc::MachineInstr *Instruction : Fixture.Function->allInstructions()) {
      FoundRequiredOpcode |=
          Instruction->opcode() ==
          rcc::Opcode::fromValue(static_cast<std::uint32_t>(TestCase.RequiredOpcode));
    }
    Context.expect(FoundRequiredOpcode,
                   "narrow integer selection must use the required RV64 instruction");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Fixture.Function),
                   "narrow integer selection must satisfy target descriptors");
  }

  auto I7 = rcc::LLT::makeInteger(7);
  auto I24 = rcc::LLT::makeInteger(24);
  if (!I7 || !I24)
    return;
  auto Compare = createCompareFunction(Context, *Target, rcc::GenericOpcode::G_ICMP, *I7, false,
                                       rcc::IntegerPredicate::SignedLessThan,
                                       rcc::FloatingPredicate::AlwaysFalse);
  if (Compare && Compare->firstBlock()->firstInstruction() != nullptr) {
    rcc::MachineInstr *Instruction = Compare->firstBlock()->firstInstruction();
    auto Action = rcc::riscv::legalizerInfo().classify(*Instruction, *Compare, *Target);
    auto Lowered = Action
                       ? rcc::riscv::legalizerInfo().lower(*Instruction, *Compare, *Target, *Action)
                       : rcc::Expected<void>(std::unexpected(Action.error()));
    auto LegalAction = rcc::riscv::legalizerInfo().classify(*Instruction, *Compare, *Target);
    bool Selected = Lowered && LegalAction && *LegalAction == rcc::LegalizationAction::Legal &&
                    constrainVirtualRegisters(*Compare);
    const auto Instructions = Compare->allInstructions();
    for (rcc::MachineInstr *Candidate : Instructions) {
      if (Selected && rcc::isGenericOpcode(Candidate->opcode()))
        Selected =
            rcc::riscv::instructionSelector().select(*Candidate, *Compare, *Target).has_value();
    }
    Context.expect(Action && *Action == rcc::LegalizationAction::Custom && Selected,
                   "signed i7 comparisons must legalize to RV64 containers and select");
    Context.expect(Selected && validatesSelectedFunction(*Target, *Compare),
                   "selected i7 comparison must satisfy target descriptors");
  }

  auto Extension = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_SEXT, *I24, *I7);
  if (Extension && Extension->firstBlock()->firstInstruction() != nullptr) {
    rcc::MachineInstr *Instruction = Extension->firstBlock()->firstInstruction();
    auto Action = rcc::riscv::legalizerInfo().classify(*Instruction, *Extension, *Target);
    auto Selection = rcc::riscv::instructionSelector().select(*Instruction, *Extension, *Target);
    Context.expect(Action && *Action == rcc::LegalizationAction::Legal && Selection,
                   "i7-to-i24 sign extension must select through a 32-bit container");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Extension),
                   "selected narrow extension must satisfy target descriptors");
  }

  auto UnsupportedMulHigh = createBinary(Context, *Target, rcc::GenericOpcode::G_SMULH, 16);
  if (UnsupportedMulHigh.Function && UnsupportedMulHigh.Instruction != nullptr) {
    auto Action = rcc::riscv::legalizerInfo().classify(*UnsupportedMulHigh.Instruction,
                                                       *UnsupportedMulHigh.Function, *Target);
    Context.expect(Action && *Action == rcc::LegalizationAction::Unsupported,
                   "non-native multiply-high widths must be explicitly unsupported");
  }
}

void testScalarSelection(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be available for scalar selection");
  if (!Target)
    return;

  auto I32 = rcc::LLT::makeInteger(32);
  auto I64 = rcc::LLT::makeInteger(64);
  auto F32 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEESingle);
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  auto I1 = rcc::LLT::makeInteger(1);
  Context.expect(I32 && I64 && F32 && F64 && I1, "scalar test types must be constructible");
  if (!I32 || !I64 || !F32 || !F64 || !I1)
    return;

  auto AnyExt = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_ANYEXT, *I64, *I32);
  auto Sext = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_SEXT, *I64, *I32);
  auto Zext = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_ZEXT, *I64, *I32);
  auto Trunc = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_TRUNC, *I32, *I64);
  for (const auto &Entry : {std::pair{AnyExt.get(), rcc::GenericOpcode::G_ANYEXT},
                            std::pair{Sext.get(), rcc::GenericOpcode::G_SEXT},
                            std::pair{Zext.get(), rcc::GenericOpcode::G_ZEXT},
                            std::pair{Trunc.get(), rcc::GenericOpcode::G_TRUNC}}) {
    Context.expect(Entry.first != nullptr, "integer extension test instruction must exist");
    if (Entry.first == nullptr)
      continue;
    auto *Instruction = Entry.first->firstBlock()->firstInstruction();
    auto Selection = rcc::riscv::instructionSelector().select(*Instruction, *Entry.first, *Target);
    const std::string_view Name = Entry.second == rcc::GenericOpcode::G_ANYEXT ? "anyext"
                                  : Entry.second == rcc::GenericOpcode::G_SEXT ? "sext"
                                  : Entry.second == rcc::GenericOpcode::G_ZEXT ? "zext"
                                                                               : "trunc";
    Context.expect(Selection.has_value(),
                   std::string("RV64GC ") + std::string(Name) + " must select");
    if (!Selection)
      continue;
    if (Entry.second == rcc::GenericOpcode::G_ANYEXT || Entry.second == rcc::GenericOpcode::G_TRUNC)
      Context.expect(Instruction->opcode() == rcc::opcode(rcc::GenericOpcode::COPY),
                     "anyext/trunc must select COPY");
    else
      Context.expect(!rcc::isGenericOpcode(Instruction->opcode()),
                     "integer extension must not remain generic");
  }
  if (Zext) {
    Context.expect(Zext->allInstructions().size() == 2,
                   "zero extension must emit a two-instruction RV64 sequence");
    if (Zext->allInstructions().size() == 2) {
      auto It = Zext->allInstructions().begin();
      Context.expect((*It)->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                            rcc::riscv::InstructionOpcode::SLLI)),
                     "zero extension must shift left first");
      ++It;
      Context.expect((*It)->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                            rcc::riscv::InstructionOpcode::SRLI)),
                     "zero extension must shift right logically second");
    }
  }

  auto FPExt = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_FPEXT, *F64, *F32);
  auto FPTrunc = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_FPTRUNC, *F32, *F64);
  auto FPToSI = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_FPTOSI, *I64, *F32);
  auto SIToFP = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_SITOFP, *F64, *I32);
  auto Bitcast = createUnaryFunction(Context, *Target, rcc::GenericOpcode::G_BITCAST, *I32, *F32);
  if (FPExt) {
    auto *Instruction = FPExt->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *FPExt, *Target).has_value(),
        "fpext must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FCVT_D_S)),
                   "fpext must select FCVT.D.S");
  }
  if (FPTrunc) {
    auto *Instruction = FPTrunc->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *FPTrunc, *Target).has_value(),
        "fptrunc must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FCVT_S_D)),
                   "fptrunc must select FCVT.S.D");
  }
  if (FPToSI) {
    auto *Instruction = FPToSI->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *FPToSI, *Target).has_value(),
        "fptosi must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FCVT_L_S)),
                   "fptosi must select FCVT.L.S");
  }
  if (SIToFP) {
    auto *Instruction = SIToFP->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *SIToFP, *Target).has_value(),
        "sitofp must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FCVT_D_W)),
                   "sitofp must select FCVT.D.W");
  }
  if (Bitcast) {
    auto *Instruction = Bitcast->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *Bitcast, *Target).has_value(),
        "float-to-integer bitcast must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FMV_X_W)),
                   "float-to-integer bitcast must select FMV.X.W");
  }
}

void testComparisonsAndControl(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be available for comparison selection");
  if (!Target)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  auto F32 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEESingle);
  auto Pointer = rcc::LLT::makePointer(0, 64);
  if (!I64 || !F32 || !Pointer)
    return;

  auto Equal =
      createCompareFunction(Context, *Target, rcc::GenericOpcode::G_ICMP, *I64, false,
                            rcc::IntegerPredicate::Equal, rcc::FloatingPredicate::AlwaysFalse);
  auto LessEqual = createCompareFunction(Context, *Target, rcc::GenericOpcode::G_ICMP, *I64, false,
                                         rcc::IntegerPredicate::SignedLessEqual,
                                         rcc::FloatingPredicate::AlwaysFalse);
  if (Equal) {
    auto *Instruction = Equal->firstBlock()->firstInstruction();
    const rcc::Register *NarrowResult = Instruction->operandAt(0)->getIf<rcc::Register>();
    auto WideResult = Equal->registerInfo().createVirtualRegister(*I64);
    rcc::MachineIRBuilder Builder(*Equal, *Target);
    if (NarrowResult == nullptr || !WideResult || !constrainRegister(*Equal, *WideResult, *I64) ||
        !Builder.setInsertPoint(*Equal->firstBlock()))
      return;
    auto Extension = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ANYEXT));
    if (!Extension || !Extension->addDef(*WideResult) || !Extension->addUse(*NarrowResult))
      return;
    auto BuiltExtension = Extension->build();
    if (!BuiltExtension)
      return;
    auto Action = rcc::riscv::legalizerInfo().classify(*Instruction, *Equal, *Target);
    auto Lowered = Action
                       ? rcc::riscv::legalizerInfo().lower(*Instruction, *Equal, *Target, *Action)
                       : rcc::Expected<void>(std::unexpected(Action.error()));
    const rcc::Register *LegalResult = Instruction->operandAt(0)->getIf<rcc::Register>();
    Context.expect(Lowered && (*BuiltExtension)->parent() == nullptr && LegalResult != nullptr &&
                       *LegalResult == *WideResult,
                   "RV64 integer comparison legalization must fold a single result extension");
    auto Selection = rcc::riscv::instructionSelector().select(*Instruction, *Equal, *Target);
    Context.expect(Action && *Action == rcc::LegalizationAction::Custom && Lowered && Selection,
                   "integer equality must legalize its result container and select");
    rcc::MachineInstr *FirstSelected = Equal->firstBlock()->firstInstruction();
    rcc::MachineInstr *SecondSelected = FirstSelected == nullptr ? nullptr : FirstSelected->next();
    Context.expect(SecondSelected != nullptr && SecondSelected->next() == nullptr,
                   "integer equality must emit XOR and zero-test instructions");
    if (SecondSelected != nullptr && SecondSelected->next() == nullptr) {
      Context.expect(FirstSelected->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                    rcc::riscv::InstructionOpcode::XOR)),
                     "integer equality must start with XOR");
      Context.expect(SecondSelected->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                     rcc::riscv::InstructionOpcode::SLTIU)),
                     "integer equality must finish with SLTIU");
    }
  }
  if (LessEqual) {
    auto *Instruction = LessEqual->firstBlock()->firstInstruction();
    auto Action = rcc::riscv::legalizerInfo().classify(*Instruction, *LessEqual, *Target);
    auto Lowered =
        Action ? rcc::riscv::legalizerInfo().lower(*Instruction, *LessEqual, *Target, *Action)
               : rcc::Expected<void>(std::unexpected(Action.error()));
    Context.expect(Action && *Action == rcc::LegalizationAction::Custom && Lowered &&
                       constrainVirtualRegisters(*LessEqual) &&
                       rcc::riscv::instructionSelector().select(*Instruction, *LessEqual, *Target),
                   "signed less-equal must legalize its result container and select");
    Context.expect(LessEqual->allInstructions().size() == 2,
                   "signed less-equal must emit compare and inversion");
  }

  auto Ordered =
      createCompareFunction(Context, *Target, rcc::GenericOpcode::G_FCMP, *F32, true,
                            rcc::IntegerPredicate::Equal, rcc::FloatingPredicate::OrderedEqual);
  auto UnorderedEqual =
      createCompareFunction(Context, *Target, rcc::GenericOpcode::G_FCMP, *F32, true,
                            rcc::IntegerPredicate::Equal, rcc::FloatingPredicate::UnorderedEqual);
  if (Ordered) {
    auto *Instruction = Ordered->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *Ordered, *Target).has_value(),
        "ordered floating equality must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FEQ_S)),
                   "ordered floating equality must select FEQ.S");
  }
  if (UnorderedEqual) {
    auto *Instruction = UnorderedEqual->firstBlock()->firstInstruction();
    Context.expect(rcc::riscv::instructionSelector()
                       .select(*Instruction, *UnorderedEqual, *Target)
                       .has_value(),
                   "unordered floating equality must select");
    Context.expect(UnorderedEqual->allInstructions().size() == 4,
                   "unordered floating equality must emit ordered compare sequence");
  }

  auto FMin = createFloatBinaryFunction(Context, *Target, rcc::GenericOpcode::G_FMINNUM, *F32);
  if (FMin) {
    auto *Instruction = FMin->firstBlock()->firstInstruction();
    Context.expect(
        rcc::riscv::instructionSelector().select(*Instruction, *FMin, *Target).has_value(),
        "floating minnum must select");
    Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                rcc::riscv::InstructionOpcode::FMIN_S)),
                   "floating minnum must select FMIN.S");
  }

  auto ControlFunction =
      rcc::MachineFunction::create("riscv_control", rcc::MachineFunctionStableID::fromValue(900));
  Context.expect(ControlFunction.has_value(), "control test function creation must succeed");
  if (!ControlFunction)
    return;
  auto Block = (*ControlFunction)->createBasicBlock();
  if (!Block || !(*ControlFunction)->appendBlock(**Block))
    return;
  auto Address = (*ControlFunction)->registerInfo().createVirtualRegister(*Pointer);
  if (!Address || !constrainRegister(**ControlFunction, *Address, *Pointer))
    return;
  rcc::MachineIRBuilder Builder(**ControlFunction, *Target);
  if (!Builder.setInsertPoint(**Block))
    return;
  auto Indirect = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_BRINDIRECT));
  auto Fence = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_FENCE));
  auto Trap = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_TRAP));
  if (!Indirect || !Fence || !Trap || !Indirect->addUse(*Address) || !Indirect->build() ||
      !Fence->addImmediate(7) || !Fence->addImmediate(0) || !Fence->build() || !Trap->build())
    return;
  auto *IndirectInstruction = (*ControlFunction)->firstBlock()->firstInstruction();
  Context.expect(rcc::riscv::instructionSelector()
                     .select(*IndirectInstruction, **ControlFunction, *Target)
                     .has_value(),
                 "indirect branch must select");
  Context.expect(IndirectInstruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                      rcc::riscv::InstructionOpcode::JALR)),
                 "indirect branch must select JALR");
  auto *FenceInstruction = IndirectInstruction->next();
  Context.expect(rcc::riscv::instructionSelector()
                     .select(*FenceInstruction, **ControlFunction, *Target)
                     .has_value(),
                 "fence must select");
  Context.expect(FenceInstruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                   rcc::riscv::InstructionOpcode::FENCE)),
                 "fence must select FENCE");
  auto *TrapInstruction = FenceInstruction->next();
  Context.expect(rcc::riscv::instructionSelector()
                     .select(*TrapInstruction, **ControlFunction, *Target)
                     .has_value(),
                 "trap must select");
  Context.expect(TrapInstruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                  rcc::riscv::InstructionOpcode::EBREAK)),
                 "trap must select EBREAK");
}

void testCompositeScalarSelection(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be available for composite selection");
  if (!Target)
    return;
  auto I1 = rcc::LLT::makeInteger(1);
  auto I32 = rcc::LLT::makeInteger(32);
  auto I64 = rcc::LLT::makeInteger(64);
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  if (!I1 || !I32 || !I64 || !F64)
    return;

  auto DivRem = createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_SDIVREM,
                                           {*I64, *I64}, {*I64, *I64});
  auto MultiplyHigh = createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_UMULH,
                                                 {*I32}, {*I32, *I32});
  auto Minimum = createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_SMIN, {*I64},
                                            {*I64, *I64});
  auto Absolute =
      createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_ABS, {*I32}, {*I32});
  auto IntegerSelect = createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_SELECT,
                                                  {*I64}, {*I1, *I64, *I64});
  auto FloatSelect = createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_SELECT,
                                                {*F64}, {*I1, *F64, *F64});
  auto CopySign = createRegisterOnlyFunction(Context, *Target, rcc::GenericOpcode::G_FCOPYSIGN,
                                             {*F64}, {*F64, *F64});
  const std::array Functions{DivRem.get(),        MultiplyHigh.get(), Minimum.get(), Absolute.get(),
                             IntegerSelect.get(), FloatSelect.get(),  CopySign.get()};
  for (rcc::MachineFunction *Function : Functions) {
    Context.expect(Function != nullptr, "composite scalar fixture must be complete");
    if (Function == nullptr)
      continue;
    rcc::MachineInstr *Instruction = Function->firstBlock()->firstInstruction();
    auto Classification = rcc::riscv::legalizerInfo().classify(*Instruction, *Function, *Target);
    Context.expect(Classification && *Classification == rcc::LegalizationAction::Legal,
                   "composite scalar operation must be legal on RV64GC");
    auto Selection = rcc::riscv::instructionSelector().select(*Instruction, *Function, *Target);
    Context.expect(Selection.has_value(), "composite scalar operation must select on RV64GC");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Function),
                   "composite scalar selection must satisfy all target descriptors");
  }
  if (DivRem) {
    const auto Instructions = DivRem->allInstructions();
    Context.expect(Instructions.size() == 2, "G_SDIVREM must emit separate quotient and remainder");
    if (Instructions.size() == 2) {
      auto It = Instructions.begin();
      Context.expect((*It)->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                            rcc::riscv::InstructionOpcode::DIV)),
                     "G_SDIVREM quotient must use DIV");
      ++It;
      Context.expect((*It)->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                            rcc::riscv::InstructionOpcode::REM)),
                     "G_SDIVREM remainder must use REM");
    }
  }
  if (MultiplyHigh) {
    Context.expect(MultiplyHigh->allInstructions().size() >= 4,
                   "i32 G_UMULH must explicitly zero-extend before multiplication");
  }
  if (CopySign) {
    Context.expect(CopySign->firstBlock()->firstInstruction()->opcode() ==
                       rcc::Opcode::fromValue(
                           static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::FSGNJ_D)),
                   "G_FCOPYSIGN must select FSGNJ.D");
  }
}

void testOverflowSelection(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return;
  auto I1 = rcc::LLT::makeInteger(1);
  auto I32 = rcc::LLT::makeInteger(32);
  auto I64 = rcc::LLT::makeInteger(64);
  if (!I1 || !I32 || !I64)
    return;
  const std::array Opcodes{rcc::GenericOpcode::G_UADDO, rcc::GenericOpcode::G_USUBO,
                           rcc::GenericOpcode::G_SADDO, rcc::GenericOpcode::G_SSUBO,
                           rcc::GenericOpcode::G_UMULO, rcc::GenericOpcode::G_SMULO};
  for (rcc::GenericOpcode Generic : Opcodes) {
    const rcc::LLT Type = Generic == rcc::GenericOpcode::G_UMULO ? *I32 : *I64;
    auto Function =
        createRegisterOnlyFunction(Context, *Target, Generic, {Type, *I1}, {Type, Type});
    Context.expect(Function != nullptr, "overflow fixture must be complete");
    if (!Function)
      continue;
    rcc::MachineInstr *Instruction = Function->firstBlock()->firstInstruction();
    auto Selection = rcc::riscv::instructionSelector().select(*Instruction, *Function, *Target);
    Context.expect(Selection.has_value(), "RV64GC overflow operation must select");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Function),
                   "overflow sequence must satisfy all target descriptors");
    Context.expect(Function->allInstructions().size() >= 2,
                   "overflow operation must materialize its status result");
  }
  const std::array ExtendedOpcodes{rcc::GenericOpcode::G_UADDE, rcc::GenericOpcode::G_USUBE,
                                   rcc::GenericOpcode::G_SADDE, rcc::GenericOpcode::G_SSUBE};
  for (rcc::GenericOpcode Generic : ExtendedOpcodes) {
    auto Function =
        createRegisterOnlyFunction(Context, *Target, Generic, {*I64, *I1}, {*I64, *I64, *I1});
    Context.expect(Function != nullptr, "extended overflow fixture must be complete");
    if (!Function)
      continue;
    rcc::MachineInstr *Instruction = Function->firstBlock()->firstInstruction();
    auto Selection = rcc::riscv::instructionSelector().select(*Instruction, *Function, *Target);
    Context.expect(Selection.has_value(), "extended RV64GC overflow operation must select");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Function),
                   "extended overflow sequence must satisfy all target descriptors");
  }
}

void testAtomicCompareExchangeLegalization(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Function = rcc::MachineFunction::create("riscv_atomic_cmpxchg",
                                               rcc::MachineFunctionStableID::fromValue(1299));
  if (!Target || !Function)
    return;
  auto Block = (*Function)->createBasicBlock();
  auto I1 = rcc::LLT::makeAnyScalar(1);
  auto I8 = rcc::LLT::makeAnyScalar(8);
  auto I64 = rcc::LLT::makeAnyScalar(64);
  auto Pointer = rcc::LLT::makePointer(0, 64);
  auto Size = rcc::MemorySize::fixed(1);
  auto Align = rcc::Alignment::create(1);
  if (!Block || !(*Function)->appendBlock(**Block) || !I1 || !I8 || !I64 || !Pointer || !Size ||
      !Align)
    return;
  auto Address = (*Function)->registerInfo().createVirtualRegister(*Pointer);
  auto Compare = (*Function)->registerInfo().createVirtualRegister(*I8);
  auto NewValue = (*Function)->registerInfo().createVirtualRegister(*I8);
  auto Result = (*Function)->registerInfo().createVirtualRegister(*I8);
  auto Extended = (*Function)->registerInfo().createVirtualRegister(*I64);
  if (!Address || !Compare || !NewValue || !Result || !Extended)
    return;

  rcc::MachineIRBuilder Builder(**Function, *Target);
  if (!Builder.setInsertPoint(**Block))
    return;
  auto AddressDefinition = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_IMPLICIT_DEF));
  if (!AddressDefinition || !AddressDefinition->addDef(*Address) || !AddressDefinition->build())
    return;
  const auto BuildConstant = [&](rcc::Register Destination, std::uint64_t Value) -> bool {
    auto Immediate = rcc::IntegerImmediate::fromUnsigned(8, Value);
    auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_CONSTANT));
    return Immediate && Pending && Pending->addDef(Destination) &&
           Pending->addIntegerImmediate(std::move(*Immediate)) && Pending->build();
  };
  if (!BuildConstant(*Compare, 0) || !BuildConstant(*NewValue, 1))
    return;

  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store};
  Description.AtomicKind = rcc::AtomicOperation::CompareExchange;
  Description.SuccessOrdering = rcc::AtomicOrdering::Monotonic;
  Description.Size = *Size;
  Description.MemoryType = *I8;
  Description.Align = *Align;
  Description.Base = rcc::MemoryBase::stack();
  auto Memory = (*Function)->createMemOperand(std::move(Description));
  auto Atomic = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ATOMIC_CMPXCHG));
  if (!Memory || !Atomic || !Atomic->addDef(*Result) || !Atomic->addUse(*Address) ||
      !Atomic->addUse(*Compare) || !Atomic->addUse(*NewValue) || !Atomic->addMemOperand(**Memory))
    return;
  auto BuiltAtomic = Atomic->build();
  auto Extension = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ANYEXT));
  if (!BuiltAtomic || !Extension || !Extension->addDef(*Extended) || !Extension->addUse(*Result))
    return;
  auto BuiltExtension = Extension->build();
  if (!BuiltExtension)
    return;

  auto Action = rcc::riscv::legalizerInfo().classify(**BuiltAtomic, **Function, *Target);
  auto Lowered =
      Action ? rcc::riscv::legalizerInfo().lower(**BuiltAtomic, **Function, *Target, *Action)
             : rcc::Expected<void>(std::unexpected(Action.error()));
  Context.expect(Action && *Action == rcc::LegalizationAction::Custom && Lowered.has_value(),
                 "narrow RV64 compare-exchange must use the widening legalizer rule");
  if (!Lowered)
    return;

  const auto CompareInfo = (*Function)->registerInfo().virtualRegisterInfo(*Compare);
  const auto NewValueInfo = (*Function)->registerInfo().virtualRegisterInfo(*NewValue);
  const rcc::Register *AtomicResult = (*BuiltAtomic)->operandAt(0)->getIf<rcc::Register>();
  const auto AtomicResultInfo =
      AtomicResult == nullptr
          ? rcc::Expected<std::reference_wrapper<const rcc::VirtualRegisterInfo>>(std::unexpected(
                rcc::Error(rcc::ErrorCode::InvalidArgument, "missing compare-exchange result")))
          : (*Function)->registerInfo().virtualRegisterInfo(*AtomicResult);
  auto LegalAction = rcc::riscv::legalizerInfo().classify(**BuiltAtomic, **Function, *Target);
  Context.expect(CompareInfo && CompareInfo->get().Type == *I64 && NewValueInfo &&
                     NewValueInfo->get().Type == *I64 && AtomicResultInfo &&
                     AtomicResultInfo->get().Type == *I64 &&
                     (*BuiltExtension)->parent() == nullptr &&
                     Target->validateInstruction(**BuiltAtomic, (*Function)->registerInfo()) &&
                     LegalAction && *LegalAction == rcc::LegalizationAction::Legal,
                 "compare-exchange widening must preserve the narrow MMO in a legal i64 container");

  auto FallbackCompare = (*Function)->registerInfo().createVirtualRegister(*I8);
  auto FallbackNewValue = (*Function)->registerInfo().createVirtualRegister(*I8);
  auto FallbackResult = (*Function)->registerInfo().createVirtualRegister(*I8);
  if (!FallbackCompare || !FallbackNewValue || !FallbackResult || !Builder.setInsertPoint(**Block))
    return;
  for (rcc::Register Definition : {*FallbackCompare, *FallbackNewValue}) {
    auto Implicit = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_IMPLICIT_DEF));
    if (!Implicit || !Implicit->addDef(Definition) || !Implicit->build())
      return;
  }
  auto Fallback = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ATOMIC_CMPXCHG));
  if (!Fallback || !Fallback->addDef(*FallbackResult) || !Fallback->addUse(*Address) ||
      !Fallback->addUse(*FallbackCompare) || !Fallback->addUse(*FallbackNewValue) ||
      !Fallback->addMemOperand(**Memory))
    return;
  auto BuiltFallback = Fallback->build();
  if (!BuiltFallback)
    return;
  auto FallbackAction = rcc::riscv::legalizerInfo().classify(**BuiltFallback, **Function, *Target);
  auto FallbackLowered =
      FallbackAction
          ? rcc::riscv::legalizerInfo().lower(**BuiltFallback, **Function, *Target, *FallbackAction)
          : rcc::Expected<void>(std::unexpected(FallbackAction.error()));
  rcc::MachineInstr *Truncation = (*BuiltFallback)->next();
  const rcc::Register *WideFallbackResult = (*BuiltFallback)->operandAt(0)->getIf<rcc::Register>();
  const rcc::Register *TruncatedResult =
      Truncation == nullptr ? nullptr : Truncation->operandAt(0)->getIf<rcc::Register>();
  const rcc::Register *TruncationSource =
      Truncation == nullptr ? nullptr : Truncation->operandAt(1)->getIf<rcc::Register>();
  Context.expect(FallbackLowered && WideFallbackResult != nullptr &&
                     *WideFallbackResult != *FallbackResult && Truncation != nullptr &&
                     Truncation->opcode() == rcc::opcode(rcc::GenericOpcode::G_TRUNC) &&
                     TruncatedResult != nullptr && *TruncatedResult == *FallbackResult &&
                     TruncationSource != nullptr && *TruncationSource == *WideFallbackResult &&
                     Target->validateInstruction(*Truncation, (*Function)->registerInfo()),
                 "general compare-exchange widening must truncate from the new i64 result");

  auto OldValue = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto Success = (*Function)->registerInfo().createVirtualRegister(*I1);
  if (!OldValue || !Success || !Builder.setInsertPoint(**Block))
    return;
  auto WithSuccess =
      Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS));
  if (!WithSuccess || !WithSuccess->addDef(*OldValue) || !WithSuccess->addDef(*Success) ||
      !WithSuccess->addUse(*Address) || !WithSuccess->addUse(*Compare) ||
      !WithSuccess->addUse(*NewValue) || !WithSuccess->addMemOperand(**Memory))
    return;
  auto BuiltWithSuccess = WithSuccess->build();
  if (!BuiltWithSuccess)
    return;
  auto WithSuccessAction =
      rcc::riscv::legalizerInfo().classify(**BuiltWithSuccess, **Function, *Target);
  auto WithSuccessLowered = WithSuccessAction
                                ? rcc::riscv::legalizerInfo().lower(**BuiltWithSuccess, **Function,
                                                                    *Target, *WithSuccessAction)
                                : rcc::Expected<void>(std::unexpected(WithSuccessAction.error()));
  rcc::MachineInstr *SuccessCompare = (*BuiltWithSuccess)->previous();
  rcc::MachineInstr *LoweredAtomic =
      SuccessCompare == nullptr ? nullptr : SuccessCompare->previous();
  Context.expect(
      WithSuccessAction && *WithSuccessAction == rcc::LegalizationAction::Custom &&
          WithSuccessLowered && LoweredAtomic != nullptr && SuccessCompare != nullptr &&
          LoweredAtomic->opcode() == rcc::opcode(rcc::GenericOpcode::G_ATOMIC_CMPXCHG) &&
          SuccessCompare->opcode() == rcc::opcode(rcc::GenericOpcode::G_ICMP) &&
          (*BuiltWithSuccess)->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
          (*BuiltWithSuccess)->memOperands().empty() &&
          Target->validateInstruction(*LoweredAtomic, (*Function)->registerInfo()) &&
          Target->validateInstruction(*SuccessCompare, (*Function)->registerInfo()) &&
          Target->validateInstruction(**BuiltWithSuccess, (*Function)->registerInfo()),
      "compare-exchange-with-success must lower to atomic, equality test, and old-value copy");
}

void testConstantExtensionPreservesLiveSource(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Function = rcc::MachineFunction::create("riscv_live_constant_extension",
                                               rcc::MachineFunctionStableID::fromValue(1300));
  auto I32 = rcc::LLT::makeAnyScalar(32);
  auto I64 = rcc::LLT::makeAnyScalar(64);
  if (!Target || !Function || !I32 || !I64)
    return;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return;
  auto Source = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Extended = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto PreservedUse = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Immediate = rcc::IntegerImmediate::fromUnsigned(32, 7);
  if (!Source || !Extended || !PreservedUse || !Immediate)
    return;

  rcc::MachineIRBuilder Builder(**Function, *Target);
  if (!Builder.setInsertPoint(**Block))
    return;
  auto Constant = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_CONSTANT));
  if (!Constant || !Constant->addDef(*Source) ||
      !Constant->addIntegerImmediate(std::move(*Immediate)))
    return;
  auto BuiltConstant = Constant->build();
  auto Extension = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ANYEXT));
  if (!BuiltConstant || !Extension || !Extension->addDef(*Extended) || !Extension->addUse(*Source))
    return;
  auto BuiltExtension = Extension->build();
  auto Copy = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!BuiltExtension || !Copy || !Copy->addDef(*PreservedUse) || !Copy->addUse(*Source) ||
      !Copy->build())
    return;

  auto Action = rcc::riscv::legalizerInfo().classify(**BuiltExtension, **Function, *Target);
  auto Lowered =
      Action ? rcc::riscv::legalizerInfo().lower(**BuiltExtension, **Function, *Target, *Action)
             : rcc::Expected<void>(std::unexpected(Action.error()));
  Context.expect(Action && *Action == rcc::LegalizationAction::Custom && Lowered &&
                     (*BuiltExtension)->opcode() == rcc::opcode(rcc::GenericOpcode::G_CONSTANT) &&
                     (*BuiltConstant)->parent() != nullptr &&
                     (*Function)->registerInfo().getUniqueVRegDef(*Source) ==
                         (*BuiltConstant)->operandAt(0) &&
                     (*Function)->registerInfo().verifyDefUseChains(),
                 "constant-extension folding must preserve a source definition with live uses");
}

void testIntegerCompareReusesConstantExtension(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto I32 = rcc::LLT::makeAnyScalar(32);
  auto I64 = rcc::LLT::makeAnyScalar(64);
  if (!Target || !I32 || !I64)
    return;
  auto Function =
      createCompareFunction(Context, *Target, rcc::GenericOpcode::G_ICMP, *I32, false,
                            rcc::IntegerPredicate::Equal, rcc::FloatingPredicate::AlwaysFalse);
  if (!Function)
    return;
  rcc::MachineInstr *Compare = Function->firstBlock()->firstInstruction();
  const rcc::Register *Right = Compare->operandAt(3)->getIf<rcc::Register>();
  auto WideRight = Function->registerInfo().createVirtualRegister(*I64);
  auto Immediate = rcc::IntegerImmediate::fromUnsigned(32, 0);
  if (Right == nullptr || !WideRight || !Immediate)
    return;

  rcc::MachineIRBuilder Builder(*Function, *Target);
  if (!Builder.setInsertPointBefore(*Compare))
    return;
  auto Constant = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_CONSTANT));
  if (!Constant || !Constant->addDef(*Right) ||
      !Constant->addIntegerImmediate(std::move(*Immediate)))
    return;
  auto BuiltConstant = Constant->build();
  if (!BuiltConstant)
    return;
  auto Extension = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ANYEXT));
  if (!Extension || !Extension->addDef(*WideRight) || !Extension->addUse(*Right))
    return;
  auto BuiltExtension = Extension->build();
  if (!BuiltExtension)
    return;

  auto Action = rcc::riscv::legalizerInfo().classify(*Compare, *Function, *Target);
  auto Lowered = Action ? rcc::riscv::legalizerInfo().lower(*Compare, *Function, *Target, *Action)
                        : rcc::Expected<void>(std::unexpected(Action.error()));
  const rcc::Register *LegalRight = Compare->operandAt(3)->getIf<rcc::Register>();
  Context.expect(Action && *Action == rcc::LegalizationAction::Custom && Lowered,
                 "RV64 integer comparison extension-reuse fixture must legalize");
  Context.expect(LegalRight != nullptr && *LegalRight == *WideRight,
                 "RV64 integer comparison legalization must reuse an existing constant extension");
  Context.expect(Function->registerInfo().verifyDefUseChains().has_value(),
                 "integer comparison extension reuse must preserve exact def-use chains");

  auto ExtensionAction = rcc::riscv::legalizerInfo().classify(**BuiltExtension, *Function, *Target);
  auto Folded = ExtensionAction ? rcc::riscv::legalizerInfo().lower(**BuiltExtension, *Function,
                                                                    *Target, *ExtensionAction)
                                : rcc::Expected<void>(std::unexpected(ExtensionAction.error()));
  const rcc::Register *FoldedResult = (*BuiltConstant)->operandAt(0)->getIf<rcc::Register>();
  Context.expect(ExtensionAction && *ExtensionAction == rcc::LegalizationAction::Custom && Folded &&
                     (*BuiltExtension)->parent() == nullptr && FoldedResult != nullptr &&
                     *FoldedResult == *WideRight &&
                     Function->firstBlock()->firstInstruction() == *BuiltConstant &&
                     Function->registerInfo().verifyDefUseChains(),
                 "single-use constant extension folding must preserve the source layout position");
}

void testNarrowAnyExtBinaryArtifacts(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Function = rcc::MachineFunction::create("riscv_narrow_binary_artifacts",
                                               rcc::MachineFunctionStableID::fromValue(1301));
  auto I1 = rcc::LLT::makeAnyScalar(1);
  auto I32 = rcc::LLT::makeAnyScalar(32);
  auto I64 = rcc::LLT::makeAnyScalar(64);
  if (!Target || !Function || !I1 || !I32 || !I64)
    return;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return;
  auto WideLeft = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto WideRight = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto NarrowLeft = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto CopiedLeft = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto NarrowRight = (*Function)->registerInfo().createVirtualRegister(*I1);
  auto ExtendedRight = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Product = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Result = (*Function)->registerInfo().createVirtualRegister(*I64);
  if (!WideLeft || !WideRight || !NarrowLeft || !CopiedLeft || !NarrowRight || !ExtendedRight ||
      !Product || !Result)
    return;

  rcc::MachineIRBuilder Builder(**Function, *Target);
  if (!Builder.setInsertPoint(**Block))
    return;
  const auto BuildUnary = [&](rcc::GenericOpcode Generic, rcc::Register Destination,
                              rcc::Register Source) -> rcc::MachineInstr * {
    auto Pending = Builder.buildInstr(rcc::opcode(Generic));
    if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Source))
      return nullptr;
    auto Built = Pending->build();
    return Built ? *Built : nullptr;
  };
  rcc::MachineInstr *LeftTrunc = BuildUnary(rcc::GenericOpcode::G_TRUNC, *NarrowLeft, *WideLeft);
  rcc::MachineInstr *LeftCopy = BuildUnary(rcc::GenericOpcode::COPY, *CopiedLeft, *NarrowLeft);
  rcc::MachineInstr *RightTrunc = BuildUnary(rcc::GenericOpcode::G_TRUNC, *NarrowRight, *WideRight);
  rcc::MachineInstr *RightExtension =
      BuildUnary(rcc::GenericOpcode::G_ANYEXT, *ExtendedRight, *NarrowRight);
  auto Multiply = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_MUL));
  if (LeftTrunc == nullptr || LeftCopy == nullptr || RightTrunc == nullptr ||
      RightExtension == nullptr || !Multiply || !Multiply->addDef(*Product) ||
      !Multiply->addUse(*CopiedLeft) || !Multiply->addUse(*ExtendedRight))
    return;
  auto BuiltMultiply = Multiply->build();
  rcc::MachineInstr *OuterExtension = BuildUnary(rcc::GenericOpcode::G_ANYEXT, *Result, *Product);
  if (!BuiltMultiply || OuterExtension == nullptr)
    return;

  auto Action = rcc::riscv::legalizerInfo().classify(*OuterExtension, **Function, *Target);
  auto Lowered =
      Action ? rcc::riscv::legalizerInfo().lower(*OuterExtension, **Function, *Target, *Action)
             : rcc::Expected<void>(std::unexpected(Action.error()));
  const rcc::Register *LegalLeft = OuterExtension->operandAt(1)->getIf<rcc::Register>();
  const rcc::Register *LegalRight = OuterExtension->operandAt(2)->getIf<rcc::Register>();
  Context.expect(
      Action && *Action == rcc::LegalizationAction::Custom && Lowered &&
          OuterExtension->opcode() == rcc::opcode(rcc::GenericOpcode::G_MUL) &&
          LegalLeft != nullptr && *LegalLeft == *WideLeft && LegalRight != nullptr &&
          *LegalRight == *WideRight && LeftTrunc->parent() == nullptr &&
          LeftCopy->parent() == nullptr && RightTrunc->parent() == nullptr &&
          RightExtension->parent() == nullptr && (*BuiltMultiply)->parent() == nullptr &&
          (*Function)->registerInfo().verifyDefUseChains() &&
          Target->validateInstruction(*OuterExtension, (*Function)->registerInfo()),
      "narrow any-extension binary folding must remove copy, truncation, and extension artifacts");
}

void testMultiplyLibcallWithoutM(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  const std::vector<std::string> FeatureNames{"+a"};
  auto Features = rcc::riscv::RISCVSubtargetFeatures::fromFeatureList(FeatureNames);
  auto Legalizer = Features ? rcc::riscv::createLegalizerInfo(*Features) : nullptr;
  auto Multiply64 =
      Target ? createBinary(Context, *Target, rcc::GenericOpcode::G_MUL, 64) : InstructionFixture{};
  if (!Target || !Features || !Legalizer || Multiply64.Instruction == nullptr)
    return;

  auto Action = Legalizer->classify(*Multiply64.Instruction, *Multiply64.Function, *Target);
  auto Lowered =
      Action ? Legalizer->lower(*Multiply64.Instruction, *Multiply64.Function, *Target, *Action)
             : rcc::Expected<void>(std::unexpected(Action.error()));
  rcc::MachineInstr *Call = nullptr;
  for (rcc::MachineInstr *Instruction : Multiply64.Function->allInstructions()) {
    if (Instruction->parent() != nullptr &&
        Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                     rcc::riscv::InstructionOpcode::PseudoCALL))) {
      Call = Instruction;
      break;
    }
  }
  const rcc::MachineOperand *Callee = Call == nullptr ? nullptr : Call->operandAt(0);
  const rcc::MachineOperand *MaskOperand = Call == nullptr ? nullptr : Call->operandAt(1);
  const auto *External = Callee == nullptr ? nullptr : Callee->getIf<rcc::ExternalSymbolOperand>();
  const auto *CallMask =
      MaskOperand == nullptr ? nullptr : MaskOperand->getIf<rcc::RegisterMaskOperand>();
  const auto LP64Mask = rcc::riscv::createCallPreservedMask(rcc::riscv::CallingConvention::LP64);
  Context.expect(Action && *Action == rcc::LegalizationAction::Libcall && Lowered &&
                     Multiply64.Instruction->parent() == nullptr && External != nullptr &&
                     External->Name == "__muldi3" && CallMask != nullptr && CallMask->Mask &&
                     LP64Mask != nullptr && *CallMask->Mask == *LP64Mask &&
                     Multiply64.Function->frameInfo().state().HasCalls,
                 "s64 G_MUL without M must lower through the LP64 __muldi3 call path");

  auto Multiply32 = createBinary(Context, *Target, rcc::GenericOpcode::G_MUL, 32);
  if (Multiply32.Instruction == nullptr)
    return;
  auto NarrowAction = Legalizer->classify(*Multiply32.Instruction, *Multiply32.Function, *Target);
  auto NarrowLowered = NarrowAction ? Legalizer->lower(*Multiply32.Instruction,
                                                       *Multiply32.Function, *Target, *NarrowAction)
                                    : rcc::Expected<void>(std::unexpected(NarrowAction.error()));
  rcc::MachineInstr *WideMultiply = nullptr;
  for (rcc::MachineInstr *Instruction : Multiply32.Function->allInstructions()) {
    if (Instruction->parent() != nullptr &&
        Instruction->opcode() == rcc::opcode(rcc::GenericOpcode::G_MUL)) {
      WideMultiply = Instruction;
      break;
    }
  }
  auto WideAction = WideMultiply == nullptr
                        ? rcc::Expected<rcc::LegalizationAction>(std::unexpected(
                              rcc::Error(rcc::ErrorCode::InternalError, "missing wide multiply")))
                        : Legalizer->classify(*WideMultiply, *Multiply32.Function, *Target);
  Context.expect(NarrowAction && *NarrowAction == rcc::LegalizationAction::Custom &&
                     NarrowLowered &&
                     Multiply32.Instruction->opcode() == rcc::opcode(rcc::GenericOpcode::G_TRUNC) &&
                     WideAction && *WideAction == rcc::LegalizationAction::Libcall,
                 "narrow G_MUL without M must widen to the single s64 multiply libcall type");
}

void testAtomicSelection(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  auto Pointer = rcc::LLT::makePointer(0, 64);
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  if (!I64 || !Pointer || !Size || !Align)
    return;

  const auto MakeRMW = [&](rcc::GenericOpcode Generic, std::uint64_t ID) -> InstructionFixture {
    auto Function =
        rcc::MachineFunction::create("riscv_atomic", rcc::MachineFunctionStableID::fromValue(ID));
    if (!Function)
      return {};
    auto Block = (*Function)->createBasicBlock();
    if (!Block || !(*Function)->appendBlock(**Block))
      return {};
    auto Result = (*Function)->registerInfo().createVirtualRegister(*I64);
    auto Address = (*Function)->registerInfo().createVirtualRegister(*Pointer);
    auto Value = (*Function)->registerInfo().createVirtualRegister(*I64);
    if (!Result || !Address || !Value)
      return {};
    rcc::MachineMemOperandDesc Description;
    Description.Accesses = {rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store};
    Description.AtomicKind = rcc::AtomicOperation::ReadModifyWrite;
    Description.SuccessOrdering = rcc::AtomicOrdering::SequentiallyConsistent;
    Description.Size = *Size;
    Description.Align = *Align;
    Description.Base = rcc::MemoryBase::stack();
    Description.Offset = 32;
    auto Memory = (*Function)->createMemOperand(std::move(Description));
    if (!Memory)
      return {};
    rcc::MachineIRBuilder Builder(**Function, *Target);
    if (!Builder.setInsertPoint(**Block))
      return {};
    auto Pending = Builder.buildInstr(rcc::opcode(Generic));
    if (!Pending || !Pending->addDef(*Result) || !Pending->addUse(*Address) ||
        !Pending->addUse(*Value) || !Pending->addMemOperand(**Memory)) {
      return {};
    }
    auto Built = Pending->build();
    if (!Built || !constrainVirtualRegisters(**Function))
      return {};
    return {std::move(*Function), *Built, *Result};
  };

  auto Add = MakeRMW(rcc::GenericOpcode::G_ATOMICRMW_ADD, 1300);
  auto Sub = MakeRMW(rcc::GenericOpcode::G_ATOMICRMW_SUB, 1301);
  auto Nand = MakeRMW(rcc::GenericOpcode::G_ATOMICRMW_NAND, 1302);
  Context.expect(Add.Instruction != nullptr && Sub.Instruction != nullptr &&
                     Nand.Instruction != nullptr,
                 "atomic RMW fixtures must be complete");
  if (Add.Instruction != nullptr) {
    auto Selection =
        rcc::riscv::instructionSelector().select(*Add.Instruction, *Add.Function, *Target);
    Context.expect(Selection.has_value() && Add.Instruction->opcode() ==
                                                rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                                    rcc::riscv::InstructionOpcode::AMOADD_D)),
                   "G_ATOMICRMW_ADD must select AMOADD.D");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Add.Function),
                   "selected AMOADD.D must satisfy its descriptor and MMO contract");
  }
  if (Sub.Instruction != nullptr) {
    auto Selection =
        rcc::riscv::instructionSelector().select(*Sub.Instruction, *Sub.Function, *Target);
    Context.expect(Selection.has_value() && Sub.Function->allInstructions().size() == 2,
                   "G_ATOMICRMW_SUB must negate its operand before AMOADD.D");
    Context.expect(Selection && validatesSelectedFunction(*Target, *Sub.Function),
                   "selected atomic subtraction must satisfy target descriptors");
  }
  if (Nand.Instruction != nullptr) {
    auto Classification =
        rcc::riscv::legalizerInfo().classify(*Nand.Instruction, *Nand.Function, *Target);
    Context.expect(
        Classification && *Classification == rcc::LegalizationAction::Unsupported,
        "an atomic operation without a single-instruction RV64GC mapping must be explicit");
    auto Match =
        rcc::riscv::instructionSelector().canSelect(*Nand.Instruction, *Nand.Function, *Target);
    Context.expect(!Match && Match.error().code() == rcc::ErrorCode::Unsupported,
                   "unsupported atomic RMW must fail before mutating MachineIR");
    Context.expect(!Match && Match.error().message().find("G_ATOMICRMW_NAND") != std::string::npos,
                   "unsupported atomic diagnostics must name the rejected opcode");
  }

  const auto MakeAtomicAccess = [&](bool Load, std::uint64_t ID) -> InstructionFixture {
    auto Function = rcc::MachineFunction::create(Load ? "riscv_atomic_load" : "riscv_atomic_store",
                                                 rcc::MachineFunctionStableID::fromValue(ID));
    if (!Function)
      return {};
    auto Block = (*Function)->createBasicBlock();
    if (!Block || !(*Function)->appendBlock(**Block))
      return {};
    auto Value = (*Function)->registerInfo().createVirtualRegister(*I64);
    auto Address = (*Function)->registerInfo().createVirtualRegister(*Pointer);
    if (!Value || !Address)
      return {};
    rcc::MachineMemOperandDesc Description;
    Description.Accesses = {Load ? rcc::MemoryAccessFlag::Load : rcc::MemoryAccessFlag::Store};
    Description.AtomicKind = Load ? rcc::AtomicOperation::Load : rcc::AtomicOperation::Store;
    Description.SuccessOrdering =
        Load ? rcc::AtomicOrdering::Acquire : rcc::AtomicOrdering::Release;
    Description.Size = *Size;
    Description.Align = *Align;
    Description.Base = rcc::MemoryBase::stack();
    auto Memory = (*Function)->createMemOperand(std::move(Description));
    if (!Memory)
      return {};
    rcc::MachineIRBuilder Builder(**Function, *Target);
    if (!Builder.setInsertPoint(**Block))
      return {};
    auto Pending = Builder.buildInstr(
        rcc::opcode(Load ? rcc::GenericOpcode::G_LOAD : rcc::GenericOpcode::G_STORE));
    if (!Pending)
      return {};
    if (Load) {
      if (!Pending->addDef(*Value) || !Pending->addUse(*Address))
        return {};
    } else if (!Pending->addUse(*Value) || !Pending->addUse(*Address)) {
      return {};
    }
    if (!Pending->addMemOperand(**Memory))
      return {};
    auto Built = Pending->build();
    if (!Built || !constrainVirtualRegisters(**Function))
      return {};
    return {std::move(*Function), *Built, *Value};
  };
  auto AtomicLoad = MakeAtomicAccess(true, 1303);
  auto AtomicStore = MakeAtomicAccess(false, 1304);
  if (AtomicLoad.Instruction != nullptr) {
    auto Selection = rcc::riscv::instructionSelector().select(*AtomicLoad.Instruction,
                                                              *AtomicLoad.Function, *Target);
    Context.expect(Selection && AtomicLoad.Instruction->opcode() ==
                                    rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                        rcc::riscv::InstructionOpcode::LR_D)),
                   "atomic G_LOAD must select LR.D");
    Context.expect(Selection && validatesSelectedFunction(*Target, *AtomicLoad.Function),
                   "selected atomic load must satisfy target descriptors");
  }
  if (AtomicStore.Instruction != nullptr) {
    auto Selection = rcc::riscv::instructionSelector().select(*AtomicStore.Instruction,
                                                              *AtomicStore.Function, *Target);
    Context.expect(Selection && AtomicStore.Instruction->opcode() ==
                                    rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                        rcc::riscv::InstructionOpcode::AMOSWAP_D)),
                   "atomic G_STORE must select AMOSWAP.D with a discarded old value");
    Context.expect(Selection && validatesSelectedFunction(*Target, *AtomicStore.Function),
                   "selected atomic store must satisfy target descriptors");
  }
}

void testUnsupportedTypes(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be available for type diagnostics");
  if (!Target)
    return;
  auto WideInteger = createBinary(Context, *Target, rcc::GenericOpcode::G_ADD, 128);
  if (WideInteger.Function && WideInteger.Instruction != nullptr) {
    auto Selection = rcc::riscv::instructionSelector().canSelect(*WideInteger.Instruction,
                                                                 *WideInteger.Function, *Target);
    Context.expect(!Selection && Selection.error().code() == rcc::ErrorCode::Unsupported,
                   "i128 arithmetic must produce an Unsupported diagnostic");
    Context.expect(!Selection && Selection.error().message().find("i128") != std::string::npos,
                   "unsupported type diagnostics must name the rejected LLT");
  }
  auto Function = rcc::MachineFunction::create("unsupported_type",
                                               rcc::MachineFunctionStableID::fromValue(100));
  if (!Function)
    return;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return;
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  if (!F64)
    return;
  auto Result = (*Function)->registerInfo().createVirtualRegister(*F64);
  auto Source = (*Function)->registerInfo().createVirtualRegister(*F64);
  if (!Result || !Source)
    return;
  rcc::MachineIRBuilder Builder(**Function, *Target);
  if (!Builder.setInsertPoint(**Block))
    return;
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_FEXP));
  if (!Pending || !Pending->addDef(*Result) || !Pending->addUse(*Source))
    return;
  if (!Pending->build())
    return;
  const rcc::MachineInstr &Instruction = **Function->get()->allInstructions().begin();
  auto Action = rcc::riscv::legalizerInfo().classify(Instruction, **Function, *Target);
  Context.expect(Action && *Action == rcc::LegalizationAction::Unsupported,
                 "unsupported floating operations must remain explicitly diagnosed");
  auto Selection = rcc::riscv::instructionSelector().canSelect(Instruction, **Function, *Target);
  Context.expect(!Selection && Selection.error().code() == rcc::ErrorCode::Unsupported,
                 "selector pre-matching must reject unsupported floating operations");
  Context.expect(!Selection && Selection.error().message().find("G_FEXP") != std::string::npos,
                 "unsupported selection diagnostics must name the rejected opcode");
}

void testMemoryOffsetSelection(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be available for memory selection");
  if (!Target)
    return;
  auto Function = rcc::MachineFunction::create("riscv_memory_offset",
                                               rcc::MachineFunctionStableID::fromValue(101));
  if (!Function)
    return;
  auto Block = (*Function)->createBasicBlock();
  if (!Block || !(*Function)->appendBlock(**Block))
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  auto Pointer = rcc::LLT::makePointer(0, 64);
  if (!I64 || !Pointer)
    return;
  auto Result = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto Address = (*Function)->registerInfo().createVirtualRegister(*Pointer);
  if (!Result || !Address)
    return;
  rcc::MachineIRBuilder Builder(**Function, *Target);
  if (!Builder.setInsertPoint(**Block))
    return;
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_LOAD));
  if (!Pending || !Pending->addDef(*Result) || !Pending->addUse(*Address))
    return;
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  if (!Size || !Align)
    return;
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Volatile};
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = rcc::MemoryBase::stack();
  Description.Offset = 24;
  auto Memory = (*Function)->createMemOperand(std::move(Description));
  if (!Memory || !Pending->addMemOperand(**Memory) || !Pending->build() ||
      !constrainVirtualRegisters(**Function))
    return;
  rcc::MachineInstr *Instruction = (*Function)->firstBlock()->firstInstruction();
  Context.expect(Instruction != nullptr, "generic load must remain attached to its block");
  if (Instruction == nullptr)
    return;
  auto Action = rcc::riscv::legalizerInfo().classify(*Instruction, **Function, *Target);
  Context.expect(Action && *Action == rcc::LegalizationAction::Legal,
                 "RV64GC legalizer must accept volatile scalar loads");
  auto Selection = rcc::riscv::instructionSelector().select(*Instruction, **Function, *Target);
  Context.expect(Selection.has_value(), "RV64GC selector must select a volatile scalar load");
  if (!Selection)
    return;
  Context.expect(Instruction->opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                              rcc::riscv::InstructionOpcode::LD)),
                 "i64 load with an offset must select LD");
  const rcc::MachineOperand *OffsetOperand = Instruction->operandAt(2);
  const std::int64_t *Offset =
      OffsetOperand == nullptr ? nullptr : OffsetOperand->getIf<std::int64_t>();
  Context.expect(Offset != nullptr && *Offset == 0,
                 "selected load must not reinterpret MMO provenance as an address immediate");
  Context.expect(Instruction->memOperands().size() == 1 &&
                     Instruction->memOperands().front()->description().Offset == 24,
                 "selected load must preserve its MMO provenance offset");
  Context.expect(validatesSelectedFunction(*Target, **Function),
                 "selected volatile load must satisfy the target descriptor");
}

} // namespace

int main() {
  TestContext Context;
  testClassification(Context);
  testNarrowIntegerSelection(Context);
  testScalarSelection(Context);
  testComparisonsAndControl(Context);
  testCompositeScalarSelection(Context);
  testOverflowSelection(Context);
  testAtomicCompareExchangeLegalization(Context);
  testConstantExtensionPreservesLiveSource(Context);
  testIntegerCompareReusesConstantExtension(Context);
  testNarrowAnyExtBinaryArtifacts(Context);
  testMultiplyLibcallWithoutM(Context);
  testAtomicSelection(Context);
  testUnsupportedTypes(Context);
  testMemoryOffsetSelection(Context);
  return Context.result();
}
