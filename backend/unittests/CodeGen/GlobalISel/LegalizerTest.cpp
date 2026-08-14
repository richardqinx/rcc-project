// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/Legalizer.h"

#include "GISelMITest.h"
#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/GlobalISel/LegalizerHelper.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <memory>
#include <string>

namespace {

using rcc::test::buildAdd;
using rcc::test::createFixture;
using rcc::test::createRegister;
using rcc::test::TestContext;

class BackwardInvalidatingLegalizer final : public rcc::LegalizerInfo {
public:
  BackwardInvalidatingLegalizer() {
    getActionDefinitionsBuilder(rcc::opcode(rcc::GenericOpcode::G_ADD)).alwaysLegal();
    getActionDefinitionsBuilder(rcc::opcode(rcc::GenericOpcode::G_SUB)).custom();
  }

  [[nodiscard]] rcc::Expected<void>
  legalizeCustom(rcc::LegalizerHelper &Helper, rcc::MachineInstr &Instruction,
                 const rcc::LegalizationDecision &) const override {
    rcc::MachineFunction &Function = Helper.MIRBuilder.function();
    for (rcc::MachineInstr *Candidate : Function.allInstructions()) {
      if (Candidate != &Instruction &&
          Candidate->opcode() == rcc::opcode(rcc::GenericOpcode::G_ADD)) {
        Helper.Observer.changingInstr(*Candidate);
        auto Invalidated = Candidate->setOpcode(rcc::opcode(rcc::GenericOpcode::G_SUB));
        if (!Invalidated)
          return std::unexpected(Invalidated.error());
        Helper.Observer.changedInstr(*Candidate);
        break;
      }
    }
    Helper.Observer.changingInstr(Instruction);
    auto Changed = Instruction.setOpcode(
        rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADD)));
    if (!Changed)
      return std::unexpected(Changed.error());
    Helper.Observer.changedInstr(Instruction);
    return {};
  }
};

void testNullAndMissingContracts(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_contracts", 1);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "pipeline contract fixture must be complete");
  if (!Target || !Fixture.Function)
    return;

  rcc::Legalizer NullPass(nullptr, rcc::OptimizationLevel::O2);
  rcc::MachineFunctionAnalysisManager Analyses;
  auto NullResult = NullPass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!NullResult && NullResult.error().code() == rcc::ErrorCode::InvalidArgument,
                 "a null legalizer interface must be rejected");
  Context.expect(Fixture.Function->properties().empty(),
                 "a rejected pass must not publish function properties");

  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::Legalizer ValidPass(Subtarget.getLegalizerInfo(), rcc::OptimizationLevel::O2);
  auto MissingResult = ValidPass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!MissingResult &&
                     MissingResult.error().code() == rcc::ErrorCode::VerificationError,
                 "a pass with missing prerequisites must be rejected");
}

