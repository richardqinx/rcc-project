// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/BackendDriver.h"

#include "rcc/CodeGen/MIRPrinter.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"
#include "rcc/Target/RISCV/RISCVObjectFile.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

[[nodiscard]] bool isMIR(BackendOutputKind Output) noexcept {
  return Output <= BackendOutputKind::FinalMIR;
}

[[nodiscard]] BackendStage outputStage(BackendOutputKind Output) noexcept {
  switch (Output) {
  case BackendOutputKind::PreLegalizerMIR:
    return BackendStage::PreLegalizerMIR;
  case BackendOutputKind::LegalizedMIR:
    return BackendStage::LegalizedMIR;
  case BackendOutputKind::RegisterBankMIR:
    return BackendStage::RegisterBankMIR;
  case BackendOutputKind::SelectedMIR:
    return BackendStage::SelectedMIR;
  case BackendOutputKind::PostSSAMIR:
    return BackendStage::PostSSAMIR;
  case BackendOutputKind::PostRegisterAllocationMIR:
    return BackendStage::PostRegisterAllocationMIR;
  case BackendOutputKind::FinalMIR:
  case BackendOutputKind::Assembly:
  case BackendOutputKind::RelocatableObject:
    return BackendStage::FinalMIR;
  }
  return BackendStage::FinalMIR;
}

[[nodiscard]] std::uint8_t ordinal(BackendStage Stage) noexcept {
  return static_cast<std::uint8_t>(Stage);
}

[[nodiscard]] Expected<std::vector<std::uint8_t>> printMIR(const mir::MIRDocument &Document,
                                                           const TargetDescription &Target,
                                                           const BackendInvocation &Invocation,
                                                           bool Simplify) {
  mir::MIRDocumentPrinterOptions Options;
  Options.Mode = Simplify ? mir::MIPrintMode::Readable : mir::MIPrintMode::Complete;
  Options.EmitEmbeddedIR = true;
  Options.DataLayout = Invocation.DataLayout;
  auto Printed = mir::printMIRDocument(Document, Target, std::move(Options));
  if (!Printed)
    return std::unexpected(Printed.error());
  return std::vector<std::uint8_t>(Printed->begin(), Printed->end());
}

} // namespace

std::string_view BackendRunResult::text() const noexcept {
  if (Kind == BackendOutputKind::RelocatableObject || Output.empty())
    return {};
  return {reinterpret_cast<const char *>(Output.data()), Output.size()};
}

Expected<BackendRunResult> runBackend(CanonicalMIR Input, const TargetDescription &Target,
                                      const BackendInvocation &Invocation, BackendOutputKind Output,
                                      PipelineControls Controls,
                                      MachinePassInstrumentation *Instrumentation) {
  auto InvocationVerification = verifyRV64GCInvocation(Invocation);
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  if (Input.Document.unit().functionCount() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "backend input must contain at least one machine function");
  }

  BackendStage InitialStage = BackendStage::FinalMIR;
  for (std::size_t Index = 0; Index < Input.Document.unit().functionCount(); ++Index) {
    const MachineFunction *Function = Input.Document.unit().functionAt(Index);
    if (Function == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "backend input contains a null function");
    const BackendStage Stage = stageForFunction(*Function);
    if (Index == 0 || ordinal(Stage) < ordinal(InitialStage))
      InitialStage = Stage;
  }
  const BackendStage RequiredStage = outputStage(Output);
  if (isMIR(Output) && ordinal(RequiredStage) < ordinal(InitialStage)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend cannot reconstruct an earlier MIR stage from a later snapshot");
  }

  if (isMIR(Output)) {
    if (Controls.StopAtStage && *Controls.StopAtStage != RequiredStage) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "output stage conflicts with the explicit pipeline stop stage");
    }
    Controls.StopAtStage = RequiredStage;
  }

  const bool SimplifyMIR = Controls.SimplifyMIR;

  MachinePassInstrumentation LocalInstrumentation;
  MachinePassInstrumentation *ActiveInstrumentation =
      Instrumentation == nullptr ? &LocalInstrumentation : Instrumentation;
  auto Pipeline = runRV64GCPipeline(Input.Document, Target, Invocation, std::move(Controls),
                                    ActiveInstrumentation);
  if (!Pipeline)
    return std::unexpected(Pipeline.error());

  std::vector<std::uint8_t> Bytes;
  if (isMIR(Output)) {
    auto Printed = printMIR(Input.Document, Target, Invocation, SimplifyMIR);
    if (!Printed)
      return std::unexpected(Printed.error());
    Bytes = std::move(*Printed);
  } else if (Output == BackendOutputKind::Assembly) {
    auto Assembly = riscv::emitAssembly(Input.Document.unit(), Target);
    if (!Assembly)
      return std::unexpected(Assembly.error());
    Bytes.assign(Assembly->begin(), Assembly->end());
  } else {
    auto Object = riscv::emitObject(Input.Document.unit(), Target);
    if (!Object)
      return std::unexpected(Object.error());
    Bytes = std::move(*Object);
  }

  return BackendRunResult{
      .Kind = Output,
      .Output = std::move(Bytes),
      .Pipeline = std::move(*Pipeline),
      .Actions = ActiveInstrumentation->actionEvents(),
  };
}

