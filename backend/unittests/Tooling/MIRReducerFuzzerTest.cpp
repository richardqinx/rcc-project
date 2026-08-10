// SPDX-License-Identifier: GPL-3.0-or-later

#include "ToolingTestSupport.h"
#include "rcc/Target/RISCV/RISCVTarget.h"
#include "rcc/Tooling/BackendDriver.h"
#include "rcc/Tooling/MIRFuzzer.h"
#include "rcc/Tooling/MIRReducer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] bool hasErrorCode(const auto &Result, rcc::ErrorCode Code) {
  return !Result && Result.error().code() == Code;
}

[[nodiscard]] bool hasReductionStep(const rcc::tooling::MIRReductionResult &Result,
                                    rcc::tooling::ReductionGranularity Granularity) {
  for (const rcc::tooling::MIRReductionStep &Step : Result.Steps) {
    if (Step.Granularity == Granularity)
      return true;
  }
  return false;
}

[[nodiscard]] rcc::Expected<std::string> addDeadInstruction(std::string Source) {
  constexpr std::string_view Anchor = "    $x10 = COPY %2\n";
  const std::size_t Position = Source.find(Anchor);
  if (Position == std::string::npos) {
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError,
                               "test MIR does not contain the expected COPY");
  }
  Source.insert(Position, "    %3:gpr(i64) = G_CONSTANT 99\n");
  return Source;
}

} // namespace

