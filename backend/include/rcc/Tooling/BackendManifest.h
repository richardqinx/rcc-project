// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_BACKENDMANIFEST_H
#define RCC_TOOLING_BACKENDMANIFEST_H

#include "rcc/Tooling/BackendInvocation.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rcc::tooling {

inline constexpr std::uint32_t BackendManifestSchemaVersion = 1;

enum class ManifestKind : std::uint8_t {
  Cache,
  Reproducer,
};

enum class ArtifactKind : std::uint8_t {
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

struct StructuredDiagnostic final {
  std::string Category;
  std::string Stage;
  std::string Pass;
  std::string Function;
  std::optional<std::uint64_t> Block;
  std::optional<std::uint64_t> Instruction;
  std::optional<std::uint32_t> Operand;
  std::string SourceLocation;
  std::string TargetConstraint;
  std::string Rule;
  std::string Message;

  friend bool operator==(const StructuredDiagnostic &, const StructuredDiagnostic &) = default;
};

struct BackendManifest final {
  std::uint32_t SchemaVersion = BackendManifestSchemaVersion;
  ManifestKind Kind = ManifestKind::Reproducer;
  ArtifactKind Artifact = ArtifactKind::FinalMIR;
  BackendInvocation Invocation;
  std::string InputMIRHash;
  std::string TargetFingerprint;
  std::string CacheKey;
  std::optional<std::string> ArtifactHash;
  std::vector<StructuredDiagnostic> Diagnostics;

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const BackendManifest &, const BackendManifest &) = default;
};

[[nodiscard]] Expected<std::string> printBackendManifest(const BackendManifest &Manifest);
[[nodiscard]] Expected<BackendManifest> parseBackendManifest(std::string SourceName,
                                                             std::string SourceText);
[[nodiscard]] std::string_view spelling(ManifestKind Value) noexcept;
[[nodiscard]] std::string_view spelling(ArtifactKind Value) noexcept;

} // namespace rcc::tooling

#endif // RCC_TOOLING_BACKENDMANIFEST_H
