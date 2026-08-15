// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/PipelineDriver.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MIRPrinter.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

struct FunctionRunResult final {
  std::vector<ExecutedPass> ExecutedPasses;
  std::vector<PipelineSnapshot> Snapshots;
  std::optional<Error> Failure;
};

[[nodiscard]] std::uint8_t stageRank(BackendStage Stage) noexcept {
  switch (Stage) {
  case BackendStage::PreLegalizerMIR:
    return 0;
  case BackendStage::LegalizedMIR:
    return 1;
  case BackendStage::RegisterBankMIR:
    return 2;
  case BackendStage::SelectedMIR:
    return 3;
  case BackendStage::PostSSAMIR:
    return 4;
  case BackendStage::PostRegisterAllocationMIR:
    return 5;
  case BackendStage::FinalMIR:
    return 6;
  }
  return 0;
}

[[nodiscard]] bool selectorMatches(std::string_view Selector, std::string_view Pass,
                                   std::uint32_t Occurrence, bool MatchAllOccurrences) {
  if (Selector == "all")
    return true;
  const std::size_t Comma = Selector.find(',');
  const std::string_view Name =
      Comma == std::string_view::npos ? Selector : Selector.substr(0, Comma);
  if (Name != Pass)
    return false;
  if (Comma == std::string_view::npos)
    return MatchAllOccurrences || Occurrence == 1U;
  std::uint32_t Instance = 0;
  const std::string_view Text = Selector.substr(Comma + 1U);
  const auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Instance);
  return Parsed.ec == std::errc{} && Parsed.ptr == Text.data() + Text.size() &&
         Occurrence == Instance + 1U;
}

[[nodiscard]] bool anySelectorMatches(std::span<const std::string> Selectors, std::string_view Pass,
                                      std::uint32_t Occurrence) {
  return std::ranges::any_of(Selectors, [Pass, Occurrence](const std::string &Selector) {
    return selectorMatches(Selector, Pass, Occurrence, true);
  });
}

[[nodiscard]] std::optional<BackendStage> stageAfterPass(std::string_view Pass) noexcept {
  if (Pass == "legalizer")
    return BackendStage::LegalizedMIR;
  if (Pass == "regbankselect")
    return BackendStage::RegisterBankMIR;
  if (Pass == "instruction-select")
    return BackendStage::SelectedMIR;
  if (Pass == "twoaddressinstruction")
    return BackendStage::PostSSAMIR;
  if (Pass == "PBQPAllocator")
    return BackendStage::PostRegisterAllocationMIR;
  if (Pass == "final-encodability-verifier")
    return BackendStage::FinalMIR;
  return std::nullopt;
}

[[nodiscard]] Expected<MachineFunctionPassManager::PassList>
createPasses(const MachineFunction &Function, const BackendInvocation &Invocation,
             const riscv::RISCVSubtarget &Subtarget, MachinePassManagerOptions Options) {
  const auto GlobalISelAbortOption = std::ranges::find(
      Invocation.TargetOptions, std::string_view{"global-isel-abort"}, &BackendOption::Name);
  GlobalISelAbortMode GlobalISelAbort = GlobalISelAbortMode::Enable;
  if (GlobalISelAbortOption != Invocation.TargetOptions.end()) {
    auto Parsed = parseGlobalISelAbortMode(GlobalISelAbortOption->Value);
    if (!Parsed)
      return std::unexpected(Parsed.error());
    GlobalISelAbort = *Parsed;
  }
  MachineFunctionPassManager Pipeline(Options);
  if (Invocation.PassPipeline.size() == 1 && Invocation.PassPipeline.front() == "default") {
    auto Added = riscv::addCanonicalPipeline(
        Pipeline, Function, Subtarget,
        riscv::RISCVPipelineOptions{.Optimization = Invocation.Optimization,
                                    .UseTargetCopyInstructions = true,
                                    .GlobalISelAbort = GlobalISelAbort});
    if (!Added)
      return std::unexpected(Added.error());
  } else {
    if (std::ranges::find(Invocation.PassPipeline, "default") != Invocation.PassPipeline.end()) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "the default pipeline marker cannot be mixed with named passes");
    }
    const auto TargetCopyOption = std::ranges::find(
        Invocation.TargetOptions, std::string_view{"mcp-use-is-copy-instr"}, &BackendOption::Name);
    const bool UseTargetCopyInstructions =
        TargetCopyOption != Invocation.TargetOptions.end() && TargetCopyOption->Value == "true";
    for (const std::string &Name : Invocation.PassPipeline) {
      auto Pass = riscv::createCanonicalPipelinePass(
          Name, Subtarget,
          riscv::RISCVPipelineOptions{.Optimization = Invocation.Optimization,
                                      .UseTargetCopyInstructions = UseTargetCopyInstructions,
                                      .GlobalISelAbort = GlobalISelAbort});
      if (!Pass)
        return std::unexpected(Pass.error());
      auto Added = Pipeline.addPass(std::move(*Pass));
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  return std::move(Pipeline).takePasses();
}

