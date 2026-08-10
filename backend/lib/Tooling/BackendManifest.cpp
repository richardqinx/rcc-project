// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Tooling/BackendManifest.h"

#include "rcc/Support/YAML.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::tooling {
namespace {

[[nodiscard]] bool isDigest(std::string_view Value) noexcept {
  return Value.size() == 64 && std::ranges::all_of(Value, [](unsigned char Character) {
           return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
         });
}

[[nodiscard]] std::string quoted(std::string_view Value) {
  constexpr std::string_view Hex = "0123456789abcdef";
  std::string Result{"\""};
  for (char RawCharacter : Value) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    switch (Character) {
    case '\\':
      Result.append("\\\\");
      break;
    case '"':
      Result.append("\\\"");
      break;
    case '\n':
      Result.append("\\n");
      break;
    case '\r':
      Result.append("\\r");
      break;
    case '\t':
      Result.append("\\t");
      break;
    default:
      if (Character < 0x20U || Character == 0x7fU) {
        Result.append("\\u00");
        Result.push_back(Hex[Character >> 4U]);
        Result.push_back(Hex[Character & 0x0fU]);
      } else {
        Result.push_back(static_cast<char>(Character));
      }
      break;
    }
  }
  Result.push_back('"');
  return Result;
}

void appendScalar(std::string &Output, std::string_view Indent, std::string_view Name,
                  std::string_view Value) {
  Output.append(Indent);
  Output.append(Name);
  Output.append(": ");
  Output.append(quoted(Value));
  Output.push_back('\n');
}

template <typename IntegerT>
void appendInteger(std::string &Output, std::string_view Indent, std::string_view Name,
                   IntegerT Value) {
  Output.append(Indent);
  Output.append(Name);
  Output.append(": ");
  Output.append(std::to_string(Value));
  Output.push_back('\n');
}

void appendBoolean(std::string &Output, std::string_view Indent, std::string_view Name,
                   bool Value) {
  Output.append(Indent);
  Output.append(Name);
  Output.append(Value ? ": true\n" : ": false\n");
}

void appendStringSequence(std::string &Output, std::string_view Indent, std::string_view Name,
                          std::span<const std::string> Values) {
  Output.append(Indent);
  Output.append(Name);
  if (Values.empty()) {
    Output.append(": []\n");
    return;
  }
  Output.append(":\n");
  for (const std::string &Value : Values) {
    Output.append(Indent);
    Output.append("  - ");
    Output.append(quoted(Value));
    Output.push_back('\n');
  }
}

void appendOptions(std::string &Output, std::string_view Indent, std::string_view Name,
                   std::span<const BackendOption> Options) {
  Output.append(Indent);
  Output.append(Name);
  if (Options.empty()) {
    Output.append(": []\n");
    return;
  }
  Output.append(":\n");
  for (const BackendOption &Option : Options) {
    Output.append(Indent);
    Output.append("  - name: ");
    Output.append(quoted(Option.Name));
    Output.push_back('\n');
    appendScalar(Output, std::string(Indent) + "    ", "value", Option.Value);
  }
}

