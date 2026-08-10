// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/ArtifactStore.h"

#include "ToolingTestSupport.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

[[nodiscard]] std::vector<std::uint8_t> bytes(std::string_view Text) {
  const auto *Begin = reinterpret_cast<const std::uint8_t *>(Text.data());
  return std::vector<std::uint8_t>(Begin, Begin + Text.size());
}

[[nodiscard]] std::filesystem::path cacheEntry(const std::filesystem::path &Root,
                                               std::string_view Key) {
  return Root / std::string(Key.substr(0, 2)) / std::string(Key);
}

[[nodiscard]] bool isCacheMiss(const rcc::Expected<rcc::tooling::CacheEntry> &Result) {
  return !Result && Result.error().code() == rcc::ErrorCode::CacheMiss;
}

} // namespace

int main() {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return tooling_test::fail(Target.error().message());
  const rcc::tooling::BackendInvocation Invocation = rcc::tooling::defaultRV64GCInvocation();
  auto Input = tooling_test::loadCanonicalInput("RISCVGenericPipeline.mir", *Target, Invocation);
  if (!Input)
    return tooling_test::fail(Input.error().message());
  auto Temporary = tooling_test::TemporaryDirectory::create("rcc-artifact-store-test");
  if (!Temporary)
    return tooling_test::fail(Temporary.error().message());

  const std::vector<std::uint8_t> Assembly = bytes(".text\nret\n");
  auto CacheManifest = rcc::tooling::createBackendManifest(
      rcc::tooling::ManifestKind::Cache, rcc::tooling::ArtifactKind::Assembly, Invocation, *Target,
      Input->Text, std::span<const std::uint8_t>(Assembly));
  if (!CacheManifest)
    return tooling_test::fail(CacheManifest.error().message());
  const std::filesystem::path CacheRoot = Temporary->path() / "cache";
  auto Stored = rcc::tooling::storeCacheEntry(CacheRoot, *CacheManifest, Input->Text, Assembly);
  if (!Stored)
    return tooling_test::fail(Stored.error().message());
  auto Loaded = rcc::tooling::loadCacheEntry(CacheRoot, rcc::tooling::ArtifactKind::Assembly,
                                             Invocation, *Target, Input->Text);
  if (!Loaded || Loaded->Manifest != *CacheManifest || Loaded->InputMIR != Input->Text ||
      Loaded->Artifact != Assembly || Loaded->ParsedArtifact) {
    return tooling_test::fail("cache hit must reload and verify every stored artifact fact");
  }
  if (rcc::tooling::storeCacheEntry(CacheRoot, *CacheManifest, Input->Text, Assembly))
    return tooling_test::fail("cache publication must not overwrite an existing entry");
  auto WrongKind = rcc::tooling::loadCacheEntry(CacheRoot, rcc::tooling::ArtifactKind::FinalMIR,
                                                Invocation, *Target, Input->Text);
  if (!isCacheMiss(WrongKind))
    return tooling_test::fail("cache lookup must include the requested artifact kind");
  auto NonBoundary = rcc::tooling::loadCacheEntry(
      CacheRoot, rcc::tooling::ArtifactKind::LegalizedMIR, Invocation, *Target, Input->Text);
  if (!isCacheMiss(NonBoundary))
    return tooling_test::fail("cache lookup must reject non-boundary MIR stages");
  rcc::tooling::BackendInvocation OtherInvocation = Invocation;
  OtherInvocation.Determinism.ThreadCount = 2;
  auto OtherInvocationResult = rcc::tooling::loadCacheEntry(
      CacheRoot, rcc::tooling::ArtifactKind::Assembly, OtherInvocation, *Target, Input->Text);
  if (!isCacheMiss(OtherInvocationResult))
    return tooling_test::fail("cache lookup must include deterministic invocation facts");

  const std::vector<std::uint8_t> MIRBytes = bytes(Input->Text);
  auto MIRManifest = rcc::tooling::createBackendManifest(
      rcc::tooling::ManifestKind::Cache, rcc::tooling::ArtifactKind::PreLegalizerMIR, Invocation,
      *Target, Input->Text, std::span<const std::uint8_t>(MIRBytes));
  if (!MIRManifest)
    return tooling_test::fail(MIRManifest.error().message());
  const std::filesystem::path MIRCacheRoot = Temporary->path() / "mir-cache";
  Stored = rcc::tooling::storeCacheEntry(MIRCacheRoot, *MIRManifest, Input->Text, MIRBytes);
  if (!Stored)
    return tooling_test::fail(Stored.error().message());
  auto LoadedMIR = rcc::tooling::loadCacheEntry(
      MIRCacheRoot, rcc::tooling::ArtifactKind::PreLegalizerMIR, Invocation, *Target, Input->Text);
  if (!LoadedMIR || !LoadedMIR->ParsedArtifact || LoadedMIR->ParsedArtifact->Text != Input->Text) {
    return tooling_test::fail("cached MIR artifacts must be parsed and canonicalized on reload");
  }

  const auto StoreCorruptible = [&](std::string_view Name) -> rcc::Expected<std::filesystem::path> {
    const std::filesystem::path Root = Temporary->path() / Name;
    auto Result = rcc::tooling::storeCacheEntry(Root, *CacheManifest, Input->Text, Assembly);
    if (!Result)
      return std::unexpected(Result.error());
    return cacheEntry(Root, CacheManifest->CacheKey);
  };

  auto CorruptArtifactPath = StoreCorruptible("corrupt-artifact-cache");
  if (!CorruptArtifactPath)
    return tooling_test::fail(CorruptArtifactPath.error().message());
  auto Written = tooling_test::writeText(*CorruptArtifactPath / "artifact.s", "corrupt");
  if (!Written)
    return tooling_test::fail(Written.error().message());
  auto CorruptArtifact = rcc::tooling::loadCacheEntry(Temporary->path() / "corrupt-artifact-cache",
                                                      rcc::tooling::ArtifactKind::Assembly,
                                                      Invocation, *Target, Input->Text);
  if (!isCacheMiss(CorruptArtifact))
    return tooling_test::fail("cache reload must reject a corrupted artifact");

  auto CorruptInputPath = StoreCorruptible("corrupt-input-cache");
  if (!CorruptInputPath)
    return tooling_test::fail(CorruptInputPath.error().message());
  Written = tooling_test::writeText(*CorruptInputPath / "input.mir", "corrupt");
  if (!Written)
    return tooling_test::fail(Written.error().message());
  auto CorruptInput = rcc::tooling::loadCacheEntry(Temporary->path() / "corrupt-input-cache",
                                                   rcc::tooling::ArtifactKind::Assembly, Invocation,
                                                   *Target, Input->Text);
  if (!isCacheMiss(CorruptInput))
    return tooling_test::fail("cache reload must reject a corrupted canonical input");

  auto CorruptManifestPath = StoreCorruptible("corrupt-manifest-cache");
  if (!CorruptManifestPath)
    return tooling_test::fail(CorruptManifestPath.error().message());
  Written = tooling_test::writeText(*CorruptManifestPath / "manifest.yaml", "---\ninvalid: true\n");
  if (!Written)
    return tooling_test::fail(Written.error().message());
  auto CorruptManifest = rcc::tooling::loadCacheEntry(Temporary->path() / "corrupt-manifest-cache",
                                                      rcc::tooling::ArtifactKind::Assembly,
                                                      Invocation, *Target, Input->Text);
  if (!isCacheMiss(CorruptManifest))
    return tooling_test::fail("cache reload must reject a corrupted manifest");

  std::vector<std::uint8_t> WrongArtifact = Assembly;
  WrongArtifact.push_back(0);
  if (rcc::tooling::storeCacheEntry(Temporary->path() / "wrong-artifact-cache", *CacheManifest,
                                    Input->Text, WrongArtifact)) {
    return tooling_test::fail("cache store must reject bytes that disagree with the manifest");
  }

  auto ReproducerManifest = rcc::tooling::createBackendManifest(
      rcc::tooling::ManifestKind::Reproducer, rcc::tooling::ArtifactKind::Assembly, Invocation,
      *Target, Input->Text);
  if (!ReproducerManifest)
    return tooling_test::fail(ReproducerManifest.error().message());
  const std::filesystem::path ReproducerPath = Temporary->path() / "reproducer";
  auto ReproducerWritten =
      rcc::tooling::writeReproducer(ReproducerPath, *ReproducerManifest, Input->Text);
  if (!ReproducerWritten)
    return tooling_test::fail(ReproducerWritten.error().message());
  auto Reproducer = rcc::tooling::loadReproducer(ReproducerPath, *Target);
  if (!Reproducer || Reproducer->Manifest != *ReproducerManifest ||
      Reproducer->Input.Text != Input->Text) {
    return tooling_test::fail("reproducer reload must verify manifest and canonical MIR identity");
  }
  if (rcc::tooling::writeReproducer(ReproducerPath, *ReproducerManifest, Input->Text))
    return tooling_test::fail("reproducer publication must not overwrite an existing package");

  rcc::tooling::StructuredDiagnostic Diagnostic;
  Diagnostic.Category = "verification error";
  Diagnostic.Stage = "regbank-mir";
  Diagnostic.Pass = "InstructionSelect";
  Diagnostic.Function = "generic_pipeline";
  Diagnostic.Rule = "machine-pass";
  Diagnostic.Message = "required selection Action was rejected";
  auto Updated = rcc::tooling::updateReproducerDiagnostics(ReproducerPath, {Diagnostic});
  if (!Updated)
    return tooling_test::fail(Updated.error().message());
  Reproducer = rcc::tooling::loadReproducer(ReproducerPath, *Target);
  if (!Reproducer || Reproducer->Manifest.Diagnostics !=
                         std::vector<rcc::tooling::StructuredDiagnostic>({Diagnostic})) {
    return tooling_test::fail("reproducer diagnostic update must round-trip atomically");
  }
  for (const std::filesystem::directory_entry &Entry :
       std::filesystem::directory_iterator(ReproducerPath)) {
    if (Entry.path().filename().string().contains(".tmp."))
      return tooling_test::fail("reproducer diagnostic update must not leave temporary files");
  }

  Written = tooling_test::writeText(ReproducerPath / "input.mir", "corrupt");
  if (!Written)
    return tooling_test::fail(Written.error().message());
  if (rcc::tooling::loadReproducer(ReproducerPath, *Target))
    return tooling_test::fail("reproducer reload must reject a modified input");
  if (rcc::tooling::updateReproducerDiagnostics(ReproducerPath, {Diagnostic}))
    return tooling_test::fail("diagnostic update must reject a modified reproducer input");

  return 0;
}
