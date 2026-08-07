// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachinePipelinePasses.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <ranges>
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

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Entry = nullptr;
};

class TogglingLegalizer final : public rcc::TargetLegalizerInfo {
public:
  [[nodiscard]] rcc::Expected<rcc::LegalizationAction>
  classify(const rcc::MachineInstr &, const rcc::MachineFunction &,
           const rcc::TargetDescription &) const override {
    return rcc::LegalizationAction::Lower;
  }

  [[nodiscard]] rcc::Expected<void> lower(rcc::MachineInstr &Instruction, rcc::MachineFunction &,
                                          const rcc::TargetDescription &,
                                          rcc::LegalizationAction) const override {
    const auto Generic = static_cast<rcc::GenericOpcode>(Instruction.opcode().value());
    return Instruction.setOpcode(rcc::opcode(Generic == rcc::GenericOpcode::G_ADD
                                                 ? rcc::GenericOpcode::G_SUB
                                                 : rcc::GenericOpcode::G_ADD));
  }
};

class MutatingFailingSelector final : public rcc::TargetInstructionSelector {
public:
  [[nodiscard]] rcc::Expected<void> canSelect(const rcc::MachineInstr &,
                                              const rcc::MachineFunction &,
                                              const rcc::TargetDescription &) const override {
    return {};
  }

  [[nodiscard]] rcc::Expected<void> select(rcc::MachineInstr &Instruction, rcc::MachineFunction &,
                                           const rcc::TargetDescription &) const override {
    auto Mutation = Instruction.setOpcode(rcc::opcode(rcc::GenericOpcode::G_SUB));
    if (!Mutation)
      return std::unexpected(Mutation.error());
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported, "test selector rejected its renderer");
  }
};

class RecordingSelector final : public rcc::TargetInstructionSelector {
public:
  explicit RecordingSelector(const rcc::TargetInstructionSelector &Delegate)
      : Delegate(&Delegate) {}

  [[nodiscard]] rcc::Expected<void> canSelect(const rcc::MachineInstr &Instruction,
                                              const rcc::MachineFunction &Function,
                                              const rcc::TargetDescription &Target) const override {
    return Delegate->canSelect(Instruction, Function, Target);
  }

  [[nodiscard]] rcc::Expected<void> select(rcc::MachineInstr &Instruction,
                                           rcc::MachineFunction &Function,
                                           const rcc::TargetDescription &Target) const override {
    SelectedIDs.push_back(Instruction.stableID().value());
    return Delegate->select(Instruction, Function, Target);
  }

  [[nodiscard]] const std::vector<std::uint64_t> &selectedIDs() const noexcept {
    return SelectedIDs;
  }

private:
  const rcc::TargetInstructionSelector *Delegate;
  mutable std::vector<std::uint64_t> SelectedIDs;
};

class TwoStepSelector final : public rcc::TargetInstructionSelector {
public:
  [[nodiscard]] rcc::Expected<void> canSelect(const rcc::MachineInstr &,
                                              const rcc::MachineFunction &,
                                              const rcc::TargetDescription &) const override {
    return {};
  }

  [[nodiscard]] rcc::Expected<void> select(rcc::MachineInstr &Instruction, rcc::MachineFunction &,
                                           const rcc::TargetDescription &) const override {
    ++Selections;
    if (Instruction.opcode() == rcc::opcode(rcc::GenericOpcode::G_ADD))
      return Instruction.setOpcode(rcc::opcode(rcc::GenericOpcode::G_SUB));
    if (Instruction.opcode() == rcc::opcode(rcc::GenericOpcode::G_SUB)) {
      return Instruction.setOpcode(
          rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADD)));
    }
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                               "two-step selector received an unexpected opcode");
  }

  [[nodiscard]] std::size_t selections() const noexcept {
    return Selections;
  }

private:
  mutable std::size_t Selections = 0;
};

