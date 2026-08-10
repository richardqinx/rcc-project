// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/ArtifactStore.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/MIR/MIRDocument.h"
#include "rcc/Support/SHA256.h"
#include "rcc/Support/TextDiagnostic.h"
#include "rcc/Tooling/BackendInvocation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

constexpr std::uintmax_t MaximumPackageFileSize = std::uintmax_t{1} << 30U;
std::atomic<std::uint64_t> TemporarySequence = 0;

[[nodiscard]] bool isMIRArtifact(ArtifactKind Artifact) noexcept {
  switch (Artifact) {
  case ArtifactKind::PreLegalizerMIR:
  case ArtifactKind::LegalizedMIR:
  case ArtifactKind::RegisterBankMIR:
  case ArtifactKind::SelectedMIR:
  case ArtifactKind::PostSSAMIR:
  case ArtifactKind::PostRegisterAllocationMIR:
  case ArtifactKind::FinalMIR:
    return true;
  case ArtifactKind::Assembly:
  case ArtifactKind::RelocatableObject:
    return false;
  }
  return false;
}

[[nodiscard]] bool isCacheableArtifact(ArtifactKind Artifact) noexcept {
  switch (Artifact) {
  case ArtifactKind::PreLegalizerMIR:
  case ArtifactKind::SelectedMIR:
  case ArtifactKind::PostRegisterAllocationMIR:
  case ArtifactKind::FinalMIR:
  case ArtifactKind::Assembly:
  case ArtifactKind::RelocatableObject:
    return true;
  case ArtifactKind::LegalizedMIR:
  case ArtifactKind::RegisterBankMIR:
  case ArtifactKind::PostSSAMIR:
    return false;
  }
  return false;
}

[[nodiscard]] std::string_view artifactFileName(ArtifactKind Artifact) noexcept {
  switch (Artifact) {
  case ArtifactKind::PreLegalizerMIR:
  case ArtifactKind::LegalizedMIR:
  case ArtifactKind::RegisterBankMIR:
  case ArtifactKind::SelectedMIR:
  case ArtifactKind::PostSSAMIR:
  case ArtifactKind::PostRegisterAllocationMIR:
  case ArtifactKind::FinalMIR:
    return "artifact.mir";
  case ArtifactKind::Assembly:
    return "artifact.s";
  case ArtifactKind::RelocatableObject:
    return "artifact.o";
  }
  return "artifact.bin";
}

[[nodiscard]] std::filesystem::path cacheEntryPath(const std::filesystem::path &Root,
                                                   std::string_view Key) {
  return Root / std::string(Key.substr(0, 2)) / std::string(Key);
}

[[nodiscard]] Expected<std::vector<std::uint8_t>> readBytes(const std::filesystem::path &Path,
                                                            ErrorCode Code) {
  std::error_code FileError;
  const std::uintmax_t Size = std::filesystem::file_size(Path, FileError);
  if (FileError)
    return makeUnexpected(Code, "cannot inspect package file '" + Path.string() + "'");
  if (Size > MaximumPackageFileSize ||
      Size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    return makeUnexpected(Code, "package file exceeds the supported size limit");
  }
  std::ifstream Input(Path, std::ios::binary);
  if (!Input)
    return makeUnexpected(Code, "cannot open package file '" + Path.string() + "'");
  std::vector<std::uint8_t> Result(static_cast<std::size_t>(Size));
  if (!Result.empty()) {
    Input.read(reinterpret_cast<char *>(Result.data()),
               static_cast<std::streamsize>(Result.size()));
  }
  if (!Input)
    return makeUnexpected(Code, "cannot read package file '" + Path.string() + "'");
  return Result;
}

[[nodiscard]] Expected<std::string> readText(const std::filesystem::path &Path, ErrorCode Code) {
  auto Bytes = readBytes(Path, Code);
  if (!Bytes)
    return std::unexpected(Bytes.error());
  return std::string(reinterpret_cast<const char *>(Bytes->data()), Bytes->size());
}

[[nodiscard]] Expected<void> writeBytes(const std::filesystem::path &Path,
                                        std::span<const std::uint8_t> Bytes, ErrorCode Code) {
  std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
  if (!Output)
    return makeUnexpected(Code, "cannot create package file '" + Path.string() + "'");
  if (!Bytes.empty()) {
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
  }
  Output.close();
  if (!Output)
    return makeUnexpected(Code, "cannot write package file '" + Path.string() + "'");
  return {};
}

[[nodiscard]] Expected<void> writeText(const std::filesystem::path &Path, std::string_view Text,
                                       ErrorCode Code) {
  const auto *Bytes = reinterpret_cast<const std::uint8_t *>(Text.data());
  return writeBytes(Path, std::span<const std::uint8_t>(Bytes, Text.size()), Code);
}

