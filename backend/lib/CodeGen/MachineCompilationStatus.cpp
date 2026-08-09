// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineCompilationStatus.h"

#include "rcc/CodeGen/MachineFunctionCloner.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rcc {
namespace {

constexpr std::array StateNames{
    std::string_view{"Success"},
    std::string_view{"Unsupported"},
    std::string_view{"MIRParseFailed"},
    std::string_view{"LegalizationFailed"},
    std::string_view{"SelectionFailed"},
    std::string_view{"RegisterAllocationFailed"},
    std::string_view{"CallLoweringFailed"},
    std::string_view{"FrameLayoutFailed"},
    std::string_view{"FinalizationFailed"},
    std::string_view{"AssemblyLoweringFailed"},
    std::string_view{"InternalError"},
    std::string_view{"SchedulingFailed"},
};

static_assert(StateNames.size() == static_cast<std::size_t>(MachineCompilationState::Count));

[[nodiscard]] bool
isKnownDiagnosticSeverity(MachineCompilationDiagnosticSeverity Severity) noexcept {
  return static_cast<std::size_t>(Severity) <
         static_cast<std::size_t>(MachineCompilationDiagnosticSeverity::Count);
}

[[nodiscard]] bool isKnownErrorCode(ErrorCode Code) noexcept {
  switch (Code) {
  case ErrorCode::InvalidArgument:
  case ErrorCode::ParseError:
  case ErrorCode::VerificationError:
  case ErrorCode::Unsupported:
  case ErrorCode::RegisterAllocationFailed:
  case ErrorCode::FrameLayoutFailed:
  case ErrorCode::SchedulingFailed:
  case ErrorCode::BranchRelaxationFailed:
  case ErrorCode::PseudoExpansionFailed:
  case ErrorCode::FinalEncodabilityFailed:
  case ErrorCode::CFGOptimizationFailed:
  case ErrorCode::AssemblyLoweringFailed:
  case ErrorCode::InstructionEncodingFailed:
  case ErrorCode::FixupApplicationFailed:
  case ErrorCode::RelocationFailed:
  case ErrorCode::ObjectWritingFailed:
  case ErrorCode::IoError:
  case ErrorCode::InternalError:
    return true;
  }
  return false;
}

[[nodiscard]] bool isKnownPipelineStage(PipelineStage Stage) noexcept {
  switch (Stage) {
  case PipelineStage::MachineIRInput:
  case PipelineStage::Legalized:
  case PipelineStage::RegBankSelected:
  case PipelineStage::Selected:
  case PipelineStage::SSAEliminated:
  case PipelineStage::TiedOpsRewritten:
  case PipelineStage::RegisterAllocated:
  case PipelineStage::FrameFinalized:
  case PipelineStage::PostRAFinalized:
  case PipelineStage::Emission:
    return true;
  }
  return false;
}

[[nodiscard]] Expected<void> verifyOptionalText(const std::optional<std::string> &Text,
                                                std::string_view FieldName) {
  if (Text && Text->empty()) {
    return makeUnexpected(ErrorCode::VerificationError, "machine compilation failure " +
                                                            std::string(FieldName) +
                                                            " cannot be empty");
  }
  return {};
}

} // namespace

bool isKnownMachineCompilationState(MachineCompilationState State) noexcept {
  return static_cast<std::size_t>(State) < StateNames.size();
}

std::string_view getMachineCompilationStateName(MachineCompilationState State) noexcept {
  const auto Index = static_cast<std::size_t>(State);
  if (Index >= StateNames.size())
    return "Unknown";
  return StateNames[Index];
}

Expected<void> MachineCompilationStatus::fail(MachineCompilationState NewState,
                                              MachineCompilationFailure NewFailure) {
  if (!succeeded()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine compilation status already contains a failure");
  }
  if (!isKnownMachineCompilationState(NewState) || NewState == MachineCompilationState::Success) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine compilation failure requires a known failure state");
  }

  MachineCompilationStatus Candidate;
  Candidate.State = NewState;
  Candidate.Failure = std::move(NewFailure);
  auto Verification = Candidate.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  *this = std::move(Candidate);
  return {};
}

Expected<void> MachineCompilationStatus::verify() const {
  if (!isKnownMachineCompilationState(State)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation status has an unknown state");
  }
  if (State == MachineCompilationState::Success) {
    if (Failure) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "successful machine compilation status contains failure details");
    }
    return {};
  }
  if (!Failure) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "failed machine compilation status lacks failure details");
  }

  if (!isKnownDiagnosticSeverity(Failure->Diagnostic.Severity)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation diagnostic has an unknown severity");
  }
  if (!isKnownErrorCode(Failure->Diagnostic.Code)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation diagnostic has an unknown error code");
  }
  if (Failure->Diagnostic.Message.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation diagnostic message cannot be empty");
  }
  if (Failure->Diagnostic.Location && !Failure->Diagnostic.Location->isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation diagnostic has an invalid source location");
  }
  if (Failure->FailureInstruction && !Failure->FailureInstruction->isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation failure has an invalid instruction StableID");
  }
  if (Failure->FailureStage && !isKnownPipelineStage(*Failure->FailureStage)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine compilation failure has an unknown pipeline stage");
  }

  for (const auto &[Text, Name] :
       {std::pair{&Failure->FailurePass, std::string_view{"pass"}},
        std::pair{&Failure->FailureRule, std::string_view{"rule or pattern"}},
        std::pair{&Failure->Target, std::string_view{"target"}},
        std::pair{&Failure->Subtarget, std::string_view{"subtarget"}}}) {
    auto TextVerification = verifyOptionalText(*Text, Name);
    if (!TextVerification)
      return TextVerification;
  }
  return {};
}

Expected<MachineCompilationStatus>
MachineCompilationStatus::clone(const MachineFunctionCloneContext &CloneContext) const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (succeeded())
    return MachineCompilationStatus{};
  if (!Failure) {
    return makeUnexpected(ErrorCode::InternalError,
                          "verified failed compilation status has no failure details");
  }

  MachineCompilationFailure ClonedFailure = *Failure;
  if (ClonedFailure.FailureInstruction) {
    auto Mapped = CloneContext.mapInstructionStableID(*ClonedFailure.FailureInstruction);
    if (!Mapped)
      return std::unexpected(Mapped.error());
    ClonedFailure.FailureInstruction = *Mapped;
  }

  MachineCompilationStatus Result;
  auto FailureResult = Result.fail(State, std::move(ClonedFailure));
  if (!FailureResult)
    return std::unexpected(FailureResult.error());
  return Result;
}

} // namespace rcc