[[nodiscard]] FunctionFixture createFixture(TestContext &Context, std::string_view Name,
                                            std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::string(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine pipeline function creation must succeed");
  if (!Function)
    return {};
  auto Entry = (*Function)->createBasicBlock();
  Context.expect(Entry.has_value(), "machine pipeline block creation must succeed");
  if (!Entry || !(*Function)->appendBlock(**Entry))
    return {};
  return {std::move(*Function), *Entry};
}

[[nodiscard]] rcc::Register createRegister(TestContext &Context, rcc::MachineFunction &Function,
                                           std::uint32_t Bits) {
  auto Type = rcc::LLT::makeInteger(Bits);
  Context.expect(Type.has_value(), "integer LLT construction must succeed");
  if (!Type)
    return {};
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Reg.has_value(), "virtual register construction must succeed");
  return Reg ? *Reg : rcc::Register{};
}

[[nodiscard]] rcc::Register createFloatRegister(TestContext &Context,
                                                rcc::MachineFunction &Function) {
  auto Type = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  Context.expect(Type.has_value(), "floating LLT construction must succeed");
  if (!Type)
    return {};
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Reg.has_value(), "floating virtual register construction must succeed");
  return Reg ? *Reg : rcc::Register{};
}

[[nodiscard]] bool assignGPR(rcc::MachineFunction &Function, rcc::Register Reg) {
  const auto Bank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto RegClass =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  return Function.registerInfo().setRegisterBank(Reg, Bank).has_value() &&
         Function.registerInfo().setRegisterClass(Reg, RegClass).has_value();
}

[[nodiscard]] bool buildConstant(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                 rcc::Register Destination, std::int64_t Value) {
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_CONSTANT));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addImmediate(Value) ||
      !Pending->build()) {
    Context.expect(false, "generic constant construction must succeed");
    return false;
  }
  return true;
}

[[nodiscard]] bool buildAdd(TestContext &Context, rcc::MachineIRBuilder &Builder,
                            rcc::Register Destination, rcc::Register Left, rcc::Register Right) {
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ADD));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Left) ||
      !Pending->addUse(Right) || !Pending->build()) {
    Context.expect(false, "generic add construction must succeed");
    return false;
  }
  return true;
}

void testNullAndMissingContracts(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_contracts", 1);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "pipeline contract fixture must be complete");
  if (!Target || !Fixture.Function)
    return;

  rcc::LegalizerPass NullPass(nullptr);
  rcc::MachineAnalysisManager Analyses;
  auto NullResult = NullPass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!NullResult && NullResult.error().code() == rcc::ErrorCode::InvalidArgument,
                 "a null legalizer interface must be rejected");
  Context.expect(Fixture.Function->properties().empty(),
                 "a rejected pass must not publish function properties");

  rcc::LegalizerPass ValidPass(rcc::riscv::legalizerInfo());
  auto MissingResult = ValidPass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!MissingResult &&
                     MissingResult.error().code() == rcc::ErrorCode::VerificationError,
                 "a pass with missing prerequisites must be rejected");
}

