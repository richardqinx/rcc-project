// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/PipelineDriver.h"

#include "ToolingTestSupport.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/MIR/MIRDocument.h"
#include "rcc/Target/RISCV/RISCVTarget.h"
#include "rcc/Tooling/BackendDriver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] rcc::Expected<std::string>
printDocument(const rcc::mir::MIRDocument &Document, const rcc::TargetDescription &Target,
              const rcc::tooling::BackendInvocation &Invocation) {
  rcc::mir::MIRDocumentPrinterOptions Options;
  Options.Mode = rcc::mir::MIPrintMode::Complete;
  Options.EmitEmbeddedIR = true;
  Options.DataLayout = Invocation.DataLayout;
  return rcc::mir::printMIRDocument(Document, Target, std::move(Options));
}

[[nodiscard]] rcc::Expected<rcc::tooling::BackendRunResult>
runInput(std::string_view SourceName, std::string SourceText, const rcc::TargetDescription &Target,
         const rcc::tooling::BackendInvocation &Invocation, rcc::tooling::BackendOutputKind Output,
         rcc::tooling::PipelineControls Controls = {}) {
  auto Input = rcc::tooling::parseAndCanonicalizeMIR(std::string(SourceName), std::move(SourceText),
                                                     Target, Invocation);
  if (!Input)
    return std::unexpected(Input.error());
  return rcc::tooling::runBackend(std::move(*Input), Target, Invocation, Output,
                                  std::move(Controls));
}

[[nodiscard]] rcc::Expected<std::string> makeMultiFunctionInput() {
  auto First = tooling_test::readInput("RISCVGenericPipeline.mir");
  if (!First)
    return std::unexpected(First.error());
  std::string Second = *First;
  constexpr std::string_view OriginalName = "name: generic_pipeline";
  const std::size_t Name = Second.find(OriginalName);
  if (Name == std::string::npos)
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "test MIR function name is absent");
  Second.replace(Name, OriginalName.size(), "name: generic_pipeline_second");
  First->push_back('\n');
  First->append(Second);
  return First;
}

struct DeterministicOutputs final {
  std::vector<std::uint8_t> FinalMIR;
  std::vector<std::uint8_t> Assembly;
  std::vector<std::uint8_t> Object;
  std::vector<rcc::MachineActionEvent> Actions;
  std::vector<rcc::tooling::ExecutedPass> ExecutedPasses;
};

