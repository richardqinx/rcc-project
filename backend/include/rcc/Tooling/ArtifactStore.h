// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TOOLING_ARTIFACTSTORE_H
#define RCC_TOOLING_ARTIFACTSTORE_H

#include "rcc/MIR/MIRDocument.h"
#include "rcc/Support/Error.h"
#include "rcc/Tooling/BackendManifest.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rcc {

class TargetDescription;

namespace tooling {

struct CanonicalMIR final {
  mir::MIRDocument Document;
  std::string Text;
};

struct CacheEntry final {
  BackendManifest Manifest;
  std::string InputMIR;
  std::vector<std::uint8_t> Artifact;
  std::optional<CanonicalMIR> ParsedArtifact;
};

struct ReproducerPackage final {
  BackendManifest Manifest;
  CanonicalMIR Input;
};

[[nodiscard]] Expected<CanonicalMIR> parseAndCanonicalizeMIR(std::string SourceName,
                                                             std::string SourceText,
                                                             const TargetDescription &Target,
                                                             const BackendInvocation &Invocation);

[[nodiscard]] Expected<BackendManifest>
createBackendManifest(ManifestKind Kind, ArtifactKind Artifact, const BackendInvocation &Invocation,
                      const TargetDescription &Target, std::string_view CanonicalInputMIR,
                      std::optional<std::span<const std::uint8_t>> ArtifactBytes = std::nullopt,
                      std::vector<StructuredDiagnostic> Diagnostics = {});

[[nodiscard]] Expected<void> storeCacheEntry(const std::filesystem::path &CacheRoot,
                                             const BackendManifest &Manifest,
                                             std::string_view CanonicalInputMIR,
                                             std::span<const std::uint8_t> Artifact);
[[nodiscard]] Expected<CacheEntry> loadCacheEntry(const std::filesystem::path &CacheRoot,
                                                  ArtifactKind Artifact,
                                                  const BackendInvocation &Invocation,
                                                  const TargetDescription &Target,
                                                  std::string_view CanonicalInputMIR);

[[nodiscard]] Expected<void> writeReproducer(const std::filesystem::path &PackagePath,
                                             const BackendManifest &Manifest,
                                             std::string_view CanonicalInputMIR);
[[nodiscard]] Expected<void>
updateReproducerDiagnostics(const std::filesystem::path &PackagePath,
                            std::vector<StructuredDiagnostic> Diagnostics);
[[nodiscard]] Expected<ReproducerPackage> loadReproducer(const std::filesystem::path &PackagePath,
                                                         const TargetDescription &Target);

} // namespace tooling
} // namespace rcc

#endif // RCC_TOOLING_ARTIFACTSTORE_H
