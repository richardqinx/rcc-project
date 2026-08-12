// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_BACKENDDRIVER_H
#define RCC_TOOLING_BACKENDDRIVER_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/Support/Error.h"
#include "rcc/Tooling/ArtifactStore.h"
#include "rcc/Tooling/PipelineDriver.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rcc {

class TargetDescription;

namespace tooling {

enum class BackendOutputKind : std::uint8_t {
  PreLegalizerMIR,
  LegalizedMIR,
  RegisterBankMIR,
  SelectedMIR,
  PostSSAMIR,
  PostRegisterAllocationMIR,
  FinalMIR,
  Assembly,
  RelocatableObject,
};

struct BackendRunResult final {
  BackendOutputKind Kind = BackendOutputKind::FinalMIR;
  std::vector<std::uint8_t> Output;
  PipelineRunResult Pipeline;
  std::vector<MachineActionEvent> Actions;

  [[nodiscard]] std::string_view text() const noexcept;
};

[[nodiscard]] Expected<BackendRunResult>
runBackend(CanonicalMIR Input, const TargetDescription &Target, const BackendInvocation &Invocation,
           BackendOutputKind Output, PipelineControls Controls = {},
           MachinePassInstrumentation *Instrumentation = nullptr);

[[nodiscard]] std::string_view spelling(BackendOutputKind Output) noexcept;
[[nodiscard]] ArtifactKind manifestArtifactKind(BackendOutputKind Output) noexcept;
[[nodiscard]] Expected<ArtifactKind> cacheArtifactKind(BackendOutputKind Output);

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_BACKENDDRIVER_H