[[nodiscard]] Expected<void> validateSelector(std::string_view Selector, bool AllowAll,
                                              const riscv::RISCVSubtarget &Subtarget) {
  if (Selector == "all" && AllowAll)
    return {};
  const std::size_t Comma = Selector.find(',');
  const std::string_view Name =
      Comma == std::string_view::npos ? Selector : Selector.substr(0, Comma);
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidInvocation, "empty pipeline pass selector");
  auto Pass = riscv::createCanonicalPipelinePass(Name, Subtarget);
  if (!Pass)
    return std::unexpected(Pass.error());
  if (Comma != std::string_view::npos) {
    const std::string_view InstanceText = Selector.substr(Comma + 1U);
    std::uint32_t Instance = 0;
    const auto Parsed =
        std::from_chars(InstanceText.data(), InstanceText.data() + InstanceText.size(), Instance);
    if (InstanceText.empty() || Parsed.ec != std::errc{} ||
        Parsed.ptr != InstanceText.data() + InstanceText.size()) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "pipeline pass instance must be a nonnegative integer");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> validateControls(const PipelineControls &Controls,
                                              riscv::RISCVSubtargetFeatures Features) {
  const riscv::RISCVSubtarget Subtarget(Features);
  if (Controls.StartBefore && Controls.StartAfter) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "-start-before and -start-after cannot be combined");
  }
  if (Controls.StopBefore && Controls.StopAfter) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "-stop-before and -stop-after cannot be combined");
  }
  if (Controls.StartBefore) {
    auto Result = validateSelector(*Controls.StartBefore, false, Subtarget);
    if (!Result)
      return Result;
  }
  if (Controls.StartAfter) {
    auto Result = validateSelector(*Controls.StartAfter, false, Subtarget);
    if (!Result)
      return Result;
  }
  if (Controls.StopBefore) {
    auto Result = validateSelector(*Controls.StopBefore, false, Subtarget);
    if (!Result)
      return Result;
  }
  if (Controls.StopAfter) {
    auto Result = validateSelector(*Controls.StopAfter, false, Subtarget);
    if (!Result)
      return Result;
  }
  for (const std::string &Selector : Controls.PrintBefore) {
    auto Result = validateSelector(Selector, true, Subtarget);
    if (!Result)
      return Result;
  }
  for (const std::string &Selector : Controls.PrintAfter) {
    auto Result = validateSelector(Selector, true, Subtarget);
    if (!Result)
      return Result;
  }
  return {};
}

[[nodiscard]] Expected<void>
captureSnapshot(FunctionRunResult &Result, const mir::MIRDocument &Document,
                const TargetDescription &Target, const BackendInvocation &Invocation,
                SnapshotPhase Phase, BackendStage Stage, std::string_view Function,
                std::string_view Pass, std::uint32_t Occurrence) {
  mir::MIRDocumentPrinterOptions PrintOptions;
  PrintOptions.Mode = mir::MIPrintMode::Complete;
  PrintOptions.EmitEmbeddedIR = true;
  PrintOptions.DataLayout = Invocation.DataLayout;
  auto Printed = mir::printMIRDocument(Document, Target, std::move(PrintOptions));
  if (!Printed)
    return std::unexpected(Printed.error());
  Result.Snapshots.push_back({
      .Phase = Phase,
      .Stage = Stage,
      .Function = std::string(Function),
      .Pass = std::string(Pass),
      .Occurrence = Occurrence,
      .MIR = std::move(*Printed),
  });
  return {};
}