std::string_view spelling(BackendOutputKind Output) noexcept {
  switch (Output) {
  case BackendOutputKind::PreLegalizerMIR:
    return "prelegalizer-mir";
  case BackendOutputKind::LegalizedMIR:
    return "legalized-mir";
  case BackendOutputKind::RegisterBankMIR:
    return "regbank-mir";
  case BackendOutputKind::SelectedMIR:
    return "selected-mir";
  case BackendOutputKind::PostSSAMIR:
    return "post-ssa-mir";
  case BackendOutputKind::PostRegisterAllocationMIR:
    return "post-ra-mir";
  case BackendOutputKind::FinalMIR:
    return "final-mir";
  case BackendOutputKind::Assembly:
    return "assembly";
  case BackendOutputKind::RelocatableObject:
    return "object";
  }
  return "invalid";
}

ArtifactKind manifestArtifactKind(BackendOutputKind Output) noexcept {
  switch (Output) {
  case BackendOutputKind::PreLegalizerMIR:
    return ArtifactKind::PreLegalizerMIR;
  case BackendOutputKind::LegalizedMIR:
    return ArtifactKind::LegalizedMIR;
  case BackendOutputKind::RegisterBankMIR:
    return ArtifactKind::RegisterBankMIR;
  case BackendOutputKind::SelectedMIR:
    return ArtifactKind::SelectedMIR;
  case BackendOutputKind::PostSSAMIR:
    return ArtifactKind::PostSSAMIR;
  case BackendOutputKind::PostRegisterAllocationMIR:
    return ArtifactKind::PostRegisterAllocationMIR;
  case BackendOutputKind::FinalMIR:
    return ArtifactKind::FinalMIR;
  case BackendOutputKind::Assembly:
    return ArtifactKind::Assembly;
  case BackendOutputKind::RelocatableObject:
    return ArtifactKind::RelocatableObject;
  }
  return ArtifactKind::FinalMIR;
}

Expected<ArtifactKind> cacheArtifactKind(BackendOutputKind Output) {
  switch (Output) {
  case BackendOutputKind::PreLegalizerMIR:
    return ArtifactKind::PreLegalizerMIR;
  case BackendOutputKind::SelectedMIR:
    return ArtifactKind::SelectedMIR;
  case BackendOutputKind::PostRegisterAllocationMIR:
    return ArtifactKind::PostRegisterAllocationMIR;
  case BackendOutputKind::FinalMIR:
    return ArtifactKind::FinalMIR;
  case BackendOutputKind::Assembly:
    return ArtifactKind::Assembly;
  case BackendOutputKind::RelocatableObject:
    return ArtifactKind::RelocatableObject;
  case BackendOutputKind::LegalizedMIR:
  case BackendOutputKind::RegisterBankMIR:
  case BackendOutputKind::PostSSAMIR:
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "the selected intermediate stage is not a cache boundary");
  }
  return makeUnexpected(ErrorCode::InvalidInvocation, "invalid backend output kind");
}

} // namespace rcc::tooling