[[nodiscard]] std::filesystem::path temporaryPath(const std::filesystem::path &Destination) {
  const std::uint64_t Sequence = TemporarySequence.fetch_add(1, std::memory_order_relaxed);
  return Destination.parent_path() /
         (Destination.filename().string() + ".tmp." + std::to_string(Sequence));
}

[[nodiscard]] Expected<void> replaceTextAtomically(const std::filesystem::path &Path,
                                                   std::string_view Text, ErrorCode Code) {
  const std::filesystem::path Temporary = temporaryPath(Path);
  auto Written = writeText(Temporary, Text, Code);
  if (!Written)
    return Written;
  std::error_code FileError;
  std::filesystem::rename(Temporary, Path, FileError);
  if (!FileError)
    return {};
  std::error_code CleanupError;
  std::filesystem::remove(Temporary, CleanupError);
  return makeUnexpected(Code, "cannot atomically replace package file '" + Path.string() + "'");
}

[[nodiscard]] Expected<void>
writePackageAtomically(const std::filesystem::path &Destination, std::string_view ManifestText,
                       std::string_view InputMIR, std::string_view ArtifactName,
                       std::span<const std::uint8_t> Artifact, bool HasArtifact, ErrorCode Code) {
  if (Destination.empty())
    return makeUnexpected(Code, "package destination cannot be empty");
  std::error_code FileError;
  const std::filesystem::path Parent = Destination.parent_path();
  if (!Parent.empty()) {
    std::filesystem::create_directories(Parent, FileError);
    if (FileError)
      return makeUnexpected(Code, "cannot create package parent directory");
  }
  if (std::filesystem::exists(Destination, FileError))
    return makeUnexpected(Code, "package destination already exists");
  if (FileError)
    return makeUnexpected(Code, "cannot inspect package destination");

  const std::filesystem::path Temporary = temporaryPath(Destination);
  std::filesystem::create_directory(Temporary, FileError);
  if (FileError)
    return makeUnexpected(Code, "cannot create temporary package directory");
  const auto Cleanup = [&]() {
    std::error_code CleanupError;
    std::filesystem::remove_all(Temporary, CleanupError);
  };
  auto Written = writeText(Temporary / "manifest.yaml", ManifestText, Code);
  if (!Written) {
    Cleanup();
    return Written;
  }
  Written = writeText(Temporary / "input.mir", InputMIR, Code);
  if (!Written) {
    Cleanup();
    return Written;
  }
  if (HasArtifact) {
    Written = writeBytes(Temporary / ArtifactName, Artifact, Code);
    if (!Written) {
      Cleanup();
      return Written;
    }
  }
  std::filesystem::rename(Temporary, Destination, FileError);
  if (FileError) {
    Cleanup();
    return makeUnexpected(Code, "cannot publish package atomically");
  }
  return {};
}

[[nodiscard]] Expected<void> verifyFunctions(const mir::MIRDocument &Document,
                                             const TargetDescription &Target) {
  auto UnitVerification = Document.unit().verify();
  if (!UnitVerification)
    return std::unexpected(UnitVerification.error());
  for (std::size_t Index = 0; Index < Document.unit().functionCount(); ++Index) {
    const MachineFunction *Function = Document.unit().functionAt(Index);
    if (Function == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "MIR document contains a null machine function");
    auto FunctionVerification = MachineVerifier::verify(*Function, Target);
    if (!FunctionVerification)
      return std::unexpected(FunctionVerification.error());
  }
  return {};
}

[[nodiscard]] Error parseFailure(const mir::MIRDocumentParseResult &Parsed, ErrorCode Code) {
  if (Parsed.diagnostics().empty())
    return Error(Code, "MIR package input could not be parsed");
  return Error(Code, formatTextDiagnostic(Parsed.diagnostics().front(), Parsed.source()));
}

[[nodiscard]] Expected<void> verifyManifestIdentity(const BackendManifest &Manifest,
                                                    ManifestKind Kind, ArtifactKind Artifact,
                                                    const BackendInvocation &Invocation,
                                                    std::string_view InputHash,
                                                    std::string_view TargetFingerprint,
                                                    std::string_view CacheKey, ErrorCode Code) {
  if (Manifest.Kind != Kind || Manifest.Artifact != Artifact || Manifest.Invocation != Invocation ||
      Manifest.InputMIRHash != InputHash || Manifest.TargetFingerprint != TargetFingerprint ||
      Manifest.CacheKey != CacheKey) {
    return makeUnexpected(Code, "package manifest does not match the requested backend input");
  }
  return {};
}

} // namespace