void testPipelineAndUnsupportedRollback(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_success", 2);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "pipeline success fixture must be complete");
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;

  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Result = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Entry).has_value(),
                 "pipeline insertion point must be valid");
  if (Left.isNoRegister() || Right.isNoRegister() || Result.isNoRegister() ||
      !buildConstant(Context, Builder, Left, 42) || !buildConstant(Context, Builder, Right, 1) ||
      !buildAdd(Context, Builder, Result, Left, Right))
    return;

  Fixture.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                    rcc::MachineFunctionProperty::TracksLiveness};
  rcc::MachinePassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<rcc::LegalizerPass>(rcc::riscv::legalizerInfo()))
                     .has_value(),
                 "legalizer must be accepted by the pass manager");
  Context.expect(
      Pipeline
          .addPass(std::make_unique<rcc::RegisterBankSelectPass>(rcc::riscv::registerBankInfo()))
          .has_value(),
      "register-bank selector must be accepted by the pass manager");
  Context.expect(
      Pipeline
          .addPass(std::make_unique<rcc::InstructionSelectPass>(rcc::riscv::instructionSelector()))
          .has_value(),
      "instruction selector must be accepted by the pass manager");
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pipeline.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Run.has_value(), "RV64GC generic pipeline must complete");
  Context.expect(Fixture.Function->properties().has(rcc::MachineFunctionProperty::Selected),
                 "successful instruction selection must publish Selected");
  Context.expect(Fixture.Function->firstBlock()->firstInstruction() != nullptr,
                 "selected instructions must remain attached to their block");
  for (const rcc::MachineInstr *Instruction : Fixture.Function->allInstructions())
    Context.expect(!rcc::isGenericOpcode(Instruction->opcode()),
                   "successful instruction selection must remove generic opcodes");

  auto Unsupported = createFixture(Context, "pipeline_unsupported", 3);
  Context.expect(Unsupported.Function != nullptr && Unsupported.Entry != nullptr,
                 "unsupported pipeline fixture must be complete");
  if (!Unsupported.Function || Unsupported.Entry == nullptr)
    return;
  const rcc::Register UnsupportedReg = createFloatRegister(Context, *Unsupported.Function);
  const rcc::Register UnsupportedSource = createFloatRegister(Context, *Unsupported.Function);
  rcc::MachineIRBuilder UnsupportedBuilder(*Unsupported.Function, *Target);
  Context.expect(UnsupportedBuilder.setInsertPoint(*Unsupported.Entry).has_value(),
                 "unsupported pipeline insertion point must be valid");
  if (UnsupportedReg.isNoRegister() || UnsupportedSource.isNoRegister())
    return;
  auto Pending = UnsupportedBuilder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_FEXP));
  Context.expect(Pending.has_value(), "unsupported generic instruction must be constructible");
  if (!Pending)
    return;
  auto Built = Pending->addDef(UnsupportedReg);
  Context.expect(Built.has_value(), "unsupported generic definition must be accepted");
  if (!Built)
    return;
  Context.expect(Pending->addUse(UnsupportedSource).has_value() && Pending->build().has_value(),
                 "unsupported floating operation must be constructible");
  Unsupported.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                        rcc::MachineFunctionProperty::TracksLiveness};
  rcc::MachinePassManager OnlyLegalizer;
  Context.expect(
      OnlyLegalizer.addPass(std::make_unique<rcc::LegalizerPass>(rcc::riscv::legalizerInfo()))
          .has_value(),
      "standalone legalizer must be accepted");
  rcc::MachineAnalysisManager UnsupportedAnalyses;
  auto Failure = OnlyLegalizer.run(*Unsupported.Function, *Target, UnsupportedAnalyses);
  Context.expect(!Failure && Failure.error().code() == rcc::ErrorCode::Unsupported,
                 "unsupported generic instructions must produce a target diagnostic");
  Context.expect(!Failure && Failure.error().message().find("G_FEXP") != std::string::npos,
                 "unsupported legalization diagnostics must name the rejected opcode");
  Context.expect(
      Unsupported.Function->properties().has(rcc::MachineFunctionProperty::IsSSA) &&
          !Unsupported.Function->properties().has(rcc::MachineFunctionProperty::Legalized),
      "failed legalizer execution must not publish Legalized");
}

void testBankConflict(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_bank_conflict", 4);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "bank conflict fixture must be complete");
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Value = createRegister(Context, *Fixture.Function, 64);
  Context.expect(
      Fixture.Function->registerInfo()
          .setRegisterBank(Value, rcc::RegisterBankID::fromValue(
                                      static_cast<std::uint32_t>(rcc::riscv::RegisterBank::FPR)))
          .has_value(),
      "conflicting bank assignment must be constructible");
  Fixture.Function->properties() = {rcc::MachineFunctionProperty::Legalized};
  rcc::RegisterBankSelectPass Pass(rcc::riscv::registerBankInfo());
  rcc::MachineAnalysisManager Analyses;
  auto Result = Pass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!Result && Result.error().code() == rcc::ErrorCode::VerificationError,
                 "bank selection must reject a conflicting pre-existing bank");
  auto Info = Fixture.Function->registerInfo().virtualRegisterInfo(Value);
  Context.expect(Info && Info->get().Bank, "failed bank selection must preserve a bank assignment");
  if (!Info || !Info->get().Bank)
    return;
  const rcc::RegisterBankID Bank = Info->get().Bank.value_or(rcc::RegisterBankID{});
  Context.expect(Bank == rcc::RegisterBankID::fromValue(
                             static_cast<std::uint32_t>(rcc::riscv::RegisterBank::FPR)),
                 "failed bank selection must preserve the original bank assignment");
}