void appendInvocation(std::string &Output, const BackendInvocation &Invocation) {
  appendInteger(Output, "  ", "schema", Invocation.SchemaVersion);
  appendScalar(Output, "  ", "triple", Invocation.Triple);
  appendScalar(Output, "  ", "cpu", Invocation.CPU);
  appendScalar(Output, "  ", "tune_cpu", Invocation.TuneCPU);
  appendStringSequence(Output, "  ", "features", Invocation.Features);
  appendScalar(Output, "  ", "abi", Invocation.ABI);
  appendScalar(Output, "  ", "data_layout", Invocation.DataLayout);
  appendScalar(Output, "  ", "relocation_model", spelling(Invocation.Relocation));
  appendScalar(Output, "  ", "code_model", spelling(Invocation.Code));
  appendOptions(Output, "  ", "target_options", Invocation.TargetOptions);
  appendScalar(Output, "  ", "layout_ownership", spelling(Invocation.Layout));
  appendScalar(Output, "  ", "assembler_flavor", Invocation.AssemblerFlavor);
  appendScalar(Output, "  ", "assembler_version", Invocation.AssemblerVersion);
  appendOptions(Output, "  ", "assembler_options", Invocation.AssemblerOptions);
  appendScalar(Output, "  ", "optimization_level", spelling(Invocation.Optimization));
  appendStringSequence(Output, "  ", "pass_pipeline", Invocation.PassPipeline);
  appendInteger(Output, "  ", "random_seed", Invocation.Determinism.RandomSeed);
  appendInteger(Output, "  ", "thread_count", Invocation.Determinism.ThreadCount);
  appendBoolean(Output, "  ", "stable_scheduling", Invocation.Determinism.StableScheduling);
  if (Invocation.ActionFuel) {
    appendInteger(Output, "  ", "action_fuel", *Invocation.ActionFuel);
  } else {
    Output.append("  action_fuel: null\n");
  }
  appendStringSequence(Output, "  ", "enabled_actions", Invocation.EnabledActions);
  appendScalar(Output, "  ", "mir_reference_revision", Invocation.MIRReferenceRevision);
  appendScalar(Output, "  ", "generic_opcode_version", Invocation.GenericOpcodeVersion);
  appendScalar(Output, "  ", "target_abi_lowering_version", Invocation.TargetABILoweringVersion);
  appendScalar(Output, "  ", "build_fingerprint", Invocation.BuildFingerprint);
}

void appendOptionalInteger(std::string &Output, std::string_view Indent, std::string_view Name,
                           std::optional<std::uint64_t> Value) {
  if (Value) {
    appendInteger(Output, Indent, Name, *Value);
  } else {
    Output.append(Indent);
    Output.append(Name);
    Output.append(": null\n");
  }
}

class ManifestParser final {
public:
  ManifestParser(std::string SourceName, std::string SourceText)
      : Parsed(yaml::parse(std::move(SourceName), std::move(SourceText))) {}