Expected<CanonicalMIR> parseAndCanonicalizeMIR(std::string SourceName, std::string SourceText,
                                               const TargetDescription &Target,
                                               const BackendInvocation &Invocation) {
  BackendInvocation CanonicalInvocation = Invocation;
  auto InvocationVerification = CanonicalInvocation.canonicalizeAndVerify();
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  if (CanonicalInvocation != Invocation) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "MIR parsing requires a canonical backend invocation");
  }
  mir::MIRDocumentParserOptions ParseOptions;
  ParseOptions.DataLayout = Invocation.DataLayout;
  ParseOptions.DefaultPointerSizeInBits = 64;
  ParseOptions.VerifyMachineFunctions = true;
  mir::MIRDocumentParseResult Parsed =
      mir::parseMIRDocument(Target, std::move(SourceName), std::move(SourceText), ParseOptions);
  if (!Parsed.succeeded())
    return std::unexpected(parseFailure(Parsed, ErrorCode::ParseError));
  if (Parsed.document() == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "successful MIR parse has no document");
  auto Verification = verifyFunctions(*Parsed.document(), Target);
  if (!Verification)
    return std::unexpected(Verification.error());
  mir::MIRDocumentPrinterOptions PrintOptions;
  PrintOptions.Mode = mir::MIPrintMode::Complete;
  PrintOptions.EmitEmbeddedIR = true;
  PrintOptions.DataLayout = Invocation.DataLayout;
  PrintOptions.DefaultPointerSizeInBits = 64;
  auto Printed = mir::printMIRDocument(*Parsed.document(), Target, std::move(PrintOptions));
  if (!Printed)
    return std::unexpected(Printed.error());
  return CanonicalMIR{
      .Document = std::move(*Parsed.document()),
      .Text = std::move(*Printed),
  };
}