void testBankClassOnlyChange(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_bank_class", 5);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "bank class fixture must be complete");
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Value = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (Value.isNoRegister() || !Builder.setInsertPoint(*Fixture.Entry) ||
      !buildConstant(Context, Builder, Value, 7))
    return;
  const auto GPR =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  Context.expect(Fixture.Function->registerInfo().setRegisterBank(Value, GPR).has_value(),
                 "pre-existing GPR bank must be assignable");
  Fixture.Function->properties() = {rcc::MachineFunctionProperty::Legalized};
  rcc::RegisterBankSelectPass Pass(rcc::riscv::registerBankInfo());
  rcc::MachineAnalysisManager Analyses;
  auto Result = Pass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Result.has_value() && Result->Changed &&
                     Result->Changes.has(rcc::MachinePassChange::Registers),
                 "bank selection must report a class-only register change");
  auto Info = Fixture.Function->registerInfo().virtualRegisterInfo(Value);
  Context.expect(Info && Info->get().Bank && Info->get().RegClass,
                 "bank selection must publish both bank and class");
  if (!Info || !Info->get().RegClass)
    return;
  Context.expect(Info->get().RegClass.value_or(rcc::RegisterClassID{}) ==
                     rcc::RegisterClassID::fromValue(
                         static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR)),
                 "i64 GPR bank must select the GPR class");
}

void testLegalizerCycleDiagnostic(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_legalizer_cycle", 6);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "legalizer cycle fixture must be complete");
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Result = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (Left.isNoRegister() || Right.isNoRegister() || Result.isNoRegister() ||
      !Builder.setInsertPoint(*Fixture.Entry) || !buildAdd(Context, Builder, Result, Left, Right))
    return;
  Fixture.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                    rcc::MachineFunctionProperty::TracksLiveness};
  TogglingLegalizer Legalizer;
  rcc::LegalizerPass StablePass(Legalizer);
  rcc::MachineAnalysisManager Analyses;
  auto ResultRun = StablePass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!ResultRun && ResultRun.error().code() == rcc::ErrorCode::InternalError,
                 "repeated legalization states must report InternalError");
  Context.expect(Fixture.Entry->firstInstruction() != nullptr &&
                     Fixture.Entry->firstInstruction()->opcode() ==
                         rcc::opcode(rcc::GenericOpcode::G_ADD),
                 "failed legalization preflight must preserve the original instruction");
  Context.expect(!Fixture.Function->properties().has(rcc::MachineFunctionProperty::Legalized),
                 "a failed legalization cycle must not publish Legalized");
}

void testInstructionSelectionTransaction(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_selection_transaction", 8);
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Result = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (Left.isNoRegister() || Right.isNoRegister() || Result.isNoRegister() ||
      !Builder.setInsertPoint(*Fixture.Entry) || !buildAdd(Context, Builder, Result, Left, Right) ||
      !assignGPR(*Fixture.Function, Left) || !assignGPR(*Fixture.Function, Right) ||
      !assignGPR(*Fixture.Function, Result)) {
    return;
  }
  rcc::MachineInstr *Original = Fixture.Entry->firstInstruction();
  if (Original == nullptr)
    return;
  const rcc::Opcode OriginalOpcode = Original->opcode();
  const std::size_t OriginalOperandCount = Original->operandCount();
  Fixture.Function->properties() = {rcc::MachineFunctionProperty::Legalized,
                                    rcc::MachineFunctionProperty::RegBankSelected};
  MutatingFailingSelector Selector;
  rcc::InstructionSelectPass Pass(Selector);
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!Run && Run.error().code() == rcc::ErrorCode::Unsupported,
                 "a renderer failure must be reported before publication");
  Context.expect(Original->opcode() == OriginalOpcode &&
                     Original->operandCount() == OriginalOperandCount &&
                     Fixture.Entry->instructionCount() == 1,
                 "a renderer failure must leave the original MachineIR unchanged");
}

