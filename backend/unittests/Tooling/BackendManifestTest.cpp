// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/BackendManifest.h"

#include "ToolingTestSupport.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using rcc::tooling::BackendInvocation;

[[nodiscard]] bool keyDiffers(const BackendInvocation &Base, BackendInvocation Changed,
                              std::string_view Input, std::string_view Fingerprint) {
  auto Canonicalized = Changed.canonicalizeAndVerify();
  if (!Canonicalized)
    return false;
  auto BaseKey = rcc::tooling::computeCacheKey(Input, Base, Fingerprint);
  auto ChangedKey = rcc::tooling::computeCacheKey(Input, Changed, Fingerprint);
  return BaseKey && ChangedKey && *BaseKey != *ChangedKey;
}

} // namespace

int main() {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return tooling_test::fail(Target.error().message());
  const BackendInvocation Base = rcc::tooling::defaultRV64GCInvocation();
  auto Fingerprint = rcc::tooling::computeTargetDescriptionFingerprint(*Target);
  if (!Fingerprint || Fingerprint->size() != 64)
    return tooling_test::fail("target description fingerprint must be a SHA-256 digest");
  auto RepeatedFingerprint = rcc::tooling::computeTargetDescriptionFingerprint(*Target);
  if (!RepeatedFingerprint || *RepeatedFingerprint != *Fingerprint)
    return tooling_test::fail("target description fingerprint must be deterministic");

  BackendInvocation Unordered = Base;
  std::ranges::reverse(Unordered.Features);
  std::ranges::reverse(Unordered.TargetOptions);
  Unordered.EnabledActions = {"spill", "coalesce"};
  auto Canonicalized = Unordered.canonicalizeAndVerify();
  if (!Canonicalized || !std::ranges::is_sorted(Unordered.Features) ||
      !std::ranges::is_sorted(Unordered.TargetOptions, {}, &rcc::tooling::BackendOption::Name) ||
      Unordered.EnabledActions != std::vector<std::string>({"coalesce", "spill"})) {
    return tooling_test::fail("backend invocation canonicalization must sort explicit identities");
  }
  BackendInvocation DuplicateFeature = Base;
  DuplicateFeature.Features.push_back("+i");
  if (DuplicateFeature.canonicalizeAndVerify())
    return tooling_test::fail("backend invocation must reject duplicate target features");
  BackendInvocation ZeroThreads = Base;
  ZeroThreads.Determinism.ThreadCount = 0;
  if (ZeroThreads.canonicalizeAndVerify())
    return tooling_test::fail("backend invocation must reject a zero thread count");

  constexpr std::string_view Input = "canonical MIR";
  auto BaseKey = rcc::tooling::computeCacheKey(Input, Base, *Fingerprint);
  if (!BaseKey || BaseKey->size() != 64)
    return tooling_test::fail("cache key must be a SHA-256 digest");
  auto DifferentInput = rcc::tooling::computeCacheKey("different MIR", Base, *Fingerprint);
  if (!DifferentInput || *DifferentInput == *BaseKey)
    return tooling_test::fail("cache key must include canonical MIR");
  std::string DifferentFingerprint(64, '0');
  auto DifferentTarget = rcc::tooling::computeCacheKey(Input, Base, DifferentFingerprint);
  if (!DifferentTarget || *DifferentTarget == *BaseKey)
    return tooling_test::fail("cache key must include the target description fingerprint");

  BackendInvocation Changed = Base;
  Changed.Triple += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the target triple");
  Changed = Base;
  Changed.CPU += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the target CPU");
  Changed = Base;
  Changed.TuneCPU += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the tuning CPU");
  Changed = Base;
  Changed.Features.front() = "+variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include target features");
  Changed = Base;
  Changed.ABI += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the ABI");
  Changed = Base;
  Changed.DataLayout += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the data layout");
  Changed = Base;
  Changed.Relocation = rcc::tooling::RelocationModel::Static;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the relocation model");
  Changed = Base;
  Changed.Code = rcc::tooling::CodeModel::Medium;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the code model");
  Changed = Base;
  Changed.TargetOptions.front().Value = "variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include target options");
  Changed = Base;
  Changed.Layout = rcc::tooling::LayoutOwnership::CompilerFinalLayout;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include layout ownership");
  Changed = Base;
  Changed.AssemblerVersion += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include assembler identity");
  Changed = Base;
  Changed.AssemblerOptions.front().Value = "false";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include assembler options");
  Changed = Base;
  Changed.Optimization = rcc::tooling::OptimizationLevel::O3;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the optimization level");
  Changed = Base;
  Changed.PassPipeline = {"Legalizer"};
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the pass pipeline");
  Changed = Base;
  Changed.Determinism.RandomSeed = 1;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the random seed");
  Changed = Base;
  Changed.Determinism.ThreadCount = 2;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the thread count");
  Changed = Base;
  Changed.Determinism.StableScheduling = false;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include stable scheduling");
  Changed = Base;
  Changed.ActionFuel = 7;
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include Action fuel");
  Changed = Base;
  Changed.EnabledActions = {"Legalizer"};
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include enabled Actions");
  Changed = Base;
  Changed.BuildFingerprint += "-variant";
  if (!keyDiffers(Base, Changed, Input, *Fingerprint))
    return tooling_test::fail("cache key must include the backend build fingerprint");

  rcc::tooling::StructuredDiagnostic Diagnostic;
  Diagnostic.Category = "verification error";
  Diagnostic.Stage = "selected-mir";
  Diagnostic.Pass = "test-pass";
  Diagnostic.Function = "test-function";
  Diagnostic.Block = 3;
  Diagnostic.Instruction = 7;
  Diagnostic.Operand = 1;
  Diagnostic.SourceLocation = "input.mir:4:5";
  Diagnostic.TargetConstraint = "GPR";
  Diagnostic.Rule = "test-rule";
  Diagnostic.Message = "quoted \"message\"\nline";
  auto Manifest = rcc::tooling::createBackendManifest(rcc::tooling::ManifestKind::Reproducer,
                                                      rcc::tooling::ArtifactKind::Assembly, Base,
                                                      *Target, Input, std::nullopt, {Diagnostic});
  if (!Manifest)
    return tooling_test::fail(Manifest.error().message());
  auto Printed = rcc::tooling::printBackendManifest(*Manifest);
  if (!Printed)
    return tooling_test::fail(Printed.error().message());
  auto Parsed = rcc::tooling::parseBackendManifest("manifest.yaml", *Printed);
  if (!Parsed || *Parsed != *Manifest)
    return tooling_test::fail("manifest printer and strict parser must round-trip all fields");

  BackendInvocation EmptyCollections = Base;
  EmptyCollections.TargetOptions.clear();
  EmptyCollections.AssemblerOptions.clear();
  auto EmptyManifest = rcc::tooling::createBackendManifest(rcc::tooling::ManifestKind::Reproducer,
                                                           rcc::tooling::ArtifactKind::FinalMIR,
                                                           EmptyCollections, *Target, Input);
  if (!EmptyManifest)
    return tooling_test::fail(EmptyManifest.error().message());
  auto EmptyPrinted = rcc::tooling::printBackendManifest(*EmptyManifest);
  if (!EmptyPrinted)
    return tooling_test::fail(EmptyPrinted.error().message());
  auto EmptyParsed = rcc::tooling::parseBackendManifest("empty-manifest.yaml", *EmptyPrinted);
  if (!EmptyParsed || *EmptyParsed != *EmptyManifest)
    return tooling_test::fail("manifest must preserve explicitly empty collections");

  std::string Unknown = *Printed;
  Unknown.insert(Unknown.find("schema:"), "unknown: 1\n");
  if (rcc::tooling::parseBackendManifest("unknown.yaml", std::move(Unknown)))
    return tooling_test::fail("strict manifest parsing must reject unknown keys");
  std::string Duplicate = *Printed;
  Duplicate.insert(Duplicate.find("schema:"), "schema: 1\n");
  if (rcc::tooling::parseBackendManifest("duplicate.yaml", std::move(Duplicate)))
    return tooling_test::fail("strict manifest parsing must reject duplicate keys");
  std::string Missing = *Printed;
  const std::size_t ABI = Missing.find("  abi:");
  Missing.erase(ABI, Missing.find('\n', ABI) - ABI + 1U);
  if (rcc::tooling::parseBackendManifest("missing.yaml", std::move(Missing)))
    return tooling_test::fail("strict manifest parsing must reject missing invocation facts");

  return 0;
}