  [[nodiscard]] Expected<BackendManifest> run() {
    if (!Parsed.succeeded()) {
      const std::span<const TextDiagnostic> Diagnostics = Parsed.diagnostics();
      return makeUnexpected(ErrorCode::ManifestError, Diagnostics.empty()
                                                          ? "manifest YAML parsing failed"
                                                          : Diagnostics.front().Message);
    }
    Stream = Parsed.stream();
    if (Stream == nullptr || Stream->documents().size() != 1) {
      return makeUnexpected(ErrorCode::ManifestError,
                            "manifest must contain exactly one YAML document");
    }
    const yaml::Node *Root = Stream->node(Stream->documents().front().Root);
    auto RootMapping = requireMapping(Root, "manifest root");
    if (!RootMapping)
      return std::unexpected(RootMapping.error());
    auto RootKeys =
        rejectUnknown(**RootMapping,
                      {"schema", "kind", "artifact", "input_mir_hash", "target_fingerprint",
                       "cache_key", "artifact_hash", "invocation", "diagnostics"},
                      "manifest root");
    if (!RootKeys)
      return std::unexpected(RootKeys.error());

    BackendManifest Result;
    auto Schema = requiredUnsigned<std::uint32_t>(**RootMapping, "schema", "manifest root");
    auto Kind = requiredScalar(**RootMapping, "kind", "manifest root");
    auto Artifact = requiredScalar(**RootMapping, "artifact", "manifest root");
    auto InputHash = requiredScalar(**RootMapping, "input_mir_hash", "manifest root");
    auto TargetHash = requiredScalar(**RootMapping, "target_fingerprint", "manifest root");
    auto CacheKey = requiredScalar(**RootMapping, "cache_key", "manifest root");
    const yaml::Node *Invocation = mappingValue(**RootMapping, "invocation");
    const yaml::Node *Diagnostics = mappingValue(**RootMapping, "diagnostics");
    if (!Schema || !Kind || !Artifact || !InputHash || !TargetHash || !CacheKey)
      return firstError(Schema, Kind, Artifact, InputHash, TargetHash, CacheKey);
    Result.SchemaVersion = *Schema;
    auto ParsedKind = parseManifestKind(*Kind);
    if (!ParsedKind)
      return std::unexpected(ParsedKind.error());
    Result.Kind = *ParsedKind;
    auto ParsedArtifact = parseArtifactKind(*Artifact);
    if (!ParsedArtifact)
      return std::unexpected(ParsedArtifact.error());
    Result.Artifact = *ParsedArtifact;
    Result.InputMIRHash = std::move(*InputHash);
    Result.TargetFingerprint = std::move(*TargetHash);
    Result.CacheKey = std::move(*CacheKey);
    if (const yaml::Node *ArtifactHash = mappingValue(**RootMapping, "artifact_hash");
        ArtifactHash != nullptr && ArtifactHash->kind() != yaml::NodeKind::Null) {
      auto Value = scalar(ArtifactHash, "manifest artifact_hash");
      if (!Value)
        return std::unexpected(Value.error());
      Result.ArtifactHash = std::move(*Value);
    }
    auto ParsedInvocation = parseInvocation(Invocation);
    if (!ParsedInvocation)
      return std::unexpected(ParsedInvocation.error());
    Result.Invocation = std::move(*ParsedInvocation);
    auto ParsedDiagnostics = parseDiagnostics(Diagnostics);
    if (!ParsedDiagnostics)
      return std::unexpected(ParsedDiagnostics.error());
    Result.Diagnostics = std::move(*ParsedDiagnostics);
    auto Verification = Result.verify();
    if (!Verification)
      return std::unexpected(Verification.error());
    return Result;
  }

private:
  template <typename... Results>
  [[nodiscard]] Expected<BackendManifest> firstError(const Results &...Values) const {
    std::optional<Error> Failure;
    const auto Capture = [&Failure](const auto &Value) {
      if (!Value && !Failure)
        Failure = Value.error();
    };
    (Capture(Values), ...);
    if (!Failure) {
      return makeUnexpected(ErrorCode::InternalError,
                            "manifest parser requested a missing field error without a failure");
    }
    return std::unexpected(*Failure);
  }

  [[nodiscard]] Expected<const yaml::Node *> requireMapping(const yaml::Node *Node,
                                                            std::string_view Context) const {
    if (Node == nullptr || Node->kind() != yaml::NodeKind::Mapping) {
      return makeUnexpected(ErrorCode::ManifestError, std::string(Context) + " must be a mapping");
    }
    return Node;
  }

  [[nodiscard]] Expected<void> rejectUnknown(const yaml::Node &Node,
                                             std::initializer_list<std::string_view> Allowed,
                                             std::string_view Context) const {
    std::unordered_set<std::string_view> Seen;
    for (const yaml::MappingEntry &Entry : Node.mapping()) {
      if (std::ranges::find(Allowed, Entry.Key) == Allowed.end()) {
        return makeUnexpected(ErrorCode::ManifestError,
                              std::string(Context) + " contains unknown key '" + Entry.Key + "'");
      }
      if (!Seen.insert(Entry.Key).second) {
        return makeUnexpected(ErrorCode::ManifestError,
                              std::string(Context) + " contains duplicate key '" + Entry.Key + "'");
      }
    }
    return {};
  }

  [[nodiscard]] const yaml::Node *mappingValue(const yaml::Node &Node,
                                               std::string_view Name) const noexcept {
    const std::optional<yaml::NodeID> ID = Node.mappingValue(Name);
    return ID && Stream != nullptr ? Stream->node(*ID) : nullptr;
  }

  [[nodiscard]] Expected<std::string> scalar(const yaml::Node *Node,
                                             std::string_view Context) const {
    if (Node == nullptr || Node->kind() != yaml::NodeKind::Scalar) {
      return makeUnexpected(ErrorCode::ManifestError, std::string(Context) + " must be a scalar");
    }
    return std::string(Node->scalar());
  }

