// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/InstructionSelect.h"

#include "GISel/RISCVRegisterBankInfo.h"
#include "GISelMITest.h"
#include "rcc/CodeGen/GlobalISel/Legalizer.h"
#include "rcc/CodeGen/GlobalISel/RegBankSelect.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <ranges>
#include <vector>

namespace {

using rcc::test::assignGPR;
using rcc::test::buildAdd;
using rcc::test::buildConstant;
using rcc::test::createFixture;
using rcc::test::createRegister;
using rcc::test::TestContext;

class MutatingFailingSelector final : public rcc::InstructionSelector {
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

class RecordingSelector final : public rcc::InstructionSelector {
public:
  explicit RecordingSelector(const rcc::InstructionSelector &Delegate) : Delegate(&Delegate) {}

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
  const rcc::InstructionSelector *Delegate;
  mutable std::vector<std::uint64_t> SelectedIDs;
};

class TwoStepSelector final : public rcc::InstructionSelector {
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

void testPipeline(TestContext &Context) {
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
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::MachineFunctionPassManager Pipeline;
  Context.expect(
      Pipeline.addPass(std::make_unique<rcc::Legalizer>(Subtarget.getLegalizerInfo())).has_value(),
      "legalizer must be accepted by the pass manager");
  Context.expect(Pipeline.addPass(std::make_unique<rcc::RegBankSelect>(Subtarget.getRegBankInfo()))
                     .has_value(),
                 "register-bank selector must be accepted by the pass manager");
  Context.expect(
      Pipeline.addPass(std::make_unique<rcc::InstructionSelect>(Subtarget.getInstructionSelector()))
          .has_value(),
      "instruction selector must be accepted by the pass manager");
  rcc::MachineFunctionAnalysisManager Analyses;
  auto Run = Pipeline.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Run.has_value(), "RV64GC generic pipeline must complete");
  Context.expect(Fixture.Function->properties().has(rcc::MachineFunctionProperty::Selected),
                 "successful instruction selection must publish Selected");
  Context.expect(Fixture.Function->firstBlock()->firstInstruction() != nullptr,
                 "selected instructions must remain attached to their block");
  for (const rcc::MachineInstr *Instruction : Fixture.Function->allInstructions())
    Context.expect(!rcc::isGenericOpcode(Instruction->opcode()),
                   "successful instruction selection must remove generic opcodes");
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
  rcc::InstructionSelect Pass(Selector);
  rcc::MachineFunctionAnalysisManager Analyses;
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
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  RecordingSelector Selector(*Subtarget.getInstructionSelector());
  rcc::InstructionSelect Pass(Selector);
  rcc::MachineFunctionAnalysisManager Analyses;
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
  rcc::InstructionSelect Pass(Selector);
  rcc::MachineFunctionAnalysisManager Analyses;
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
  auto I1 = rcc::LowLevelType::makeCategoryNeutralScalar(1);
  auto I64 = rcc::LowLevelType::makeCategoryNeutralScalar(64);
  auto Pointer = rcc::LowLevelType::makePointer(0, 64);
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
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::MachineFunctionPassManager Pipeline;
  if (!Pipeline.addPass(std::make_unique<rcc::Legalizer>(Subtarget.getLegalizerInfo())) ||
      !Pipeline.addPass(std::make_unique<rcc::RegBankSelect>(Subtarget.getRegBankInfo())) ||
      !Pipeline.addPass(
          std::make_unique<rcc::InstructionSelect>(Subtarget.getInstructionSelector()))) {
    Context.expect(false, "extended RV64GC pipeline construction must succeed");
    return;
  }
  rcc::MachineFunctionAnalysisManager Analyses;
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
  testPipeline(Context);
  testInstructionSelectionTransaction(Context);
  testInstructionSelectionOrder(Context);
  testInstructionSelectionWorklist(Context);
  testExtendedRV64GCPipeline(Context);
  return Context.result();
}