int main() {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return tooling_test::fail(Target.error().message());
  const rcc::tooling::BackendInvocation Invocation = rcc::tooling::defaultRV64GCInvocation();
  auto Source = tooling_test::readInput("RISCVGenericPipeline.mir");
  if (!Source)
    return tooling_test::fail(Source.error().message());
  auto ReducibleSource = addDeadInstruction(*Source);
  if (!ReducibleSource)
    return tooling_test::fail(ReducibleSource.error().message());
  auto CanonicalReducible = rcc::tooling::parseAndCanonicalizeMIR(
      "validity-seed.mir", *ReducibleSource, *Target, Invocation);
  if (!CanonicalReducible)
    return tooling_test::fail(CanonicalReducible.error().message());

  rcc::tooling::MIRReducerOptions ValidityOptions;
  ValidityOptions.Mode = rcc::tooling::ReductionMode::PreserveValidity;
  ValidityOptions.MaximumAttempts = 128;
  ValidityOptions.Granularities = {rcc::tooling::ReductionGranularity::Instruction};
  auto ValidityReduction = rcc::tooling::reduceMIR("validity-reducer.mir", *ReducibleSource,
                                                   Invocation, *Target, {}, ValidityOptions);
  if (!ValidityReduction || ValidityReduction->MIR.size() >= CanonicalReducible->Text.size() ||
      !hasReductionStep(*ValidityReduction, rcc::tooling::ReductionGranularity::Instruction)) {
    return tooling_test::fail(
        "validity reduction must remove a dead instruction and record the accepted step");
  }
  auto ReducedCanonical = rcc::tooling::parseAndCanonicalizeMIR(
      "validity-reduced.mir", ValidityReduction->MIR, *Target, ValidityReduction->Invocation);
  if (!ReducedCanonical || ReducedCanonical->Text != ValidityReduction->MIR)
    return tooling_test::fail("validity reduction must return canonical verified MIR");

  rcc::tooling::MIRReducerOptions FeatureOptions;
  FeatureOptions.Mode = rcc::tooling::ReductionMode::PreserveFailure;
  FeatureOptions.MaximumAttempts = 128;
  FeatureOptions.Granularities = {rcc::tooling::ReductionGranularity::TargetFeature};
  const auto KeepTwoFeatures = [](std::string_view,
                                  const rcc::tooling::BackendInvocation &Candidate) {
    return rcc::Expected<bool>(Candidate.Features.size() >= 2);
  };
  auto FeatureReduction = rcc::tooling::reduceMIR("feature-reducer.mir", *Source, Invocation,
                                                  *Target, KeepTwoFeatures, FeatureOptions);
  if (!FeatureReduction || FeatureReduction->Invocation.Features.size() != 2 ||
      !hasReductionStep(*FeatureReduction, rcc::tooling::ReductionGranularity::TargetFeature)) {
    return tooling_test::fail("target-feature reduction must preserve its interestingness limit");
  }

  rcc::tooling::BackendInvocation PassInvocation = Invocation;
  PassInvocation.PassPipeline = {"Legalizer", "RegisterBankSelect", "InstructionSelect"};
  rcc::tooling::MIRReducerOptions PassOptions;
  PassOptions.Mode = rcc::tooling::ReductionMode::PreserveFailure;
  PassOptions.MaximumAttempts = 32;
  PassOptions.Granularities = {rcc::tooling::ReductionGranularity::PassPipeline};
  const auto KeepOnePass = [](std::string_view, const rcc::tooling::BackendInvocation &Candidate) {
    return rcc::Expected<bool>(!Candidate.PassPipeline.empty());
  };
  auto PassReduction = rcc::tooling::reduceMIR("pass-reducer.mir", *Source, PassInvocation, *Target,
                                               KeepOnePass, PassOptions);
  if (!PassReduction || PassReduction->Invocation.PassPipeline.size() != 1 ||
      !hasReductionStep(*PassReduction, rcc::tooling::ReductionGranularity::PassPipeline)) {
    return tooling_test::fail("pass-pipeline reduction must retain one interesting pass");
  }

  auto RegBankInput = rcc::tooling::parseAndCanonicalizeMIR("failure-seed.mir", *ReducibleSource,
                                                            *Target, Invocation);
  if (!RegBankInput)
    return tooling_test::fail(RegBankInput.error().message());
  auto RegBank = rcc::tooling::runBackend(std::move(*RegBankInput), *Target, Invocation,
                                          rcc::tooling::BackendOutputKind::RegisterBankMIR);
  if (!RegBank)
    return tooling_test::fail(RegBank.error().message());
  rcc::tooling::BackendInvocation FailureInvocation = Invocation;
  FailureInvocation.PassPipeline = {"InstructionSelect"};
  FailureInvocation.ActionFuel = 0;
  const auto PreservesActionFailure = [&](std::string_view MIR,
                                          const rcc::tooling::BackendInvocation &Candidate) {
    auto Input = rcc::tooling::parseAndCanonicalizeMIR("failure-candidate.mir", std::string(MIR),
                                                       *Target, Candidate);
    if (!Input)
      return rcc::Expected<bool>(std::unexpected(Input.error()));
    auto Run = rcc::tooling::runBackend(std::move(*Input), *Target, Candidate,
                                        rcc::tooling::BackendOutputKind::SelectedMIR);
    return rcc::Expected<bool>(!Run && Run.error().code() == rcc::ErrorCode::VerificationError);
  };
  rcc::tooling::MIRReducerOptions FailureOptions;
  FailureOptions.Mode = rcc::tooling::ReductionMode::PreserveFailure;
  FailureOptions.MaximumAttempts = 128;
  FailureOptions.Granularities = {rcc::tooling::ReductionGranularity::Instruction};
  auto FailureReduction =
      rcc::tooling::reduceMIR("failure-reducer.mir", std::string(RegBank->text()),
                              FailureInvocation, *Target, PreservesActionFailure, FailureOptions);
  if (!FailureReduction ||
      !hasReductionStep(*FailureReduction, rcc::tooling::ReductionGranularity::Instruction)) {
    return tooling_test::fail("failure reduction must retain the Action/Fuel failure");
  }
  auto StillInteresting =
      PreservesActionFailure(FailureReduction->MIR, FailureReduction->Invocation);
  if (!StillInteresting || !*StillInteresting)
    return tooling_test::fail("reduced failure input must still satisfy its predicate");

  rcc::tooling::MIRReducerOptions InvalidReducerOptions = ValidityOptions;
  InvalidReducerOptions.MaximumAttempts = 0;
  auto ZeroBudget = rcc::tooling::reduceMIR("zero-budget.mir", *Source, Invocation, *Target, {},
                                            InvalidReducerOptions);
  if (!hasErrorCode(ZeroBudget, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("reducer must reject a zero attempt budget");
  InvalidReducerOptions = FeatureOptions;
  auto MissingPredicate = rcc::tooling::reduceMIR("missing-predicate.mir", *Source, Invocation,
                                                  *Target, {}, InvalidReducerOptions);
  if (!hasErrorCode(MissingPredicate, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("failure reduction must require an interestingness predicate");
  const auto NeverInteresting = [](std::string_view, const rcc::tooling::BackendInvocation &) {
    return rcc::Expected<bool>(false);
  };
  auto FalsePredicate = rcc::tooling::reduceMIR("false-predicate.mir", *Source, Invocation, *Target,
                                                NeverInteresting, FeatureOptions);
  if (!hasErrorCode(FalsePredicate, rcc::ErrorCode::ReductionFailed))
    return tooling_test::fail("reducer must reject an initially uninteresting input");
  InvalidReducerOptions = ValidityOptions;
  InvalidReducerOptions.Granularities = {
      rcc::tooling::ReductionGranularity::Instruction,
      rcc::tooling::ReductionGranularity::Instruction,
  };
  auto DuplicateGranularity = rcc::tooling::reduceMIR(
      "duplicate-granularity.mir", *Source, Invocation, *Target, {}, InvalidReducerOptions);
  if (!hasErrorCode(DuplicateGranularity, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("reducer must reject duplicate granularities");
  volatile std::uint8_t InvalidGranularityValue = 255;
  InvalidReducerOptions.Granularities = {
      static_cast<rcc::tooling::ReductionGranularity>(InvalidGranularityValue)};
  auto InvalidGranularity = rcc::tooling::reduceMIR("invalid-granularity.mir", *Source, Invocation,
                                                    *Target, {}, InvalidReducerOptions);
  if (!hasErrorCode(InvalidGranularity, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("reducer must reject an unknown granularity");

  rcc::tooling::MIRFuzzOptions FuzzOptions;
  FuzzOptions.Seed = 0x123456789abcdef0ULL;
  FuzzOptions.Iterations = 2;
  auto FirstFuzz =
      rcc::tooling::fuzzMIR("fuzz-seed.mir", *Source, Invocation, *Target, FuzzOptions);
  auto SecondFuzz =
      rcc::tooling::fuzzMIR("fuzz-seed.mir", *Source, Invocation, *Target, FuzzOptions);
  if (!FirstFuzz || !SecondFuzz || FirstFuzz->Seed != FuzzOptions.Seed ||
      FirstFuzz->Layers != SecondFuzz->Layers || FirstFuzz->Layers.size() != 6 ||
      FirstFuzz->executed() != 11 ||
      FirstFuzz->accepted() + FirstFuzz->rejected() != FirstFuzz->executed()) {
    return tooling_test::fail("all six fuzz layers must produce deterministic accounting");
  }
  constexpr std::array LayerNames{
      std::string_view("parser"),         std::string_view("builder"),
      std::string_view("property-stage"), std::string_view("mutation"),
      std::string_view("pipeline"),       std::string_view("semantics"),
  };
  for (std::size_t Index = 0; Index < LayerNames.size(); ++Index) {
    auto Parsed = rcc::tooling::parseMIRFuzzLayer(LayerNames[Index]);
    if (!Parsed || rcc::tooling::spelling(*Parsed) != LayerNames[Index] ||
        FirstFuzz->Layers[Index].Layer != *Parsed) {
      return tooling_test::fail("fuzz layer spelling and parsing must round-trip");
    }
  }
  if (rcc::tooling::parseMIRFuzzLayer("unknown"))
    return tooling_test::fail("fuzz layer parser must reject unknown names");

  rcc::tooling::MIRFuzzOptions InvalidFuzzOptions = FuzzOptions;
  InvalidFuzzOptions.Iterations = 0;
  auto ZeroIterations = rcc::tooling::fuzzMIR("zero-iterations.mir", *Source, Invocation, *Target,
                                              InvalidFuzzOptions);
  if (!hasErrorCode(ZeroIterations, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("fuzzer must reject zero iterations");
  InvalidFuzzOptions = FuzzOptions;
  InvalidFuzzOptions.MaximumInputSize = 0;
  auto ZeroInputLimit = rcc::tooling::fuzzMIR("zero-input-limit.mir", *Source, Invocation, *Target,
                                              InvalidFuzzOptions);
  if (!hasErrorCode(ZeroInputLimit, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("fuzzer must reject a zero input-size limit");
  InvalidFuzzOptions = FuzzOptions;
  InvalidFuzzOptions.MaximumInputSize = 1;
  auto OversizedSeed =
      rcc::tooling::fuzzMIR("oversized-seed.mir", *Source, Invocation, *Target, InvalidFuzzOptions);
  if (!hasErrorCode(OversizedSeed, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("fuzzer must reject a seed larger than its input limit");
  InvalidFuzzOptions = FuzzOptions;
  InvalidFuzzOptions.Layers = {rcc::tooling::MIRFuzzLayer::Parser,
                               rcc::tooling::MIRFuzzLayer::Parser};
  auto DuplicateLayer = rcc::tooling::fuzzMIR("duplicate-layer.mir", *Source, Invocation, *Target,
                                              InvalidFuzzOptions);
  if (!hasErrorCode(DuplicateLayer, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("fuzzer must reject duplicate layers");
  volatile std::uint8_t InvalidFuzzLayerValue = 255;
  InvalidFuzzOptions.Layers = {static_cast<rcc::tooling::MIRFuzzLayer>(InvalidFuzzLayerValue)};
  auto InvalidLayer =
      rcc::tooling::fuzzMIR("invalid-layer.mir", *Source, Invocation, *Target, InvalidFuzzOptions);
  if (!hasErrorCode(InvalidLayer, rcc::ErrorCode::InvalidArgument))
    return tooling_test::fail("fuzzer must reject an unknown layer");

  return 0;
}
