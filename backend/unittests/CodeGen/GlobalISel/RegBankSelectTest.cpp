// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/RegBankSelect.h"

#include "GISel/RISCVRegisterBankInfo.h"
#include "GISelMITest.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>

namespace {

using rcc::test::buildConstant;
using rcc::test::createFixture;
using rcc::test::createRegister;
using rcc::test::TestContext;

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
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::RegBankSelect Pass(Subtarget.getRegBankInfo());
  rcc::MachineFunctionAnalysisManager Analyses;
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

void testBankSelectionDoesNotAssignAClass(TestContext &Context) {
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
  const rcc::riscv::RISCVSubtarget Subtarget(rcc::riscv::RISCVSubtargetFeatures::rv64gc());
  rcc::RegBankSelect Pass(Subtarget.getRegBankInfo());
  rcc::MachineFunctionAnalysisManager Analyses;
  auto Result = Pass.run(*Fixture.Function, *Target, Analyses);
  Context.expect(Result.has_value() && !Result->Changed,
                 "bank selection must preserve an existing bank-only mapping");
  auto Info = Fixture.Function->registerInfo().virtualRegisterInfo(Value);
  Context.expect(Info && Info->get().Bank && !Info->get().RegClass,
                 "bank selection must not assign a register class");
  if (!Info || !Info->get().Bank)
    return;
  Context.expect(Info->get().Bank.value_or(rcc::RegisterBankID{}) == GPR,
                 "bank selection must preserve the GPR bank");
}

} // namespace

int main() {
  TestContext Context;
  testBankConflict(Context);
  testBankSelectionDoesNotAssignAClass(Context);
  return Context.result();
}
