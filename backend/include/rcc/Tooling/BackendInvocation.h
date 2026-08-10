// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_BACKENDINVOCATION_H
#define RCC_TOOLING_BACKENDINVOCATION_H

#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class TargetDescription;

namespace tooling {

inline constexpr std::uint32_t BackendInvocationSchemaVersion = 1;
inline constexpr std::string_view LLVMReferenceRevision =
    "2b40c303e79f64c5c7642032554eda026594d604";
inline constexpr std::string_view GenericOpcodeReferenceVersion = "rcc-generic-opcode-v1";
inline constexpr std::string_view RV64GCTargetABILoweringVersion = "rcc-rv64gc-lp64d-v1";
inline constexpr std::string_view CompilerBuildFingerprint = "rcc-backend-0.1.0-stage17";

enum class RelocationModel : std::uint8_t {
  Static,
  PositionIndependent,
};

enum class CodeModel : std::uint8_t {
  Small,
  Medium,
  Large,
};

enum class OptimizationLevel : std::uint8_t {
  O0,
  O1,
  O2,
  O3,
  Os,
  Oz,
};

enum class LayoutOwnership : std::uint8_t {
  NativeObject,
  CompilerFinalLayout,
  ExternalRelaxation,
};

struct BackendOption final {
  std::string Name;
  std::string Value;

  friend bool operator==(const BackendOption &, const BackendOption &) = default;
};

struct DeterminismOptions final {
  std::uint64_t RandomSeed = 0;
  std::uint32_t ThreadCount = 1;
  bool StableScheduling = true;

  friend bool operator==(const DeterminismOptions &, const DeterminismOptions &) = default;
};

struct BackendInvocation final {
  std::uint32_t SchemaVersion = BackendInvocationSchemaVersion;
  std::string Triple;
  std::string CPU;
  std::string TuneCPU;
  std::vector<std::string> Features;
  std::string ABI;
  std::string DataLayout;
  RelocationModel Relocation = RelocationModel::PositionIndependent;
  CodeModel Code = CodeModel::Small;
  std::vector<BackendOption> TargetOptions;
  LayoutOwnership Layout = LayoutOwnership::NativeObject;
  std::string AssemblerFlavor;
  std::string AssemblerVersion;
  std::vector<BackendOption> AssemblerOptions;
  OptimizationLevel Optimization = OptimizationLevel::O2;
  std::vector<std::string> PassPipeline{"default"};
  DeterminismOptions Determinism;
  std::optional<std::uint64_t> ActionFuel;
  std::vector<std::string> EnabledActions;
  std::string MIRReferenceRevision{LLVMReferenceRevision};
  std::string GenericOpcodeVersion{GenericOpcodeReferenceVersion};
  std::string TargetABILoweringVersion{RV64GCTargetABILoweringVersion};
  std::string BuildFingerprint{CompilerBuildFingerprint};

  [[nodiscard]] Expected<void> canonicalizeAndVerify();
  [[nodiscard]] std::string canonicalText() const;

  friend bool operator==(const BackendInvocation &, const BackendInvocation &) = default;
};

[[nodiscard]] BackendInvocation defaultRV64GCInvocation();
[[nodiscard]] Expected<std::string>
computeTargetDescriptionFingerprint(const TargetDescription &Target);
[[nodiscard]] Expected<std::string> computeCacheKey(std::string_view CanonicalMIR,
                                                    const BackendInvocation &Invocation,
                                                    std::string_view TargetFingerprint);

[[nodiscard]] std::string_view spelling(RelocationModel Value) noexcept;
[[nodiscard]] std::string_view spelling(CodeModel Value) noexcept;
[[nodiscard]] std::string_view spelling(OptimizationLevel Value) noexcept;
[[nodiscard]] std::string_view spelling(LayoutOwnership Value) noexcept;
[[nodiscard]] Expected<RelocationModel> parseRelocationModel(std::string_view Value);
[[nodiscard]] Expected<CodeModel> parseCodeModel(std::string_view Value);
[[nodiscard]] Expected<OptimizationLevel> parseOptimizationLevel(std::string_view Value);
[[nodiscard]] Expected<LayoutOwnership> parseLayoutOwnership(std::string_view Value);

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_BACKENDINVOCATION_H