void testUnsupportedRollback(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return;
  auto Unsupported = createFixture(Context, "pipeline_unsupported", 3);
  Context.expect(Unsupported.Function != nullptr && Unsupported.Entry != nullptr,
                 "unsupported pipeline fixture must be complete");
  if (!Unsupported.Function || Unsupported.Entry == nullptr)
    return;
  const rcc::Register UnsupportedReg = createRegister(Context, *Unsupported.Function, 64);
  rcc::MachineIRBuilder UnsupportedBuilder(*Unsupported.Function, *Target);
  Context.expect(UnsupportedBuilder.setInsertPoint(*Unsupported.Entry).has_value(),
                 "unsupported pipeline insertion point must be valid");
  if (UnsupportedReg.isNoRegister())
    return;
  auto Pending = UnsupportedBuilder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_GET_FPENV));
  Context.expect(Pending.has_value(), "unsupported generic instruction must be constructible");
  if (!Pending)
    return;
  auto Built = Pending->addDef(UnsupportedReg);
  Context.expect(Built.has_value(), "unsupported generic definition must be accepted");
  if (!Built)
    return;
  Context.expect(Pending->build().has_value(),
                 "unsupported floating-point environment operation must be constructible");
  Unsupported.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                        rcc::MachineFunctionProperty::TracksLiveness};
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::MachineFunctionPassManager OnlyLegalizer;
  Context.expect(OnlyLegalizer
                     .addPass(std::make_unique<rcc::Legalizer>(Subtarget.getLegalizerInfo(),
                                                               rcc::OptimizationLevel::O2))
                     .has_value(),
                 "standalone legalizer must be accepted");
  rcc::MachineFunctionAnalysisManager UnsupportedAnalyses;
  auto Failure = OnlyLegalizer.run(*Unsupported.Function, *Target, UnsupportedAnalyses);
  Context.expect(!Failure && Failure.error().code() == rcc::ErrorCode::Unsupported,
                 "unsupported generic instructions must produce a target diagnostic");
  Context.expect(!Failure && Failure.error().message().find("G_GET_FPENV") != std::string::npos,
                 "unsupported legalization diagnostics must name the rejected opcode");
  Context.expect(
      Unsupported.Function->properties().has(rcc::MachineFunctionProperty::IsSSA) &&
          !Unsupported.Function->properties().has(rcc::MachineFunctionProperty::Legalized),
      "failed legalizer execution must not publish Legalized");
}

void testLegalizerRevisitsInvalidatedInstructions(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_legalizer_revisit", 7);
  if (!Target || !Fixture.Function || Fixture.Entry == nullptr)
    return;
  const rcc::Register Left = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register Right = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register FirstResult = createRegister(Context, *Fixture.Function, 64);
  const rcc::Register SecondResult = createRegister(Context, *Fixture.Function, 64);
  rcc::MachineIRBuilder Builder(*Fixture.Function, *Target);
  if (Left.isNoRegister() || Right.isNoRegister() || FirstResult.isNoRegister() ||
      SecondResult.isNoRegister() || !Builder.setInsertPoint(*Fixture.Entry) ||
      !buildAdd(Context, Builder, FirstResult, Left, Right) ||
      !buildAdd(Context, Builder, SecondResult, Left, Right))
    return;
  rcc::MachineInstr *First = Fixture.Entry->firstInstruction();
  rcc::MachineInstr *Second = First == nullptr ? nullptr : First->next();
  if (Second == nullptr || !Second->setOpcode(rcc::opcode(rcc::GenericOpcode::G_SUB)))
    return;
  auto X10 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X10);
  auto X11 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X11);
  auto FirstCopy = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  auto SecondCopy = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!X10 || !X11 || !FirstCopy || !FirstCopy->addDef(rcc::Register::fromPhysical(*X10)) ||
      !FirstCopy->addUse(FirstResult) || !FirstCopy->build() || !SecondCopy ||
      !SecondCopy->addDef(rcc::Register::fromPhysical(*X11)) || !SecondCopy->addUse(SecondResult) ||
      !SecondCopy->build())
    return;

  Fixture.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                    rcc::MachineFunctionProperty::TracksLiveness};
  BackwardInvalidatingLegalizer Legalizer;
  rcc::Legalizer Pass(Legalizer, rcc::OptimizationLevel::O2);
  rcc::MachineFunctionAnalysisManager Analyses;
  auto Run = Pass.run(*Fixture.Function, *Target, Analyses);
  const rcc::Opcode LegalOpcode =
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADD));
  Context.expect(Run && First->opcode() == LegalOpcode && Second->opcode() == LegalOpcode,
                 "legalizer must revisit an earlier instruction invalidated by a later lowering");
}

} // namespace

int main() {
  TestContext Context;
  testNullAndMissingContracts(Context);
  testUnsupportedRollback(Context);
  testLegalizerRevisitsInvalidatedInstructions(Context);
  return Context.result();
}