void testInstructionSelectionOrder(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_selection_order", 9);
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Result = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (Left.isNoRegister() || Right.isNoRegister() || Result.isNoRegister() ||
      !Builder.setInsertPoint(*Fixture.Entry) || !buildConstant(Context, Builder, Left, 6) ||
      !buildConstant(Context, Builder, Right, 7) ||
      !buildAdd(Context, Builder, Result, Left, Right) || !assignGPR(*Fixture.Function, Left) ||
      !assignGPR(*Fixture.Function, Right) || !assignGPR(*Fixture.Function, Result)) {
    return;
  }
  std::vector<std::uint64_t> LayoutIDs;
  for (const rcc::MachineInstr *Instruction = Fixture.Entry->firstInstruction();
       Instruction != nullptr; Instruction = Instruction->next()) {
    LayoutIDs.push_back(Instruction->stableID().value());
  }
  Fixture.Function->properties() = {rcc::MachineFunctionProperty::Legalized,
                                    rcc::MachineFunctionProperty::RegBankSelected};
  RecordingSelector Selector(rcc::riscv::instructionSelector());
  rcc::InstructionSelectPass Pass(Selector);
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Run.has_value(), "recorded instruction selection must complete");
  std::ranges::reverse(LayoutIDs);
  std::vector<std::uint64_t> Expected = LayoutIDs;
  Expected.insert(Expected.end(), LayoutIDs.begin(), LayoutIDs.end());
  Context.expect(
      Selector.selectedIDs() == Expected,
      "instruction selection must use stable block-postorder and reverse instruction order");
}

void testInstructionSelectionWorklist(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_selection_worklist", 10);
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Result = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (Left.isNoRegister() || Right.isNoRegister() || Result.isNoRegister() ||
      !Builder.setInsertPoint(*Fixture.Entry) || !buildAdd(Context, Builder, Result, Left, Right) ||
      !assignGPR(*Fixture.Function, Left) || !assignGPR(*Fixture.Function, Right) ||
      !assignGPR(*Fixture.Function, Result)) {
    return;
  }
  Fixture.Function->properties() = {rcc::MachineFunctionProperty::Legalized,
                                    rcc::MachineFunctionProperty::RegBankSelected};
  TwoStepSelector Selector;
  rcc::InstructionSelectPass Pass(Selector);
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Run.has_value(), "a progressing multi-step selection must complete");
  Context.expect(Selector.selections() == 4,
                 "transaction preflight and publication must each select both generic states");
  Context.expect(Fixture.Entry->firstInstruction() != nullptr &&
                     Fixture.Entry->firstInstruction()->opcode() ==
                         rcc::Opcode::fromValue(
                             static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADD)),
                 "the selection worklist must revisit a progressing generic root");
}