Expected<BackendManifest>
createBackendManifest(ManifestKind Kind, ArtifactKind Artifact, const BackendInvocation &Invocation,
                      const TargetDescription &Target, std::string_view CanonicalInputMIR,
                      std::optional<std::span<const std::uint8_t>> ArtifactBytes,
                      std::vector<StructuredDiagnostic> Diagnostics) {
  BackendInvocation CanonicalInvocation = Invocation;
  auto InvocationVerification = CanonicalInvocation.canonicalizeAndVerify();
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  if (CanonicalInvocation != Invocation) {
    return makeUnexpected(ErrorCode::InvalidInvocation,
                          "manifest creation requires a canonical backend invocation");
  }
  auto Fingerprint = computeTargetDescriptionFingerprint(Target);
  if (!Fingerprint)
    return std::unexpected(Fingerprint.error());
  auto Key = computeCacheKey(CanonicalInputMIR, Invocation, *Fingerprint);
  if (!Key)
    return std::unexpected(Key.error());
  BackendManifest Result{
      .SchemaVersion = BackendManifestSchemaVersion,
      .Kind = Kind,
      .Artifact = Artifact,
      .Invocation = Invocation,
      .InputMIRHash = sha256Hex(CanonicalInputMIR),
      .TargetFingerprint = std::move(*Fingerprint),
      .CacheKey = std::move(*Key),
      .ArtifactHash = std::nullopt,
      .Diagnostics = std::move(Diagnostics),
  };
  if (ArtifactBytes) {
    const auto *Bytes = reinterpret_cast<const std::byte *>(ArtifactBytes->data());
    Result.ArtifactHash = sha256Hex(
        std::span<const std::byte>(Bytes, static_cast<std::size_t>(ArtifactBytes->size())));
  }
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<void> storeCacheEntry(const std::filesystem::path &CacheRoot,
                               const BackendManifest &Manifest, std::string_view CanonicalInputMIR,
                               std::span<const std::uint8_t> Artifact) {
  auto Verification = Manifest.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (Manifest.Kind != ManifestKind::Cache || !Manifest.ArtifactHash) {
    return makeUnexpected(ErrorCode::ManifestError,
                          "cache entry requires a cache manifest and artifact hash");
  }
  if (!isCacheableArtifact(Manifest.Artifact)) {
    return makeUnexpected(ErrorCode::ManifestError,
                          "manifest artifact is not an allowed cache boundary");
  }
  if (sha256Hex(CanonicalInputMIR) != Manifest.InputMIRHash) {
    return makeUnexpected(ErrorCode::ManifestError,
                          "cache input MIR does not match its manifest hash");
  }
  const auto *ArtifactBytes = reinterpret_cast<const std::byte *>(Artifact.data());
  if (sha256Hex(std::span<const std::byte>(ArtifactBytes, Artifact.size())) !=
      *Manifest.ArtifactHash) {
    return makeUnexpected(ErrorCode::ManifestError,
                          "cache artifact does not match its manifest hash");
  }
  auto Printed = printBackendManifest(Manifest);
  if (!Printed)
    return std::unexpected(Printed.error());
  const std::filesystem::path Destination = cacheEntryPath(CacheRoot, Manifest.CacheKey);
  return writePackageAtomically(Destination, *Printed, CanonicalInputMIR,
                                artifactFileName(Manifest.Artifact), Artifact, true,
                                ErrorCode::IoError);
}

Expected<CacheEntry> loadCacheEntry(const std::filesystem::path &CacheRoot, ArtifactKind Artifact,
                                    const BackendInvocation &Invocation,
                                    const TargetDescription &Target,
                                    std::string_view CanonicalInputMIR) {
  if (!isCacheableArtifact(Artifact))
    return makeUnexpected(ErrorCode::CacheMiss, "requested artifact is not a cache boundary");
  BackendInvocation CanonicalInvocation = Invocation;
  auto InvocationVerification = CanonicalInvocation.canonicalizeAndVerify();
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  if (CanonicalInvocation != Invocation)
    return makeUnexpected(ErrorCode::CacheMiss, "cache invocation is not canonical");
  auto Fingerprint = computeTargetDescriptionFingerprint(Target);
  if (!Fingerprint)
    return std::unexpected(Fingerprint.error());
  auto Key = computeCacheKey(CanonicalInputMIR, Invocation, *Fingerprint);
  if (!Key)
    return std::unexpected(Key.error());
  const std::filesystem::path EntryPath = cacheEntryPath(CacheRoot, *Key);
  std::error_code FileError;
  if (!std::filesystem::is_directory(EntryPath, FileError) || FileError)
    return makeUnexpected(ErrorCode::CacheMiss, "cache entry is absent");
  auto ManifestText = readText(EntryPath / "manifest.yaml", ErrorCode::CacheMiss);
  if (!ManifestText)
    return std::unexpected(ManifestText.error());
  auto Manifest =
      parseBackendManifest((EntryPath / "manifest.yaml").string(), std::move(*ManifestText));
  if (!Manifest)
    return makeUnexpected(ErrorCode::CacheMiss, std::string(Manifest.error().message()));
  const std::string InputHash = sha256Hex(CanonicalInputMIR);
  auto Identity = verifyManifestIdentity(*Manifest, ManifestKind::Cache, Artifact, Invocation,
                                         InputHash, *Fingerprint, *Key, ErrorCode::CacheMiss);
  if (!Identity)
    return std::unexpected(Identity.error());
  if (!Manifest->ArtifactHash)
    return makeUnexpected(ErrorCode::CacheMiss, "cache manifest has no artifact hash");

  auto StoredInput = readText(EntryPath / "input.mir", ErrorCode::CacheMiss);
  if (!StoredInput)
    return std::unexpected(StoredInput.error());
  if (*StoredInput != CanonicalInputMIR || sha256Hex(*StoredInput) != Manifest->InputMIRHash)
    return makeUnexpected(ErrorCode::CacheMiss, "cached input MIR is inconsistent");
  auto StrictInput = parseAndCanonicalizeMIR("cache-input.mir", *StoredInput, Target, Invocation);
  if (!StrictInput || StrictInput->Text != *StoredInput)
    return makeUnexpected(ErrorCode::CacheMiss, "cached input MIR is not canonical and valid");

  auto ArtifactBytes = readBytes(EntryPath / artifactFileName(Artifact), ErrorCode::CacheMiss);
  if (!ArtifactBytes)
    return std::unexpected(ArtifactBytes.error());
  const auto *Bytes = reinterpret_cast<const std::byte *>(ArtifactBytes->data());
  if (sha256Hex(std::span<const std::byte>(Bytes, ArtifactBytes->size())) !=
      *Manifest->ArtifactHash) {
    return makeUnexpected(ErrorCode::CacheMiss, "cached artifact hash is inconsistent");
  }
  std::optional<CanonicalMIR> ParsedArtifact;
  if (isMIRArtifact(Artifact)) {
    std::string ArtifactText(reinterpret_cast<const char *>(ArtifactBytes->data()),
                             ArtifactBytes->size());
    auto Parsed = parseAndCanonicalizeMIR("cache-artifact.mir", ArtifactText, Target, Invocation);
    if (!Parsed || Parsed->Text != ArtifactText)
      return makeUnexpected(ErrorCode::CacheMiss, "cached MIR artifact is not canonical and valid");
    ParsedArtifact = std::move(*Parsed);
  }
  return CacheEntry{
      .Manifest = std::move(*Manifest),
      .InputMIR = std::move(*StoredInput),
      .Artifact = std::move(*ArtifactBytes),
      .ParsedArtifact = std::move(ParsedArtifact),
  };
}

Expected<void> writeReproducer(const std::filesystem::path &PackagePath,
                               const BackendManifest &Manifest,
                               std::string_view CanonicalInputMIR) {
  auto Verification = Manifest.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (Manifest.Kind != ManifestKind::Reproducer || Manifest.ArtifactHash) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "reproducer requires a reproducer manifest without an artifact hash");
  }
  if (sha256Hex(CanonicalInputMIR) != Manifest.InputMIRHash) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "reproducer input MIR does not match its manifest hash");
  }
  auto Printed = printBackendManifest(Manifest);
  if (!Printed)
    return std::unexpected(Printed.error());
  return writePackageAtomically(PackagePath, *Printed, CanonicalInputMIR, {}, {}, false,
                                ErrorCode::ReproducerError);
}

