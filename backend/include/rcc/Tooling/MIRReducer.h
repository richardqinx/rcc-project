// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_MIRREDUCER_H
#define RCC_TOOLING_MIRREDUCER_H

#include "rcc/Support/Error.h"
#include "rcc/Tooling/BackendInvocation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class TargetDescription;

namespace tooling {

enum class ReductionMode : std::uint8_t {
  PreserveValidity,
  PreserveFailure,
};

enum class ReductionGranularity : std::uint8_t {
  CompilationUnit,
  MachineFunction,
  BasicBlock,
  CFGEdge,
  Instruction,
  Operand,
  RegisterSubgraph,
  FrameObject,
  ConstantPool,
  JumpTable,
  TargetFeature,
  PassPipeline,
};

struct MIRReducerOptions final {
  ReductionMode Mode = ReductionMode::PreserveFailure;
  std::size_t MaximumAttempts = 100'000;
  std::vector<ReductionGranularity> Granularities;
};

using MIRInterestingnessTest =
    std::function<Expected<bool>(std::string_view, const BackendInvocation &)>;

// PreserveFailure requires and evaluates the interestingness test. PreserveValidity accepts every
// smaller candidate that reparses, canonicalizes, and verifies; its interestingness test is
// ignored.

struct MIRReductionStep final {
  ReductionGranularity Granularity = ReductionGranularity::Instruction;
  std::string Description;
  std::size_t PreviousSize = 0;
  std::size_t ReducedSize = 0;
};

struct MIRReductionResult final {
  std::string MIR;
  BackendInvocation Invocation;
  std::vector<MIRReductionStep> Steps;
  std::size_t Attempts = 0;
  bool Exhausted = false;
};

[[nodiscard]] Expected<MIRReductionResult> reduceMIR(std::string SourceName, std::string SourceText,
                                                     BackendInvocation Invocation,
                                                     const TargetDescription &Target,
                                                     const MIRInterestingnessTest &Interesting,
                                                     MIRReducerOptions Options = {});

[[nodiscard]] std::string_view spelling(ReductionGranularity Granularity) noexcept;

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_MIRREDUCER_H
