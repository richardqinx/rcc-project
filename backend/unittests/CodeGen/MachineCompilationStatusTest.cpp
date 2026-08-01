// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCompilationStatus.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

[[nodiscard]] rcc::MachineCompilationFailure
makeFailure(std::string Message = "selection did not match") {
  rcc::MachineCompilationFailure Failure;
  Failure.Diagnostic.Severity = rcc::MachineCompilationDiagnosticSeverity::Error;
  Failure.Diagnostic.Code = rcc::ErrorCode::Unsupported;
  Failure.Diagnostic.Message = std::move(Message);
  Failure.Diagnostic.Location = rcc::SourceLocation{"input.mir", 7, 3, 0};
  Failure.FailurePass = "instruction-select";
  Failure.FailureInstruction = rcc::MachineInstrStableID::fromValue(17);
  Failure.FailureStage = rcc::PipelineStage::Selected;
  Failure.FailureRule = "rv64-test-pattern";
  Failure.Target = "riscv64-unknown-linux-gnu";
  Failure.Subtarget = "rv64gc";
  return Failure;
}

void testStateNames(TestContext &Context) {
  constexpr std::array States{
      rcc::MachineCompilationState::Success,
      rcc::MachineCompilationState::Unsupported,
      rcc::MachineCompilationState::MIRParseFailed,
      rcc::MachineCompilationState::LegalizationFailed,
      rcc::MachineCompilationState::SelectionFailed,
      rcc::MachineCompilationState::RegisterAllocationFailed,
      rcc::MachineCompilationState::CallLoweringFailed,
      rcc::MachineCompilationState::FrameLayoutFailed,
      rcc::MachineCompilationState::FinalizationFailed,
      rcc::MachineCompilationState::AssemblyLoweringFailed,
      rcc::MachineCompilationState::InternalError,
  };
  for (const rcc::MachineCompilationState State : States) {
    Context.expect(rcc::isKnownMachineCompilationState(State) &&
                       rcc::getMachineCompilationStateName(State) != "Unknown",
                   "every public compilation state must have a stable name");
  }
  Context.expect(!rcc::isKnownMachineCompilationState(rcc::MachineCompilationState::Count) &&
                     rcc::getMachineCompilationStateName(rcc::MachineCompilationState::Count) ==
                         "Unknown",
                 "compilation state sentinel must not be accepted as a public state");
}

void testSuccessfulAndFailedStatus(TestContext &Context) {
  rcc::MachineCompilationStatus Status;
  Context.expect(Status.succeeded() && Status.state() == rcc::MachineCompilationState::Success &&
                     Status.failure() == nullptr && Status.verify().has_value(),
                 "default compilation status must represent verified success");

  Context.expect(
      Status.fail(rcc::MachineCompilationState::SelectionFailed, makeFailure()).has_value(),
      "valid structured failure must atomically replace success");
  const rcc::MachineCompilationFailure *Failure = Status.failure();
  Context.expect(!Status.succeeded() &&
                     Status.state() == rcc::MachineCompilationState::SelectionFailed &&
                     Failure != nullptr &&
                     Failure->FailureInstruction == rcc::MachineInstrStableID::fromValue(17) &&
                     Failure->FailureStage == rcc::PipelineStage::Selected &&
                     Failure->Target == "riscv64-unknown-linux-gnu" && Status.verify().has_value(),
                 "failed compilation status must preserve all structured context");

  Context.expect(
      !Status.fail(rcc::MachineCompilationState::InternalError, makeFailure("second failure")),
      "first compilation failure must remain sticky");
  Context.expect(Status.state() == rcc::MachineCompilationState::SelectionFailed &&
                     Status.failure() != nullptr &&
                     Status.failure()->Diagnostic.Message == "selection did not match",
                 "a rejected later failure must not mutate the original status");
}

void testInvalidFailures(TestContext &Context) {
  rcc::MachineCompilationStatus SuccessState;
  Context.expect(!SuccessState.fail(rcc::MachineCompilationState::Success, makeFailure()),
                 "failure API must reject the success state");

  rcc::MachineCompilationStatus UnknownState;
  Context.expect(!UnknownState.fail(rcc::MachineCompilationState::Count, makeFailure()),
                 "failure API must reject an unknown state");

  rcc::MachineCompilationStatus EmptyDiagnostic;
  Context.expect(
      !EmptyDiagnostic.fail(rcc::MachineCompilationState::SelectionFailed, makeFailure({})),
      "failure API must reject an empty diagnostic message");

  auto InvalidLocationFailure = makeFailure();
  InvalidLocationFailure.Diagnostic.Location = rcc::SourceLocation{"", 0, 0, 0};
  rcc::MachineCompilationStatus InvalidLocation;
  Context.expect(!InvalidLocation.fail(rcc::MachineCompilationState::SelectionFailed,
                                       std::move(InvalidLocationFailure)),
                 "failure API must reject an invalid diagnostic location");

  auto InvalidInstructionFailure = makeFailure();
  InvalidInstructionFailure.FailureInstruction = rcc::MachineInstrStableID{};
  rcc::MachineCompilationStatus InvalidInstruction;
  Context.expect(!InvalidInstruction.fail(rcc::MachineCompilationState::SelectionFailed,
                                          std::move(InvalidInstructionFailure)),
                 "failure API must reject an invalid instruction StableID");

  auto InvalidStageFailure = makeFailure();
  InvalidStageFailure.FailureStage = static_cast<rcc::PipelineStage>(255);
  rcc::MachineCompilationStatus InvalidStage;
  Context.expect(!InvalidStage.fail(rcc::MachineCompilationState::SelectionFailed,
                                    std::move(InvalidStageFailure)),
                 "failure API must reject an unknown pipeline stage");

  auto EmptyPassFailure = makeFailure();
  EmptyPassFailure.FailurePass = std::string{};
  rcc::MachineCompilationStatus EmptyPass;
  Context.expect(
      !EmptyPass.fail(rcc::MachineCompilationState::SelectionFailed, std::move(EmptyPassFailure)),
      "failure API must reject empty optional text fields");

  auto InvalidSeverityFailure = makeFailure();
  InvalidSeverityFailure.Diagnostic.Severity = rcc::MachineCompilationDiagnosticSeverity::Count;
  rcc::MachineCompilationStatus InvalidSeverity;
  Context.expect(!InvalidSeverity.fail(rcc::MachineCompilationState::SelectionFailed,
                                       std::move(InvalidSeverityFailure)),
                 "failure API must reject an unknown diagnostic severity");

  auto InvalidCodeFailure = makeFailure();
  InvalidCodeFailure.Diagnostic.Code = static_cast<rcc::ErrorCode>(255);
  rcc::MachineCompilationStatus InvalidCode;
  Context.expect(!InvalidCode.fail(rcc::MachineCompilationState::SelectionFailed,
                                   std::move(InvalidCodeFailure)),
                 "failure API must reject an unknown diagnostic code");
}

} // namespace

int main() {
  TestContext Context;
  testStateNames(Context);
  testSuccessfulAndFailedStatus(Context);
  testInvalidFailures(Context);
  return Context.result();
}