[[nodiscard]] Expected<FunctionRunResult>
runFunction(MachineFunction &Function, mir::MIRDocument &Document, const TargetDescription &Target,
            const BackendInvocation &Invocation, const PipelineControls &Controls,
            riscv::RISCVSubtargetFeatures Features, bool AllowSnapshots,
            MachinePassInstrumentation *Instrumentation) {
  FunctionRunResult Result;
  const riscv::RISCVSubtarget Subtarget(Features);
  MachinePassManagerOptions ManagerOptions{
      .VerifyBefore = true,
      .VerifyEach = Controls.VerifyEach,
      .DetectUnreportedChanges = Controls.DetectUnreportedChanges,
  };
  auto Passes = createPasses(Function, Invocation, Subtarget, ManagerOptions);
  if (!Passes)
    return std::unexpected(Passes.error());
  MachineFunctionAnalysisManager Analyses;
  auto Registered = registerMachineOptimizationAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());

  BackendStage CurrentStage = stageForFunction(Function);
  if (AllowSnapshots && Controls.CaptureStageSnapshots) {
    auto Captured = captureSnapshot(Result, Document, Target, Invocation, SnapshotPhase::Stage,
                                    CurrentStage, Function.name(), {}, 0);
    if (!Captured)
      return std::unexpected(Captured.error());
  }
  if (Controls.StopAtStage && !isEarlierStage(CurrentStage, *Controls.StopAtStage)) {
    return Result;
  }

  std::unordered_map<std::string, std::uint32_t> Occurrences;
  bool Started = !Controls.StartBefore && !Controls.StartAfter;
  bool StopMatched = !Controls.StopBefore && !Controls.StopAfter;
  for (std::unique_ptr<MachineFunctionPass> &Pass : *Passes) {
    if (Pass == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "pipeline contains a null pass");
    const std::string Name = Pass->contract().Name;
    const std::uint32_t Occurrence = ++Occurrences[Name];
    if (!Started) {
      if (Controls.StartBefore && selectorMatches(*Controls.StartBefore, Name, Occurrence, false)) {
        Started = true;
      } else {
        if (Controls.StartAfter && selectorMatches(*Controls.StartAfter, Name, Occurrence, false))
          Started = true;
        continue;
      }
    }
    if (Controls.StopBefore && selectorMatches(*Controls.StopBefore, Name, Occurrence, false)) {
      StopMatched = true;
      break;
    }
    if (AllowSnapshots && anySelectorMatches(Controls.PrintBefore, Name, Occurrence)) {
      auto Captured =
          captureSnapshot(Result, Document, Target, Invocation, SnapshotPhase::BeforePass,
                          CurrentStage, Function.name(), Name, Occurrence);
      if (!Captured)
        return std::unexpected(Captured.error());
    }

    MachineFunctionPassManager SinglePass(ManagerOptions);
    auto Added = SinglePass.addPass(std::move(Pass));
    if (!Added)
      return std::unexpected(Added.error());
    auto Run = SinglePass.run(Function, Target, Analyses, Instrumentation);
    if (!Run)
      return std::unexpected(Run.error());
    Result.ExecutedPasses.push_back({
        .Function = std::string(Function.name()),
        .Pass = Name,
        .Occurrence = Occurrence,
    });

    if (const std::optional<BackendStage> NewStage = stageAfterPass(Name))
      CurrentStage = *NewStage;
    if (AllowSnapshots && anySelectorMatches(Controls.PrintAfter, Name, Occurrence)) {
      auto Captured =
          captureSnapshot(Result, Document, Target, Invocation, SnapshotPhase::AfterPass,
                          CurrentStage, Function.name(), Name, Occurrence);
      if (!Captured)
        return std::unexpected(Captured.error());
    }
    if (AllowSnapshots && Controls.CaptureStageSnapshots && stageAfterPass(Name)) {
      auto Captured = captureSnapshot(Result, Document, Target, Invocation, SnapshotPhase::Stage,
                                      CurrentStage, Function.name(), Name, Occurrence);
      if (!Captured)
        return std::unexpected(Captured.error());
    }
    if (Controls.StopAfter && selectorMatches(*Controls.StopAfter, Name, Occurrence, false)) {
      StopMatched = true;
      break;
    }
    if (Controls.StopAtStage && !isEarlierStage(CurrentStage, *Controls.StopAtStage)) {
      break;
    }
  }
  if (!Started) {
    const std::string Selector = Controls.StartBefore.value_or(Controls.StartAfter.value_or(""));
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "pipeline start selector did not match: '" + Selector + "'");
  }
  if (!StopMatched) {
    const std::string Selector = Controls.StopBefore.value_or(Controls.StopAfter.value_or(""));
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "pipeline stop selector did not match: '" + Selector + "'");
  }
  return Result;
}

