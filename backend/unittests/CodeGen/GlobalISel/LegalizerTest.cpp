// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/Legalizer.h"

#include "GISelMITest.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <memory>
#include <string>

namespace {

using rcc::test::buildAdd;
using rcc::test::createFixture;
using rcc::test::createFloatRegister;
using rcc::test::createRegister;
using rcc::test::TestContext;

class TogglingLegalizer final : public rcc::LegalizerInfo {
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

class BackwardInvalidatingLegalizer final : public rcc::LegalizerInfo {
public:
  [[nodiscard]] rcc::Expected<rcc::LegalizationAction>
  classify(const rcc::MachineInstr &Instruction, const rcc::MachineFunction &,
           const rcc::TargetDescription &) const override {
    return Instruction.opcode() == rcc::opcode(rcc::GenericOpcode::G_SUB)
               ? rcc::LegalizationAction::Custom
               : rcc::LegalizationAction::Legal;
  }

  [[nodiscard]] rcc::Expected<void> lower(rcc::MachineInstr &Instruction,
                                          rcc::MachineFunction &Function,
                                          const rcc::TargetDescription &,
                                          rcc::LegalizationAction) const override {
    for (rcc::MachineInstr *Candidate : Function.allInstructions()) {
      if (Candidate != &Instruction &&
          Candidate->opcode() == rcc::opcode(rcc::GenericOpcode::G_ADD)) {
        auto Invalidated = Candidate->setOpcode(rcc::opcode(rcc::GenericOpcode::G_SUB));
        if (!Invalidated)
          return std::unexpected(Invalidated.error());
        break;
      }
    }
    return Instruction.setOpcode(
        rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADD)));
  }
};

void testNullAndMissingContracts(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFixture(Context, "pipeline_contracts", 1);
  Context.expect(Target.has_value() && Fixture.Function != nullptr && Fixture.Entry != nullptr,
                 "pipeline contract fixture must be complete");
  if (!Target || !Fixture.Function)
    return;

  rcc::Legalizer NullPass(nullptr);
  rcc::MachineAnalysisManager Analyses;
  auto NullResult = NullPass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(!NullResult && NullResult.error().code() == rcc::ErrorCode::InvalidArgument,
                 "a null legalizer interface must be rejected");
  Context.expect(Fixture.Function->properties().empty(),
                 "a rejected pass must not publish function properties");

  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::Legalizer ValidPass(Subtarget.getLegalizerInfo());
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
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::MachinePassManager OnlyLegalizer;
  Context.expect(
      OnlyLegalizer.addPass(std::make_unique<rcc::Legalizer>(Subtarget.getLegalizerInfo()))
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
  rcc::Legalizer StablePass(Legalizer);
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

  Fixture.Function->properties() = {rcc::MachineFunctionProperty::IsSSA,
                                    rcc::MachineFunctionProperty::TracksLiveness};
  BackwardInvalidatingLegalizer Legalizer;
  rcc::Legalizer Pass(Legalizer);
  rcc::MachineAnalysisManager Analyses;
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
  testLegalizerCycleDiagnostic(Context);
  testLegalizerRevisitsInvalidatedInstructions(Context);
  return Context.result();
}
