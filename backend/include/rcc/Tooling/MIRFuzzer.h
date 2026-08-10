// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_MIRFUZZER_H
#define RCC_TOOLING_MIRFUZZER_H

#include "rcc/Support/Error.h"
#include "rcc/Tooling/BackendInvocation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class TargetDescription;

namespace tooling {

enum class MIRFuzzLayer : std::uint8_t {
  Parser,
  BuilderStructural,
  PropertyStage,
  Mutation,
  Pipeline,
  RestrictedSemantics,
};

struct MIRFuzzOptions final {
  std::uint64_t Seed = 0;
  std::size_t Iterations = 100;
  std::size_t MaximumInputSize = 1U << 20U;
  std::vector<MIRFuzzLayer> Layers;
};

struct MIRFuzzLayerResult final {
  MIRFuzzLayer Layer = MIRFuzzLayer::Parser;
  std::size_t Executed = 0;
  std::size_t Accepted = 0;
  std::size_t Rejected = 0;

  friend bool operator==(const MIRFuzzLayerResult &, const MIRFuzzLayerResult &) = default;
};

struct MIRFuzzResult final {
  std::uint64_t Seed = 0;
  std::vector<MIRFuzzLayerResult> Layers;

  [[nodiscard]] std::size_t executed() const noexcept;
  [[nodiscard]] std::size_t accepted() const noexcept;
  [[nodiscard]] std::size_t rejected() const noexcept;
};

[[nodiscard]] Expected<MIRFuzzResult> fuzzMIR(std::string SourceName, std::string SourceText,
                                              BackendInvocation Invocation,
                                              const TargetDescription &Target,
                                              MIRFuzzOptions Options = {});

[[nodiscard]] std::string_view spelling(MIRFuzzLayer Layer) noexcept;
[[nodiscard]] Expected<MIRFuzzLayer> parseMIRFuzzLayer(std::string_view Value);

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_MIRFUZZER_H