[[nodiscard]] bool requiresSequentialExecution(const PipelineControls &Controls) noexcept {
  return Controls.CaptureStageSnapshots || !Controls.PrintBefore.empty() ||
         !Controls.PrintAfter.empty();
}

} // namespace

bool isEarlierStage(BackendStage Left, BackendStage Right) noexcept {
  return stageRank(Left) < stageRank(Right);
}

BackendStage stageForFunction(const MachineFunction &Function) noexcept {
  const MachineFunctionProperties &Properties = Function.properties();
  if (Properties.has(MachineFunctionProperty::Selected) &&
      Properties.has(MachineFunctionProperty::NoExecutablePseudos))
    return BackendStage::FinalMIR;
  if (Properties.has(MachineFunctionProperty::Selected) &&
      Properties.has(MachineFunctionProperty::NoVRegs))
    return BackendStage::PostRegisterAllocationMIR;
  if (Properties.has(MachineFunctionProperty::Selected) &&
      Properties.has(MachineFunctionProperty::TiedOpsRewritten)) {
    return BackendStage::PostSSAMIR;
  }
  if (Properties.has(MachineFunctionProperty::Selected))
    return BackendStage::SelectedMIR;
  if (Properties.has(MachineFunctionProperty::RegBankSelected))
    return BackendStage::RegisterBankMIR;
  if (Properties.has(MachineFunctionProperty::Legalized))
    return BackendStage::LegalizedMIR;
  return BackendStage::PreLegalizerMIR;
}

std::string_view spelling(BackendStage Stage) noexcept {
  switch (Stage) {
  case BackendStage::PreLegalizerMIR:
    return "prelegalizer-mir";
  case BackendStage::LegalizedMIR:
    return "legalized-mir";
  case BackendStage::RegisterBankMIR:
    return "regbank-mir";
  case BackendStage::SelectedMIR:
    return "selected-mir";
  case BackendStage::PostSSAMIR:
    return "post-ssa-mir";
  case BackendStage::PostRegisterAllocationMIR:
    return "post-ra-mir";
  case BackendStage::FinalMIR:
    return "final-mir";
  }
  return "invalid";
}

Expected<BackendStage> parseBackendStage(std::string_view Value) {
  if (Value == "prelegalizer-mir")
    return BackendStage::PreLegalizerMIR;
  if (Value == "legalized-mir")
    return BackendStage::LegalizedMIR;
  if (Value == "regbank-mir")
    return BackendStage::RegisterBankMIR;
  if (Value == "selected-mir")
    return BackendStage::SelectedMIR;
  if (Value == "post-ssa-mir")
    return BackendStage::PostSSAMIR;
  if (Value == "post-ra-mir")
    return BackendStage::PostRegisterAllocationMIR;
  if (Value == "final-mir")
    return BackendStage::FinalMIR;
  return makeUnexpected(ErrorCode::InvalidInvocation, "unknown backend stage");
}

Expected<void> verifyRV64GCInvocation(const BackendInvocation &Invocation) {
  BackendInvocation Canonical = Invocation;
  auto Verified = Canonical.canonicalizeAndVerify();
  if (!Verified)
    return std::unexpected(Verified.error());
  if (Canonical != Invocation) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "RV64GC backend invocation must be canonicalized before execution");
  }
  const BackendInvocation Expected = defaultRV64GCInvocation();
  auto Features = riscv::RISCVSubtargetFeatures::fromFeatureList(Invocation.Features);
  if (!Features)
    return std::unexpected(Features.error());
  if (Invocation.Triple != Expected.Triple || Invocation.CPU != Expected.CPU ||
      Invocation.TuneCPU != Expected.TuneCPU || Invocation.ABI != Expected.ABI ||
      Invocation.DataLayout != Expected.DataLayout) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation does not select the supported RV64 LP64D target");
  }
  if (Invocation.MIRReferenceRevision != LLVMReferenceRevision ||
      Invocation.GenericOpcodeVersion != GenericOpcodeReferenceVersion ||
      Invocation.TargetABILoweringVersion != RV64GCTargetABILoweringVersion) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation reference versions do not match this build");
  }
  return {};
}