void testExtendedRV64GCPipeline(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_extended_rv64gc", 7);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "extended pipeline fixture must be complete");
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  auto I1 = rcc::LLT::makeInteger(1);
  auto I64 = rcc::LLT::makeInteger(64);
  auto Pointer = rcc::LLT::makePointer(0, 64);
  if (!I1 || !I64 || !Pointer)
    return;
  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Condition =
      Fixture.Function->registerInfo().createVirtualRegister(*I1).value_or(rcc::Register{});
  const rcc::Register Selected = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Sum = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Overflow =
      Fixture.Function->registerInfo().createVirtualRegister(*I1).value_or(rcc::Register{});
  const rcc::Register Quotient = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Remainder = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Address =
      Fixture.Function->registerInfo().createVirtualRegister(*Pointer).value_or(rcc::Register{});
  const rcc::Register AtomicResult = createRegister(Context, *Fixture.Function, 64);
  const std::array Registers{Left,     Right,    Condition, Selected, Sum,
                             Overflow, Quotient, Remainder, Address,  AtomicResult};
  if (std::ranges::any_of(Registers, [](rcc::Register Reg) { return Reg.isNoRegister(); }))
    return;
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (!Builder.setInsertPoint(*Fixture.Entry) || !buildConstant(Context, Builder, Left, 41) ||
      !buildConstant(Context, Builder, Right, 7)) {
    return;
  }
  auto Compare = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ICMP));
  if (!Compare || !Compare->addDef(Condition) ||
      !Compare->addIntegerPredicate(rcc::IntegerPredicate::UnsignedGreaterThan) ||
      !Compare->addUse(Left) || !Compare->addUse(Right) || !Compare->build()) {
    return;
  }
  auto Select = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_SELECT));
  if (!Select || !Select->addDef(Selected) || !Select->addUse(Condition) || !Select->addUse(Left) ||
      !Select->addUse(Right) || !Select->build()) {
    return;
  }
  auto AddOverflow = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_UADDO));
  if (!AddOverflow || !AddOverflow->addDef(Sum) || !AddOverflow->addDef(Overflow) ||
      !AddOverflow->addUse(Left) || !AddOverflow->addUse(Right) || !AddOverflow->build()) {
    return;
  }
  auto DivRem = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_SDIVREM));
  if (!DivRem || !DivRem->addDef(Quotient) || !DivRem->addDef(Remainder) || !DivRem->addUse(Left) ||
      !DivRem->addUse(Right) || !DivRem->build()) {
    return;
  }
  auto AddressDefinition = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_IMPLICIT_DEF));
  if (!AddressDefinition || !AddressDefinition->addDef(Address) || !AddressDefinition->build())
    return;
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  if (!Size || !Align)
    return;
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store};
  Description.AtomicKind = rcc::AtomicOperation::ReadModifyWrite;
  Description.SuccessOrdering = rcc::AtomicOrdering::SequentiallyConsistent;
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = rcc::MemoryBase::stack();
  auto Memory = Fixture.Function->createMemOperand(std::move(Description));
  if (!Memory)
    return;
  auto Atomic = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ATOMICRMW_ADD));
  if (!Atomic || !Atomic->addDef(AtomicResult) || !Atomic->addUse(Address) ||
      !Atomic->addUse(Selected) || !Atomic->addMemOperand(**Memory) || !Atomic->build()) {
    return;
  }

  Fixture.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                    rcc::MachineFunctionProperty::TracksLiveness};
  rcc::MachinePassManager Pipeline;
  if (!Pipeline.addPass(std::make_unique<rcc::LegalizerPass>(rcc::riscv::legalizerInfo())) ||
      !Pipeline.addPass(
          std::make_unique<rcc::RegisterBankSelectPass>(rcc::riscv::registerBankInfo())) ||
      !Pipeline.addPass(
          std::make_unique<rcc::InstructionSelectPass>(rcc::riscv::instructionSelector()))) {
    Context.expect(false, "extended RV64GC pipeline construction must succeed");
    return;
  }
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pipeline.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Run.has_value(), "extended RV64GC pipeline must complete");
  Context.expect(Run && Fixture.Function->properties().has(rcc::MachineFunctionProperty::Selected),
                 "extended pipeline must publish Selected");
  for (const rcc::MachineInstr *Instruction : Fixture.Function->allInstructions()) {
    Context.expect(!rcc::isGenericOpcode(Instruction->opcode()),
                   "extended pipeline must eliminate every generic opcode");
    Context.expect(
        Target->validateInstruction(*Instruction, Fixture.Function->registerInfo()).has_value(),
        "extended pipeline output must satisfy target descriptors");
  }
}

} // namespace

int main() {
  TestContext Context;
  testNullAndMissingContracts(Context);
  testPipelineAndUnsupportedRollback(Context);
  testBankConflict(Context);
  testBankClassOnlyChange(Context);
  testLegalizerCycleDiagnostic(Context);
  testInstructionSelectionTransaction(Context);
  testInstructionSelectionOrder(Context);
  testInstructionSelectionWorklist(Context);
  testExtendedRV64GCPipeline(Context);
  return Context.result();
}