  [[nodiscard]] Expected<std::string> requiredScalar(const yaml::Node &Node, std::string_view Name,
                                                     std::string_view Context) const {
    const yaml::Node *Value = mappingValue(Node, Name);
    if (Value == nullptr) {
      return makeUnexpected(ErrorCode::ManifestError, std::string(Context) +
                                                          " is missing required key '" +
                                                          std::string(Name) + "'");
    }
    return scalar(Value, std::string(Context) + "." + std::string(Name));
  }

  template <typename IntegerT>
  [[nodiscard]] Expected<IntegerT> unsignedValue(const yaml::Node *Node,
                                                 std::string_view Context) const {
    auto Text = scalar(Node, Context);
    if (!Text)
      return std::unexpected(Text.error());
    IntegerT Result = 0;
    const char *Begin = Text->data();
    const char *End = Begin + Text->size();
    const auto [Position, ErrorValue] = std::from_chars(Begin, End, Result);
    if (ErrorValue != std::errc{} || Position != End) {
      return makeUnexpected(ErrorCode::ManifestError,
                            std::string(Context) + " must be an unsigned integer");
    }
    return Result;
  }

  template <typename IntegerT>
  [[nodiscard]] Expected<IntegerT> requiredUnsigned(const yaml::Node &Node, std::string_view Name,
                                                    std::string_view Context) const {
    const yaml::Node *Value = mappingValue(Node, Name);
    if (Value == nullptr) {
      return makeUnexpected(ErrorCode::ManifestError, std::string(Context) +
                                                          " is missing required key '" +
                                                          std::string(Name) + "'");
    }
    return unsignedValue<IntegerT>(Value, std::string(Context) + "." + std::string(Name));
  }

  [[nodiscard]] Expected<bool> requiredBoolean(const yaml::Node &Node, std::string_view Name,
                                               std::string_view Context) const {
    auto Value = requiredScalar(Node, Name, Context);
    if (!Value)
      return std::unexpected(Value.error());
    if (*Value == "true")
      return true;
    if (*Value == "false")
      return false;
    return makeUnexpected(ErrorCode::ManifestError, std::string(Context) + "." + std::string(Name) +
                                                        " must be true or false");
  }

  [[nodiscard]] Expected<std::vector<std::string>> stringSequence(const yaml::Node *Node,
                                                                  std::string_view Context) const {
    if (Node == nullptr || Node->kind() != yaml::NodeKind::Sequence) {
      return makeUnexpected(ErrorCode::ManifestError, std::string(Context) + " must be a sequence");
    }
    std::vector<std::string> Result;
    Result.reserve(Node->sequence().size());
    for (yaml::NodeID ID : Node->sequence()) {
      auto Value = scalar(Stream->node(ID), Context);
      if (!Value)
        return std::unexpected(Value.error());
      Result.push_back(std::move(*Value));
    }
    return Result;
  }

  [[nodiscard]] Expected<std::vector<BackendOption>> parseOptions(const yaml::Node *Node,
                                                                  std::string_view Context) const {
    if (Node == nullptr || Node->kind() != yaml::NodeKind::Sequence) {
      return makeUnexpected(ErrorCode::ManifestError, std::string(Context) + " must be a sequence");
    }
    std::vector<BackendOption> Result;
    Result.reserve(Node->sequence().size());
    for (yaml::NodeID ID : Node->sequence()) {
      auto Entry = requireMapping(Stream->node(ID), Context);
      if (!Entry)
        return std::unexpected(Entry.error());
      auto Keys = rejectUnknown(**Entry, {"name", "value"}, Context);
      if (!Keys)
        return std::unexpected(Keys.error());
      auto Name = requiredScalar(**Entry, "name", Context);
      auto Value = requiredScalar(**Entry, "value", Context);
      if (!Name)
        return std::unexpected(Name.error());
      if (!Value)
        return std::unexpected(Value.error());
      Result.push_back({std::move(*Name), std::move(*Value)});
    }
    return Result;
  }

