// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_PIPELINEDRIVER_H
#define RCC_TOOLING_PIPELINEDRIVER_H

#include "rcc/CodeGen/MIRParser/MIRParser.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/Target/TargetMachine.h"
#include "rcc/Tooling/BackendInvocation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

namespace tooling {

enum class BackendStage : std::uint8_t {
  PreLegalizerMIR,
  LegalizedMIR,
  RegisterBankMIR,
  SelectedMIR,
  PostSSAMIR,
  PostRegisterAllocationMIR,
  FinalMIR,
};

enum class SnapshotPhase : std::uint8_t {
  Stage,
  BeforePass,
  AfterPass,
};

struct PipelineControls final {
  std::optional<std::string> StartBefore;
  std::optional<std::string> StartAfter;
  std::optional<std::string> StopBefore;
  std::optional<std::string> StopAfter;
  std::vector<std::string> PrintBefore;
  std::vector<std::string> PrintAfter;
  std::optional<BackendStage> StopAtStage;
  bool CaptureStageSnapshots = false;
  bool VerifyEach = true;
  bool DetectUnreportedChanges = true;
  bool SimplifyMIR = false;
  bool DebugifyAndStripAllSafe = false;
};

struct PipelineSnapshot final {
  SnapshotPhase Phase = SnapshotPhase::Stage;
  BackendStage Stage = BackendStage::PreLegalizerMIR;
  std::string Function;
  std::string Pass;
  std::uint32_t Occurrence = 0;
  std::string MIR;
};

struct ExecutedPass final {
  std::string Function;
  std::string Pass;
  std::uint32_t Occurrence = 0;

  friend bool operator==(const ExecutedPass &, const ExecutedPass &) = default;
};

struct PipelineRunResult final {
  std::vector<ExecutedPass> ExecutedPasses;
  std::vector<PipelineSnapshot> Snapshots;
};

[[nodiscard]] Expected<PipelineRunResult>
runTargetPipeline(mir::MIRDocument &Document, const TargetMachine &TargetMachine,
                  const BackendInvocation &Invocation, PipelineControls Controls = {},
                  MachinePassInstrumentation *Instrumentation = nullptr);
[[nodiscard]] BackendStage stageForFunction(const MachineFunction &Function) noexcept;
[[nodiscard]] bool isEarlierStage(BackendStage Left, BackendStage Right) noexcept;
[[nodiscard]] std::string_view spelling(BackendStage Stage) noexcept;
[[nodiscard]] Expected<BackendStage> parseBackendStage(std::string_view Value);

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_PIPELINEDRIVER_H