Expected<void> updateReproducerDiagnostics(const std::filesystem::path &PackagePath,
                                           std::vector<StructuredDiagnostic> Diagnostics) {
  const std::filesystem::path ManifestPath = PackagePath / "manifest.yaml";
  auto ManifestText = readText(ManifestPath, ErrorCode::ReproducerError);
  if (!ManifestText)
    return std::unexpected(ManifestText.error());
  auto Manifest = parseBackendManifest(ManifestPath.string(), std::move(*ManifestText));
  if (!Manifest) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "cannot update an invalid reproducer manifest: " +
                              std::string(Manifest.error().message()));
  }
  if (Manifest->Kind != ManifestKind::Reproducer || Manifest->ArtifactHash) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "diagnostics can only update a reproducer package");
  }
  auto Input = readText(PackagePath / "input.mir", ErrorCode::ReproducerError);
  if (!Input)
    return std::unexpected(Input.error());
  if (sha256Hex(*Input) != Manifest->InputMIRHash) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "reproducer input changed before diagnostic update");
  }
  Manifest->Diagnostics = std::move(Diagnostics);
  auto Printed = printBackendManifest(*Manifest);
  if (!Printed)
    return makeUnexpected(ErrorCode::ReproducerError, std::string(Printed.error().message()));
  return replaceTextAtomically(ManifestPath, *Printed, ErrorCode::ReproducerError);
}

Expected<ReproducerPackage> loadReproducer(const std::filesystem::path &PackagePath,
                                           const TargetDescription &Target) {
  auto ManifestText = readText(PackagePath / "manifest.yaml", ErrorCode::ReproducerError);
  if (!ManifestText)
    return std::unexpected(ManifestText.error());
  auto Manifest =
      parseBackendManifest((PackagePath / "manifest.yaml").string(), std::move(*ManifestText));
  if (!Manifest)
    return makeUnexpected(ErrorCode::ReproducerError, std::string(Manifest.error().message()));
  if (Manifest->Kind != ManifestKind::Reproducer || Manifest->ArtifactHash) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "package does not contain a valid reproducer manifest");
  }
  auto Fingerprint = computeTargetDescriptionFingerprint(Target);
  if (!Fingerprint)
    return std::unexpected(Fingerprint.error());
  if (Manifest->TargetFingerprint != *Fingerprint) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "reproducer target fingerprint does not match this backend");
  }
  auto InputText = readText(PackagePath / "input.mir", ErrorCode::ReproducerError);
  if (!InputText)
    return std::unexpected(InputText.error());
  auto Input = parseAndCanonicalizeMIR((PackagePath / "input.mir").string(), *InputText, Target,
                                       Manifest->Invocation);
  if (!Input)
    return makeUnexpected(ErrorCode::ReproducerError, std::string(Input.error().message()));
  if (Input->Text != *InputText || sha256Hex(Input->Text) != Manifest->InputMIRHash) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "reproducer input MIR is not canonical or has the wrong hash");
  }
  auto Key = computeCacheKey(Input->Text, Manifest->Invocation, *Fingerprint);
  if (!Key)
    return std::unexpected(Key.error());
  if (*Key != Manifest->CacheKey) {
    return makeUnexpected(ErrorCode::ReproducerError,
                          "reproducer cache key is inconsistent with its explicit inputs");
  }
  return ReproducerPackage{
      .Manifest = std::move(*Manifest),
      .Input = std::move(*Input),
  };
}

} // namespace rcc::tooling
