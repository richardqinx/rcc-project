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

struct BackendRunResult final {
  ArtifactKind Kind = ArtifactKind::FinalMIR;
  std::vector<std::uint8_t> Output;
  PipelineRunResult Pipeline;
  std::vector<MachineActionEvent> Actions;

  [[nodiscard]] std::string_view text() const noexcept;
};

[[nodiscard]] Expected<BackendRunResult>
runBackend(CanonicalMIR Input, const TargetDescription &Target, const BackendInvocation &Invocation,
           ArtifactKind Output, PipelineControls Controls = {},
           MachinePassInstrumentation *Instrumentation = nullptr);

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_BACKENDDRIVER_H