  [[nodiscard]] Expected<BackendInvocation> parseInvocation(const yaml::Node *Node) const {
    auto Mapping = requireMapping(Node, "manifest invocation");
    if (!Mapping)
      return std::unexpected(Mapping.error());
    auto Keys = rejectUnknown(**Mapping,
                              {"schema",
                               "triple",
                               "cpu",
                               "tune_cpu",
                               "features",
                               "abi",
                               "data_layout",
                               "relocation_model",
                               "code_model",
                               "target_options",
                               "layout_ownership",
                               "assembler_flavor",
                               "assembler_version",
                               "assembler_options",
                               "optimization_level",
                               "pass_pipeline",
                               "random_seed",
                               "thread_count",
                               "stable_scheduling",
                               "action_fuel",
                               "enabled_actions",
                               "mir_reference_revision",
                               "generic_opcode_version",
                               "target_abi_lowering_version",
                               "build_fingerprint"},
                              "manifest invocation");
    if (!Keys)
      return std::unexpected(Keys.error());

    BackendInvocation Result;
    auto Schema = requiredUnsigned<std::uint32_t>(**Mapping, "schema", "manifest invocation");
    auto Triple = requiredScalar(**Mapping, "triple", "manifest invocation");
    auto CPU = requiredScalar(**Mapping, "cpu", "manifest invocation");
    auto TuneCPU = requiredScalar(**Mapping, "tune_cpu", "manifest invocation");
    auto Features = stringSequence(mappingValue(**Mapping, "features"), "invocation.features");
    auto ABI = requiredScalar(**Mapping, "abi", "manifest invocation");
    auto DataLayout = requiredScalar(**Mapping, "data_layout", "manifest invocation");
    auto Relocation = requiredScalar(**Mapping, "relocation_model", "manifest invocation");
    auto Code = requiredScalar(**Mapping, "code_model", "manifest invocation");
    auto TargetOptions =
        parseOptions(mappingValue(**Mapping, "target_options"), "invocation.target_options");
    auto Layout = requiredScalar(**Mapping, "layout_ownership", "manifest invocation");
    auto AssemblerFlavor = requiredScalar(**Mapping, "assembler_flavor", "manifest invocation");
    auto AssemblerVersion = requiredScalar(**Mapping, "assembler_version", "manifest invocation");
    auto AssemblerOptions =
        parseOptions(mappingValue(**Mapping, "assembler_options"), "invocation.assembler_options");
    auto Optimization = requiredScalar(**Mapping, "optimization_level", "manifest invocation");
    auto Pipeline =
        stringSequence(mappingValue(**Mapping, "pass_pipeline"), "invocation.pass_pipeline");
    auto RandomSeed =
        requiredUnsigned<std::uint64_t>(**Mapping, "random_seed", "manifest invocation");
    auto ThreadCount =
        requiredUnsigned<std::uint32_t>(**Mapping, "thread_count", "manifest invocation");
    auto Stable = requiredBoolean(**Mapping, "stable_scheduling", "manifest invocation");
    auto Actions =
        stringSequence(mappingValue(**Mapping, "enabled_actions"), "invocation.enabled_actions");
    auto MIRRevision = requiredScalar(**Mapping, "mir_reference_revision", "manifest invocation");
    auto GenericVersion =
        requiredScalar(**Mapping, "generic_opcode_version", "manifest invocation");
    auto ABIVersion =
        requiredScalar(**Mapping, "target_abi_lowering_version", "manifest invocation");
    auto Build = requiredScalar(**Mapping, "build_fingerprint", "manifest invocation");

    if (!Schema || !Triple || !CPU || !TuneCPU || !Features || !ABI || !DataLayout || !Relocation ||
        !Code || !TargetOptions || !Layout || !AssemblerFlavor || !AssemblerVersion ||
        !AssemblerOptions || !Optimization || !Pipeline || !RandomSeed || !ThreadCount || !Stable ||
        !Actions || !MIRRevision || !GenericVersion || !ABIVersion || !Build) {
      return makeUnexpected(ErrorCode::ManifestError,
                            "manifest invocation is missing or has an invalid required field");
    }
    Result.SchemaVersion = *Schema;
    Result.Triple = std::move(*Triple);
    Result.CPU = std::move(*CPU);
    Result.TuneCPU = std::move(*TuneCPU);
    Result.Features = std::move(*Features);
    Result.ABI = std::move(*ABI);
    Result.DataLayout = std::move(*DataLayout);
    auto ParsedRelocation = parseRelocationModel(*Relocation);
    auto ParsedCode = parseCodeModel(*Code);
    auto ParsedLayout = parseLayoutOwnership(*Layout);
    auto ParsedOptimization = parseOptimizationLevel(*Optimization);
    if (!ParsedRelocation || !ParsedCode || !ParsedLayout || !ParsedOptimization) {
      return makeUnexpected(ErrorCode::ManifestError,
                            "manifest invocation contains an unknown enum value");
    }
    Result.Relocation = *ParsedRelocation;
    Result.Code = *ParsedCode;
    Result.TargetOptions = std::move(*TargetOptions);
    Result.Layout = *ParsedLayout;
    Result.AssemblerFlavor = std::move(*AssemblerFlavor);
    Result.AssemblerVersion = std::move(*AssemblerVersion);
    Result.AssemblerOptions = std::move(*AssemblerOptions);
    Result.Optimization = *ParsedOptimization;
    Result.PassPipeline = std::move(*Pipeline);
    Result.Determinism = {
        .RandomSeed = *RandomSeed,
        .ThreadCount = *ThreadCount,
        .StableScheduling = *Stable,
    };
    const yaml::Node *Fuel = mappingValue(**Mapping, "action_fuel");
    if (Fuel == nullptr) {
      return makeUnexpected(ErrorCode::ManifestError,
                            "manifest invocation is missing required key 'action_fuel'");
    }
    if (Fuel->kind() != yaml::NodeKind::Null) {
      auto Value = unsignedValue<std::uint64_t>(Fuel, "invocation.action_fuel");
      if (!Value)
        return std::unexpected(Value.error());
      Result.ActionFuel = *Value;
    }
    Result.EnabledActions = std::move(*Actions);
    Result.MIRReferenceRevision = std::move(*MIRRevision);
    Result.GenericOpcodeVersion = std::move(*GenericVersion);
    Result.TargetABILoweringVersion = std::move(*ABIVersion);
    Result.BuildFingerprint = std::move(*Build);
    auto Verified = Result.canonicalizeAndVerify();
    if (!Verified)
      return std::unexpected(Verified.error());
    return Result;
  }