Expected<PipelineRunResult> runRV64GCPipeline(mir::MIRDocument &Document,
                                              const TargetDescription &Target,
                                              const BackendInvocation &Invocation,
                                              PipelineControls Controls,
                                              MachinePassInstrumentation *Instrumentation) {
  auto InvocationVerification = verifyRV64GCInvocation(Invocation);
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  auto Features = riscv::RISCVSubtargetFeatures::fromFeatureList(Invocation.Features);
  if (!Features)
    return std::unexpected(Features.error());
  auto ControlVerification = validateControls(Controls, *Features);
  if (!ControlVerification)
    return std::unexpected(ControlVerification.error());
  auto UnitVerification = Document.unit().verify();
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  if (Document.unit().functionCount() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "pipeline input must contain at least one machine function");
  }

  MachinePassInstrumentation LocalInstrumentation;
  MachinePassInstrumentation *ActiveInstrumentation =
      Instrumentation == nullptr ? &LocalInstrumentation : Instrumentation;
  ActiveInstrumentation->setPerFunctionFuel(Invocation.ActionFuel);
  if (!Invocation.EnabledActions.empty()) {
    const std::vector<std::string> Enabled = Invocation.EnabledActions;
    ActiveInstrumentation->setActionFilter([Enabled](const MachineActionID &Action, std::uint64_t) {
      return std::ranges::binary_search(Enabled, Action.Rule) ||
             std::ranges::binary_search(Enabled, Action.Pass) ||
             std::ranges::binary_search(Enabled, Action.toString());
    });
  }

  const std::size_t FunctionCount = Document.unit().functionCount();
  std::vector<FunctionRunResult> FunctionResults(FunctionCount);
  const bool Sequential = requiresSequentialExecution(Controls) ||
                          Invocation.Determinism.ThreadCount == 1 || FunctionCount == 1;
  if (Sequential) {
    for (std::size_t Index = 0; Index < FunctionCount; ++Index) {
      MachineFunction *Function = Document.unit().functionAt(Index);
      if (Function == nullptr)
        return makeUnexpected(ErrorCode::InternalError,
                              "compilation unit contains a null function");
      auto Run = runFunction(*Function, Document, Target, Invocation, Controls, *Features, true,
                             ActiveInstrumentation);
      if (!Run)
        return std::unexpected(Run.error());
      FunctionResults[Index] = std::move(*Run);
    }
  } else {
    const std::size_t WorkerCount =
        std::min<std::size_t>(Invocation.Determinism.ThreadCount, FunctionCount);
    std::atomic<std::size_t> NextFunction = 0;
    const auto Worker = [&]() {
      while (true) {
        const std::size_t Index = NextFunction.fetch_add(1, std::memory_order_relaxed);
        if (Index >= FunctionCount)
          return;
        MachineFunction *Function = Document.unit().functionAt(Index);
        if (Function == nullptr) {
          FunctionResults[Index].Failure =
              Error(ErrorCode::InternalError, "compilation unit contains a null function");
          continue;
        }
        auto Run = runFunction(*Function, Document, Target, Invocation, Controls, *Features, false,
                               ActiveInstrumentation);
        if (!Run) {
          FunctionResults[Index].Failure = Run.error();
        } else {
          FunctionResults[Index] = std::move(*Run);
        }
      }
    };
    std::vector<std::thread> Workers;
    Workers.reserve(WorkerCount);
    for (std::size_t Index = 0; Index < WorkerCount; ++Index)
      Workers.emplace_back(Worker);
    for (std::thread &Thread : Workers)
      Thread.join();
  }

  PipelineRunResult Result;
  for (FunctionRunResult &FunctionResult : FunctionResults) {
    if (FunctionResult.Failure)
      return std::unexpected(std::move(*FunctionResult.Failure));
    Result.ExecutedPasses.insert(Result.ExecutedPasses.end(),
                                 std::make_move_iterator(FunctionResult.ExecutedPasses.begin()),
                                 std::make_move_iterator(FunctionResult.ExecutedPasses.end()));
    Result.Snapshots.insert(Result.Snapshots.end(),
                            std::make_move_iterator(FunctionResult.Snapshots.begin()),
                            std::make_move_iterator(FunctionResult.Snapshots.end()));
  }
  return Result;
}

} // namespace rcc::tooling