[[nodiscard]] rcc::Expected<DeterministicOutputs>
runDeterminismCase(std::string_view Input, const rcc::TargetDescription &Target,
                   std::uint32_t ThreadCount) {
  rcc::tooling::BackendInvocation Invocation = rcc::tooling::defaultRV64GCInvocation();
  Invocation.Determinism.ThreadCount = ThreadCount;
  auto Final = runInput("determinism-final.mir", std::string(Input), Target, Invocation,
                        rcc::tooling::BackendOutputKind::FinalMIR);
  if (!Final)
    return std::unexpected(Final.error());
  auto Assembly = runInput("determinism-assembly.mir", std::string(Input), Target, Invocation,
                           rcc::tooling::BackendOutputKind::Assembly);
  if (!Assembly)
    return std::unexpected(Assembly.error());
  auto Object = runInput("determinism-object.mir", std::string(Input), Target, Invocation,
                         rcc::tooling::BackendOutputKind::RelocatableObject);
  if (!Object)
    return std::unexpected(Object.error());
  return DeterministicOutputs{
      .FinalMIR = std::move(Final->Output),
      .Assembly = std::move(Assembly->Output),
      .Object = std::move(Object->Output),
      .Actions = std::move(Final->Actions),
      .ExecutedPasses = std::move(Final->Pipeline.ExecutedPasses),
  };
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

  rcc::tooling::BackendInvocation AtomicOnlyInvocation = Invocation;
  AtomicOnlyInvocation.Features = {"+a", "+zacas", "+zabha"};
  AtomicOnlyInvocation.PassPipeline = {"Legalizer"};
  auto CanonicalAtomicOnly = AtomicOnlyInvocation.canonicalizeAndVerify();
  if (!CanonicalAtomicOnly)
    return tooling_test::fail(CanonicalAtomicOnly.error().message());
  auto AtomicOnlyRun = runInput("atomic-only.mir", *Source, *Target, AtomicOnlyInvocation,
                                rcc::tooling::BackendOutputKind::LegalizedMIR);
  if (!AtomicOnlyRun)
    return tooling_test::fail(AtomicOnlyRun.error().message());

  rcc::tooling::BackendInvocation UnsupportedFeatureInvocation = Invocation;
  UnsupportedFeatureInvocation.Features = {"+v"};
  auto CanonicalUnsupported = UnsupportedFeatureInvocation.canonicalizeAndVerify();
  if (!CanonicalUnsupported)
    return tooling_test::fail(CanonicalUnsupported.error().message());
  if (rcc::tooling::verifyRV64GCInvocation(UnsupportedFeatureInvocation))
    return tooling_test::fail("RV64 backend invocation must reject unsupported target features");

  constexpr std::array StageCases{
      std::pair{rcc::tooling::BackendOutputKind::PreLegalizerMIR,
                rcc::tooling::BackendStage::PreLegalizerMIR},
      std::pair{rcc::tooling::BackendOutputKind::LegalizedMIR,
                rcc::tooling::BackendStage::LegalizedMIR},
      std::pair{rcc::tooling::BackendOutputKind::RegisterBankMIR,
                rcc::tooling::BackendStage::RegisterBankMIR},
      std::pair{rcc::tooling::BackendOutputKind::SelectedMIR,
                rcc::tooling::BackendStage::SelectedMIR},
      std::pair{rcc::tooling::BackendOutputKind::PostSSAMIR,
                rcc::tooling::BackendStage::PostSSAMIR},
      std::pair{rcc::tooling::BackendOutputKind::PostRegisterAllocationMIR,
                rcc::tooling::BackendStage::PostRegisterAllocationMIR},
      std::pair{rcc::tooling::BackendOutputKind::FinalMIR, rcc::tooling::BackendStage::FinalMIR},
  };
  std::optional<std::string> SelectedMIR;
  for (const auto &[Output, ExpectedStage] : StageCases) {
    auto Run = runInput("stage.mir", *Source, *Target, Invocation, Output);
    if (!Run)
      return tooling_test::fail(Run.error().message());
    auto Parsed = rcc::tooling::parseAndCanonicalizeMIR(
        "stage-output.mir", std::string(Run->text()), *Target, Invocation);
    if (!Parsed || Parsed->Document.unit().functionCount() != 1)
      return tooling_test::fail("every MIR stage output must strictly parse and verify");
    const rcc::MachineFunction *Function = Parsed->Document.unit().functionAt(0);
    if (Function == nullptr)
      return tooling_test::fail("MIR stage output must contain a machine function");
    const rcc::tooling::BackendStage ActualStage = rcc::tooling::stageForFunction(*Function);
    if (ActualStage != ExpectedStage) {
      return tooling_test::fail(
          "MIR output requested " + std::string(rcc::tooling::spelling(ExpectedStage)) +
          " but reparsed as " + std::string(rcc::tooling::spelling(ActualStage)));
    }
    if (Output == rcc::tooling::BackendOutputKind::SelectedMIR)
      SelectedMIR = std::string(Run->text());
  }

  auto Assembly = runInput("assembly.mir", *Source, *Target, Invocation,
                           rcc::tooling::BackendOutputKind::Assembly);
  if (!Assembly || !Assembly->text().contains("generic_pipeline") ||
      !Assembly->text().contains("jalr x0, x1, 0")) {
    return tooling_test::fail("assembly output must use the complete RV64GC production pipeline");
  }
  auto Object = runInput("object.mir", *Source, *Target, Invocation,
                         rcc::tooling::BackendOutputKind::RelocatableObject);
  constexpr std::array<std::uint8_t, 4> ELFMagic{0x7fU, 'E', 'L', 'F'};
  if (!Object || Object->Output.size() < ELFMagic.size() ||
      !std::ranges::equal(ELFMagic, std::span<const std::uint8_t>(Object->Output).first(4))) {
    return tooling_test::fail("object output must be a native ELF relocatable object");
  }

  auto SnapshotInput =
      rcc::tooling::parseAndCanonicalizeMIR("snapshots.mir", *Source, *Target, Invocation);
  if (!SnapshotInput)
    return tooling_test::fail(SnapshotInput.error().message());
  rcc::tooling::PipelineControls SnapshotControls;
  SnapshotControls.CaptureStageSnapshots = true;
  auto Snapshots = rcc::tooling::runRV64GCPipeline(SnapshotInput->Document, *Target, Invocation,
                                                   SnapshotControls);
  if (!Snapshots || Snapshots->Snapshots.size() != 7 ||
      Snapshots->Snapshots.front().Stage != rcc::tooling::BackendStage::PreLegalizerMIR ||
      Snapshots->Snapshots.back().Stage != rcc::tooling::BackendStage::FinalMIR) {
    return tooling_test::fail("pipeline must capture every stable MIR stage boundary");
  }
  for (const rcc::tooling::PipelineSnapshot &Snapshot : Snapshots->Snapshots) {
    if (Snapshot.Phase != rcc::tooling::SnapshotPhase::Stage || Snapshot.MIR.empty())
      return tooling_test::fail("stage snapshots must contain complete canonical MIR");
  }

  auto PrintInput =
      rcc::tooling::parseAndCanonicalizeMIR("print-points.mir", *Source, *Target, Invocation);
  if (!PrintInput)
    return tooling_test::fail(PrintInput.error().message());
  rcc::tooling::PipelineControls PrintControls;
  PrintControls.PrintBefore = {"Legalizer"};
  PrintControls.PrintAfter = {"Legalizer#1"};
  auto PrintPoints =
      rcc::tooling::runRV64GCPipeline(PrintInput->Document, *Target, Invocation, PrintControls);
  if (!PrintPoints || PrintPoints->Snapshots.size() != 2 ||
      PrintPoints->Snapshots[0].Phase != rcc::tooling::SnapshotPhase::BeforePass ||
      PrintPoints->Snapshots[1].Phase != rcc::tooling::SnapshotPhase::AfterPass) {
    return tooling_test::fail("pass selectors must capture deterministic before/after snapshots");
  }

  auto StopInput =
      rcc::tooling::parseAndCanonicalizeMIR("stop-after.mir", *Source, *Target, Invocation);
  if (!StopInput)
    return tooling_test::fail(StopInput.error().message());
  rcc::tooling::PipelineControls StopControls;
  StopControls.StopAfter = "Legalizer#1";
  auto Stopped =
      rcc::tooling::runRV64GCPipeline(StopInput->Document, *Target, Invocation, StopControls);
  const rcc::MachineFunction *StoppedFunction = StopInput->Document.unit().functionAt(0);
  if (!Stopped || StoppedFunction == nullptr ||
      rcc::tooling::stageForFunction(*StoppedFunction) !=
          rcc::tooling::BackendStage::LegalizedMIR ||
      Stopped->ExecutedPasses.size() != 1) {
    return tooling_test::fail("stop-after must stop at the selected pass occurrence");
  }
  StopControls.StopAfter = "Legalizer#0";
  if (rcc::tooling::runRV64GCPipeline(StopInput->Document, *Target, Invocation, StopControls))
    return tooling_test::fail("pipeline controls must reject occurrence zero");

  rcc::tooling::BackendInvocation MixedPipeline = Invocation;
  MixedPipeline.PassPipeline = {"default", "Legalizer"};
  auto MixedInput =
      rcc::tooling::parseAndCanonicalizeMIR("mixed-pipeline.mir", *Source, *Target, MixedPipeline);
  if (!MixedInput)
    return tooling_test::fail(MixedInput.error().message());
  if (rcc::tooling::runRV64GCPipeline(MixedInput->Document, *Target, MixedPipeline))
    return tooling_test::fail("named passes must not be mixed with the default pipeline marker");

  rcc::tooling::BackendInvocation NoPassInvocation = Invocation;
  NoPassInvocation.PassPipeline = {"none"};
  auto NoPassInput =
      rcc::tooling::parseAndCanonicalizeMIR("no-pass.mir", *Source, *Target, NoPassInvocation);
  if (!NoPassInput)
    return tooling_test::fail(NoPassInput.error().message());
  const std::string NoPassExpected = NoPassInput->Text;
  auto NoPassRun = rcc::tooling::runBackend(std::move(*NoPassInput), *Target, NoPassInvocation,
                                            rcc::tooling::BackendOutputKind::FinalMIR);
  if (!NoPassRun || !NoPassRun->Pipeline.ExecutedPasses.empty() ||
      NoPassRun->text() != NoPassExpected) {
    return tooling_test::fail("the LLVM-compatible none pass must preserve canonical MIR");
  }

  if (!SelectedMIR)
    return tooling_test::fail("selected MIR stage was not captured");
  auto LaterInput =
      rcc::tooling::parseAndCanonicalizeMIR("later-stage.mir", *SelectedMIR, *Target, Invocation);
  if (!LaterInput)
    return tooling_test::fail(LaterInput.error().message());
  if (rcc::tooling::runBackend(std::move(*LaterInput), *Target, Invocation,
                               rcc::tooling::BackendOutputKind::PreLegalizerMIR)) {
    return tooling_test::fail(
        "backend must not reconstruct an earlier stage from a later snapshot");
  }

  auto RegBankRun = runInput("action-regbank.mir", *Source, *Target, Invocation,
                             rcc::tooling::BackendOutputKind::RegisterBankMIR);
  if (!RegBankRun)
    return tooling_test::fail(RegBankRun.error().message());
  rcc::tooling::BackendInvocation InstructionSelectInvocation = Invocation;
  InstructionSelectInvocation.PassPipeline = {"InstructionSelect"};
  InstructionSelectInvocation.ActionFuel = 0;
  auto RejectedInput = rcc::tooling::parseAndCanonicalizeMIR(
      "action-rejected.mir", std::string(RegBankRun->text()), *Target, InstructionSelectInvocation);
  if (!RejectedInput)
    return tooling_test::fail(RejectedInput.error().message());
  const std::string BeforeRejected = RejectedInput->Text;
  if (rcc::tooling::runRV64GCPipeline(RejectedInput->Document, *Target,
                                      InstructionSelectInvocation)) {
    return tooling_test::fail("zero Action fuel must reject required instruction selection");
  }
  auto AfterRejected = printDocument(RejectedInput->Document, *Target, InstructionSelectInvocation);
  if (!AfterRejected || *AfterRejected != BeforeRejected)
    return tooling_test::fail("Action rejection must not leave a partially modified MachineIR");

  InstructionSelectInvocation.ActionFuel.reset();
  auto AcceptedFirst =
      runInput("action-accepted-1.mir", std::string(RegBankRun->text()), *Target,
               InstructionSelectInvocation, rcc::tooling::BackendOutputKind::SelectedMIR);
  auto AcceptedSecond =
      runInput("action-accepted-2.mir", std::string(RegBankRun->text()), *Target,
               InstructionSelectInvocation, rcc::tooling::BackendOutputKind::SelectedMIR);
  if (!AcceptedFirst || !AcceptedSecond || AcceptedFirst->Actions.empty() ||
      AcceptedFirst->Actions != AcceptedSecond->Actions ||
      AcceptedFirst->Output != AcceptedSecond->Output) {
    return tooling_test::fail("Action replay and stable IDs must be deterministic");
  }
  for (const rcc::MachineActionEvent &Action : AcceptedFirst->Actions) {
    if (!Action.Accepted || Action.ID.Function.value() == 0 || Action.ID.Instruction.value() == 0)
      return tooling_test::fail("instruction selection Actions must use stable nonzero IDs");
  }

  auto MultiFunction = makeMultiFunctionInput();
  if (!MultiFunction)
    return tooling_test::fail(MultiFunction.error().message());
  auto OneThread = runDeterminismCase(*MultiFunction, *Target, 1);
  auto TwoThreads = runDeterminismCase(*MultiFunction, *Target, 2);
  auto FourThreads = runDeterminismCase(*MultiFunction, *Target, 4);
  if (!OneThread || !TwoThreads || !FourThreads)
    return tooling_test::fail("parallel determinism pipeline failed");
  if (OneThread->FinalMIR != TwoThreads->FinalMIR || OneThread->FinalMIR != FourThreads->FinalMIR ||
      OneThread->Assembly != TwoThreads->Assembly || OneThread->Assembly != FourThreads->Assembly ||
      OneThread->Object != TwoThreads->Object || OneThread->Object != FourThreads->Object ||
      OneThread->Actions != TwoThreads->Actions || OneThread->Actions != FourThreads->Actions ||
      OneThread->ExecutedPasses != TwoThreads->ExecutedPasses ||
      OneThread->ExecutedPasses != FourThreads->ExecutedPasses) {
    return tooling_test::fail("1/2/4-thread execution must produce stable output and event order");
  }

  return 0;
}