  [[nodiscard]] Expected<std::vector<StructuredDiagnostic>>
  parseDiagnostics(const yaml::Node *Node) const {
    if (Node == nullptr || Node->kind() != yaml::NodeKind::Sequence) {
      return makeUnexpected(ErrorCode::ManifestError, "manifest diagnostics must be a sequence");
    }
    std::vector<StructuredDiagnostic> Result;
    for (yaml::NodeID ID : Node->sequence()) {
      auto Mapping = requireMapping(Stream->node(ID), "manifest diagnostic");
      if (!Mapping)
        return std::unexpected(Mapping.error());
      auto Keys =
          rejectUnknown(**Mapping,
                        {"category", "stage", "pass", "function", "block", "instruction", "operand",
                         "source_location", "target_constraint", "rule", "message"},
                        "manifest diagnostic");
      if (!Keys)
        return std::unexpected(Keys.error());
      StructuredDiagnostic Diagnostic;
      auto Category = requiredScalar(**Mapping, "category", "manifest diagnostic");
      auto Stage = requiredScalar(**Mapping, "stage", "manifest diagnostic");
      auto Pass = requiredScalar(**Mapping, "pass", "manifest diagnostic");
      auto Function = requiredScalar(**Mapping, "function", "manifest diagnostic");
      auto Source = requiredScalar(**Mapping, "source_location", "manifest diagnostic");
      auto Constraint = requiredScalar(**Mapping, "target_constraint", "manifest diagnostic");
      auto Rule = requiredScalar(**Mapping, "rule", "manifest diagnostic");
      auto Message = requiredScalar(**Mapping, "message", "manifest diagnostic");
      if (!Category || !Stage || !Pass || !Function || !Source || !Constraint || !Rule ||
          !Message) {
        return makeUnexpected(ErrorCode::ManifestError,
                              "manifest diagnostic is missing a required string field");
      }
      Diagnostic.Category = std::move(*Category);
      Diagnostic.Stage = std::move(*Stage);
      Diagnostic.Pass = std::move(*Pass);
      Diagnostic.Function = std::move(*Function);
      Diagnostic.SourceLocation = std::move(*Source);
      Diagnostic.TargetConstraint = std::move(*Constraint);
      Diagnostic.Rule = std::move(*Rule);
      Diagnostic.Message = std::move(*Message);
      auto OptionalValue = [&](std::string_view Name, auto &Destination) -> Expected<void> {
        const yaml::Node *Value = mappingValue(**Mapping, Name);
        if (Value == nullptr) {
          return makeUnexpected(ErrorCode::ManifestError,
                                "manifest diagnostic is missing key '" + std::string(Name) + "'");
        }
        if (Value->kind() == yaml::NodeKind::Null)
          return {};
        using ValueT = typename std::remove_reference_t<decltype(Destination)>::value_type;
        auto ParsedValue = unsignedValue<ValueT>(Value, Name);
        if (!ParsedValue)
          return std::unexpected(ParsedValue.error());
        Destination = *ParsedValue;
        return {};
      };
      auto Block = OptionalValue("block", Diagnostic.Block);
      auto Instruction = OptionalValue("instruction", Diagnostic.Instruction);
      auto Operand = OptionalValue("operand", Diagnostic.Operand);
      if (!Block)
        return std::unexpected(Block.error());
      if (!Instruction)
        return std::unexpected(Instruction.error());
      if (!Operand)
        return std::unexpected(Operand.error());
      Result.push_back(std::move(Diagnostic));
    }
    return Result;
  }

