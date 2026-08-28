// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/BackendDriver.h"

#include "rcc/CodeGen/MIRPrinter.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/Target/TargetMachine.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

[[nodiscard]] BackendStage outputStage(ArtifactKind Output) noexcept {
  switch (Output) {
  case ArtifactKind::PreLegalizerMIR:
    return BackendStage::PreLegalizerMIR;
  case ArtifactKind::LegalizedMIR:
    return BackendStage::LegalizedMIR;
  case ArtifactKind::RegisterBankMIR:
    return BackendStage::RegisterBankMIR;
  case ArtifactKind::SelectedMIR:
    return BackendStage::SelectedMIR;
  case ArtifactKind::PostSSAMIR:
    return BackendStage::PostSSAMIR;
  case ArtifactKind::PostRegisterAllocationMIR:
    return BackendStage::PostRegisterAllocationMIR;
  case ArtifactKind::FinalMIR:
  case ArtifactKind::Assembly:
  case ArtifactKind::RelocatableObject:
    return BackendStage::FinalMIR;
  }
  return BackendStage::FinalMIR;
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
  if (Kind == ArtifactKind::RelocatableObject || Output.empty())
    return {};
  return {reinterpret_cast<const char *>(Output.data()), Output.size()};
}

Expected<BackendRunResult> runBackend(CanonicalMIR Input, const BackendInvocation &Invocation,
                                      ArtifactKind Output, PipelineControls Controls,
                                      MachinePassInstrumentation *Instrumentation) {
  auto TargetMachine = createTargetMachine(Invocation);
  if (!TargetMachine)
    return std::unexpected(TargetMachine.error());
  const TargetDescription &Target = (*TargetMachine)->targetDescription();
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
    if (Index == 0 || isEarlierStage(Stage, InitialStage))
      InitialStage = Stage;
  }
  const BackendStage RequiredStage = outputStage(Output);
  if (isMIRArtifact(Output) && isEarlierStage(RequiredStage, InitialStage)) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend cannot reconstruct an earlier MIR stage from a later snapshot");
  }

  if (isMIRArtifact(Output)) {
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
  auto Pipeline = runTargetPipeline(Input.Document, **TargetMachine, Invocation,
                                    std::move(Controls), ActiveInstrumentation);
  if (!Pipeline)
    return std::unexpected(Pipeline.error());

  std::vector<std::uint8_t> Bytes;
  if (isMIRArtifact(Output)) {
    auto Printed = printMIR(Input.Document, Target, Invocation, SimplifyMIR);
    if (!Printed)
      return std::unexpected(Printed.error());
    Bytes = std::move(*Printed);
  } else if (Output == ArtifactKind::Assembly) {
    auto Assembly = (*TargetMachine)->emitAssembly(Input.Document.unit());
    if (!Assembly)
      return std::unexpected(Assembly.error());
    Bytes.assign(Assembly->begin(), Assembly->end());
  } else {
    auto Object = (*TargetMachine)->emitObject(Input.Document.unit());
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

} // namespace rcc::tooling
