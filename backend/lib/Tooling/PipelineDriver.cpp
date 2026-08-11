// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/PipelineDriver.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/MIR/MIRDocument.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"

#include <algorithm>
#include <atomic>
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

[[nodiscard]] unsigned stageOrdinal(BackendStage Stage) noexcept {
  return static_cast<unsigned>(Stage);
}

[[nodiscard]] bool selectorMatches(std::string_view Selector, std::string_view Pass,
                                   std::uint32_t Occurrence) {
  if (Selector == "all" || Selector == Pass)
    return true;
  std::string Qualified(Pass);
  Qualified.push_back('#');
  Qualified.append(std::to_string(Occurrence));
  return Selector == Qualified;
}

[[nodiscard]] bool anySelectorMatches(std::span<const std::string> Selectors, std::string_view Pass,
                                      std::uint32_t Occurrence) {
  return std::ranges::any_of(Selectors, [Pass, Occurrence](const std::string &Selector) {
    return selectorMatches(Selector, Pass, Occurrence);
  });
}

[[nodiscard]] std::optional<BackendStage> stageAfterPass(std::string_view Pass) noexcept {
  if (Pass == "Legalizer")
    return BackendStage::LegalizedMIR;
  if (Pass == "RegisterBankSelect")
    return BackendStage::RegisterBankMIR;
  if (Pass == "InstructionSelect")
    return BackendStage::SelectedMIR;
  if (Pass == "TwoAddressRewrite")
    return BackendStage::PostSSAMIR;
  if (Pass == "PBQPAllocator")
    return BackendStage::PostRegisterAllocationMIR;
  if (Pass == "final-encodability-verifier")
    return BackendStage::FinalMIR;
  return std::nullopt;
}

[[nodiscard]] Expected<MachinePassManager::PassList>
createPasses(const MachineFunction &Function, const BackendInvocation &Invocation,
             MachinePassManagerOptions Options) {
  MachinePassManager Pipeline(Options);
  if (Invocation.PassPipeline.size() == 1 && Invocation.PassPipeline.front() == "default") {
    auto Added = riscv::addCanonicalPipeline(Pipeline, Function);
    if (!Added)
      return std::unexpected(Added.error());
  } else {
    if (std::ranges::find(Invocation.PassPipeline, "default") != Invocation.PassPipeline.end()) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "the default pipeline marker cannot be mixed with named passes");
    }
    for (const std::string &Name : Invocation.PassPipeline) {
      if (Name == "none")
        continue;
      auto Pass = riscv::createCanonicalPipelinePass(Name);
      if (!Pass)
        return std::unexpected(Pass.error());
      auto Added = Pipeline.addPass(std::move(*Pass));
      if (!Added)
        return std::unexpected(Added.error());
    }
  }
  return std::move(Pipeline).takePasses();
}

[[nodiscard]] Expected<void> validateSelector(std::string_view Selector) {
  if (Selector == "all")
    return {};
  const std::size_t Hash = Selector.rfind('#');
  const std::string_view Name =
      Hash == std::string_view::npos ? Selector : Selector.substr(0, Hash);
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidInvocation, "empty pipeline pass selector");
  auto Pass = riscv::createCanonicalPipelinePass(Name);
  if (!Pass)
    return std::unexpected(Pass.error());
  if (Hash != std::string_view::npos) {
    const std::string_view Occurrence = Selector.substr(Hash + 1U);
    if (Occurrence.empty() || Occurrence == "0" ||
        !std::ranges::all_of(Occurrence,
                             [](char Character) { return Character >= '0' && Character <= '9'; })) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "pipeline pass occurrence must be a positive integer");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> validateControls(const PipelineControls &Controls) {
  if (Controls.StopBefore) {
    auto Result = validateSelector(*Controls.StopBefore);
    if (!Result)
      return Result;
  }
  if (Controls.StopAfter) {
    auto Result = validateSelector(*Controls.StopAfter);
    if (!Result)
      return Result;
  }
  for (const std::string &Selector : Controls.PrintBefore) {
    auto Result = validateSelector(Selector);
    if (!Result)
      return Result;
  }
  for (const std::string &Selector : Controls.PrintAfter) {
    auto Result = validateSelector(Selector);
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
  PrintOptions.DefaultPointerSizeInBits = 64;
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
            bool AllowSnapshots, MachinePassInstrumentation *Instrumentation) {
  FunctionRunResult Result;
  MachinePassManagerOptions ManagerOptions{
      .VerifyBefore = true,
      .VerifyEach = Controls.VerifyEach,
      .DetectUnreportedChanges = Controls.DetectUnreportedChanges,
  };
  auto Passes = createPasses(Function, Invocation, ManagerOptions);
  if (!Passes)
    return std::unexpected(Passes.error());
  MachineAnalysisManager Analyses;
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
  if (Controls.StopAtStage && stageOrdinal(CurrentStage) >= stageOrdinal(*Controls.StopAtStage)) {
    return Result;
  }

  std::unordered_map<std::string, std::uint32_t> Occurrences;
  for (std::unique_ptr<MachineFunctionPass> &Pass : *Passes) {
    if (Pass == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "pipeline contains a null pass");
    const std::string Name = Pass->contract().Name;
    const std::uint32_t Occurrence = ++Occurrences[Name];
    if (Controls.StopBefore && selectorMatches(*Controls.StopBefore, Name, Occurrence))
      break;
    if (AllowSnapshots && anySelectorMatches(Controls.PrintBefore, Name, Occurrence)) {
      auto Captured =
          captureSnapshot(Result, Document, Target, Invocation, SnapshotPhase::BeforePass,
                          CurrentStage, Function.name(), Name, Occurrence);
      if (!Captured)
        return std::unexpected(Captured.error());
    }

    MachinePassManager SinglePass(ManagerOptions);
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
    if (Controls.StopAfter && selectorMatches(*Controls.StopAfter, Name, Occurrence))
      break;
    if (Controls.StopAtStage && stageOrdinal(CurrentStage) >= stageOrdinal(*Controls.StopAtStage)) {
      break;
    }
  }
  return Result;
}

[[nodiscard]] bool requiresSequentialExecution(const PipelineControls &Controls) noexcept {
  return Controls.CaptureStageSnapshots || !Controls.PrintBefore.empty() ||
         !Controls.PrintAfter.empty();
}

} // namespace

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
  if (Invocation.Triple != Expected.Triple || Invocation.CPU != Expected.CPU ||
      Invocation.TuneCPU != Expected.TuneCPU || Invocation.Features != Expected.Features ||
      Invocation.ABI != Expected.ABI || Invocation.DataLayout != Expected.DataLayout) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "backend invocation does not select the supported RV64GC LP64D target");
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
  auto ControlVerification = validateControls(Controls);
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
      auto Run = runFunction(*Function, Document, Target, Invocation, Controls, true,
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
        auto Run = runFunction(*Function, Document, Target, Invocation, Controls, false,
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