  [[nodiscard]] Expected<ManifestKind> parseManifestKind(std::string_view Value) const {
    if (Value == "cache")
      return ManifestKind::Cache;
    if (Value == "reproducer")
      return ManifestKind::Reproducer;
    return makeUnexpected(ErrorCode::ManifestError, "unknown manifest kind");
  }

  [[nodiscard]] Expected<ArtifactKind> parseArtifactKind(std::string_view Value) const {
    if (Value == "prelegalizer-mir")
      return ArtifactKind::PreLegalizerMIR;
    if (Value == "legalized-mir")
      return ArtifactKind::LegalizedMIR;
    if (Value == "regbank-mir")
      return ArtifactKind::RegisterBankMIR;
    if (Value == "selected-mir")
      return ArtifactKind::SelectedMIR;
    if (Value == "post-ssa-mir")
      return ArtifactKind::PostSSAMIR;
    if (Value == "post-ra-mir")
      return ArtifactKind::PostRegisterAllocationMIR;
    if (Value == "final-mir")
      return ArtifactKind::FinalMIR;
    if (Value == "assembly")
      return ArtifactKind::Assembly;
    if (Value == "object")
      return ArtifactKind::RelocatableObject;
    return makeUnexpected(ErrorCode::ManifestError, "unknown artifact kind");
  }

  yaml::ParseResult Parsed;
  const yaml::Stream *Stream = nullptr;
};

} // namespace

std::string_view spelling(ManifestKind Value) noexcept {
  switch (Value) {
  case ManifestKind::Cache:
    return "cache";
  case ManifestKind::Reproducer:
    return "reproducer";
  }
  return "invalid";
}

std::string_view spelling(ArtifactKind Value) noexcept {
  switch (Value) {
  case ArtifactKind::PreLegalizerMIR:
    return "prelegalizer-mir";
  case ArtifactKind::LegalizedMIR:
    return "legalized-mir";
  case ArtifactKind::RegisterBankMIR:
    return "regbank-mir";
  case ArtifactKind::SelectedMIR:
    return "selected-mir";
  case ArtifactKind::PostSSAMIR:
    return "post-ssa-mir";
  case ArtifactKind::PostRegisterAllocationMIR:
    return "post-ra-mir";
  case ArtifactKind::FinalMIR:
    return "final-mir";
  case ArtifactKind::Assembly:
    return "assembly";
  case ArtifactKind::RelocatableObject:
    return "object";
  }
  return "invalid";
}

Expected<void> BackendManifest::verify() const {
  if (SchemaVersion != BackendManifestSchemaVersion) {
    return makeUnexpected(ErrorCode::ManifestError, "manifest schema version is unsupported");
  }
  BackendInvocation Canonical = Invocation;
  auto InvocationVerification = Canonical.canonicalizeAndVerify();
  if (!InvocationVerification)
    return std::unexpected(InvocationVerification.error());
  if (Canonical != Invocation) {
    return makeUnexpected(ErrorCode::ManifestError, "manifest invocation is not in canonical form");
  }
  if (!isDigest(InputMIRHash) || !isDigest(TargetFingerprint) || !isDigest(CacheKey) ||
      (ArtifactHash && !isDigest(*ArtifactHash))) {
    return makeUnexpected(ErrorCode::ManifestError,
                          "manifest hashes must be lowercase SHA-256 digests");
  }
  for (const StructuredDiagnostic &Diagnostic : Diagnostics) {
    if (Diagnostic.Category.empty() || Diagnostic.Message.empty()) {
      return makeUnexpected(ErrorCode::ManifestError,
                            "manifest diagnostic category and message cannot be empty");
    }
  }
  return {};
}

Expected<std::string> printBackendManifest(const BackendManifest &Manifest) {
  auto Verification = Manifest.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  std::string Output;
  Output.append("---\n");
  appendInteger(Output, "", "schema", Manifest.SchemaVersion);
  appendScalar(Output, "", "kind", spelling(Manifest.Kind));
  appendScalar(Output, "", "artifact", spelling(Manifest.Artifact));
  appendScalar(Output, "", "input_mir_hash", Manifest.InputMIRHash);
  appendScalar(Output, "", "target_fingerprint", Manifest.TargetFingerprint);
  appendScalar(Output, "", "cache_key", Manifest.CacheKey);
  if (Manifest.ArtifactHash) {
    appendScalar(Output, "", "artifact_hash", *Manifest.ArtifactHash);
  } else {
    Output.append("artifact_hash: null\n");
  }
  Output.append("invocation:\n");
  appendInvocation(Output, Manifest.Invocation);
  if (Manifest.Diagnostics.empty()) {
    Output.append("diagnostics: []\n");
    return Output;
  }
  Output.append("diagnostics:\n");
  for (const StructuredDiagnostic &Diagnostic : Manifest.Diagnostics) {
    Output.append("  - category: ");
    Output.append(quoted(Diagnostic.Category));
    Output.push_back('\n');
    appendScalar(Output, "    ", "stage", Diagnostic.Stage);
    appendScalar(Output, "    ", "pass", Diagnostic.Pass);
    appendScalar(Output, "    ", "function", Diagnostic.Function);
    appendOptionalInteger(Output, "    ", "block", Diagnostic.Block);
    appendOptionalInteger(Output, "    ", "instruction", Diagnostic.Instruction);
    if (Diagnostic.Operand) {
      appendInteger(Output, "    ", "operand", *Diagnostic.Operand);
    } else {
      Output.append("    operand: null\n");
    }
    appendScalar(Output, "    ", "source_location", Diagnostic.SourceLocation);
    appendScalar(Output, "    ", "target_constraint", Diagnostic.TargetConstraint);
    appendScalar(Output, "    ", "rule", Diagnostic.Rule);
    appendScalar(Output, "    ", "message", Diagnostic.Message);
  }
  return Output;
}

Expected<BackendManifest> parseBackendManifest(std::string SourceName, std::string SourceText) {
  return ManifestParser(std::move(SourceName), std::move(SourceText)).run();
}

} // namespace rcc::tooling
