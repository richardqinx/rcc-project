// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CommandFlags.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/Support/Error.h"
#include "rcc/Target/RISCV/RISCVPipeline.h"
#include "rcc/Target/RISCV/RISCVTarget.h"
#include "rcc/Tooling/ArtifactStore.h"
#include "rcc/Tooling/BackendDriver.h"
#include "rcc/Tooling/BackendInvocation.h"
#include "rcc/Tooling/MIRFuzzer.h"
#include "rcc/Tooling/MIRReducer.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using rcc::Error;
using rcc::ErrorCode;
using rcc::Expected;
using rcc::tooling::ArtifactKind;
using rcc::tooling::BackendInvocation;
using rcc::tooling::BackendOutputKind;
using rcc::tooling::BackendRunResult;
using rcc::tooling::CanonicalMIR;

enum class Operation : std::uint8_t {
  Run,
  Verify,
  Reproduce,
  ReduceValidity,
  ReduceFailure,
  Fuzz,
  ListPasses,
};

enum class CachePolicy : std::uint8_t {
  Off,
  Read,
  Write,
  ReadWrite,
};

struct CommandLine final {
  Operation Action = Operation::Run;
  CachePolicy Cache = CachePolicy::Off;
  std::string InputPath;
  std::string OutputPath = "-";
  std::filesystem::path ReproducerPath;
  std::filesystem::path CachePath;
  BackendOutputKind Output = BackendOutputKind::FinalMIR;
  BackendInvocation Invocation = rcc::tooling::defaultRV64GCInvocation();
  rcc::tooling::PipelineControls Controls;
  rcc::tooling::MIRFuzzOptions FuzzOptions;
  bool OutputWasSpecified = false;
  bool DiscardOutput = false;
  bool PrintActions = false;
  bool ShowHelp = false;
};

void printUsage(std::ostream &Output) {
  Output << "usage: rcc-backend-mir [operation] [options] <input.mir|->\n"
            "operations:\n"
            "  --verify                       Parse, canonicalize, and verify MIR\n"
            "  --reproduce <package>          Strictly replay a reproducer package\n"
            "  --reduce <validity|failure>    Structurally reduce MIR\n"
            "  --fuzz <layer,...|all>         Run deterministic layered fuzzing\n"
            "  --list-passes                  List the canonical pipeline suffix\n"
            "pipeline/output:\n"
            "  -run-pass <name>               Run one named pass\n"
            "  -passes <name,...>             Run an explicit pass sequence\n"
            "  -start-before <pass[,N]>       Start before a pass instance\n"
            "  -start-after <pass[,N]>        Start after a pass instance\n"
            "  -stop-before <pass[,N]>        Stop before a pass instance\n"
            "  -stop-after <pass[,N]>         Stop after a pass instance\n"
            "  -print-before <selector,...>   Print snapshots before passes\n"
            "  -print-after <selector,...>    Print snapshots after passes\n"
            "  -simplify-mir                  Omit default-valued MIR fields\n"
            "  -verify-machineinstrs          Verify each machine pass result\n"
            "  --stage-snapshots              Print every stage boundary\n"
            "  -filetype <asm|obj|null>       Select the output artifact\n"
            "  -o <file|->                    Select the output path\n"
            "reproduction/cache/fuzz:\n"
            "  --crash-reproducer <path>      Save input and manifest before execution\n"
            "  --cache-dir <path>             Select the content-addressed cache root\n"
            "  --cache-policy <policy>        off, read, write, or read-write\n"
            "  --fuzz-iterations <count>      Cases per selected fuzz layer\n"
            "execution identity:\n"
            "  --threads <count>              Function worker count\n"
            "  --seed <value>                 Deterministic random seed\n"
            "  --fuel <value>                 Per-function Action fuel\n"
            "  --enable-action <name>         Enable an Action ID component\n"
            "  --print-actions                Print stable Action decisions\n"
            "  -O0|-O1|-O2|-O3|-Os|-Oz        Optimization level identity\n"
            "  -relocation-model <static|pic>\n"
            "  -code-model <small|medium|large>\n"
            "  --layout <native-object|compiler-final-layout|external-relaxation>\n"
            "  -mtriple/-mcpu/-mtune/-mattr/-target-abi <value>\n"
            "  --data-layout <value>          Override the MIR data layout\n"
            "  --target-option <name=value>   Explicit target option\n"
            "  --assembler-option <name=value>\n"
            "  --assembler-flavor/--assembler-version <value>\n";
}

[[nodiscard]] Expected<std::optional<std::string_view>> takeOption(std::string_view Argument,
                                                                   std::string_view Name,
                                                                   int &Index, int ArgumentCount,
                                                                   char *Arguments[]) {
  const std::string LongName =
      Name.starts_with('-') && !Name.starts_with("--") ? "-" + std::string(Name) : std::string();
  if (Argument == Name || (!LongName.empty() && Argument == LongName)) {
    if (Index + 1 >= ArgumentCount) {
      return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                                 "missing value for command-line option '" + std::string(Name) +
                                     "'");
    }
    return std::string_view(Arguments[++Index]);
  }
  std::string Prefix(Name);
  Prefix.push_back('=');
  if (Argument.starts_with(Prefix))
    return Argument.substr(Prefix.size());
  if (!LongName.empty()) {
    std::string LongPrefix(LongName);
    LongPrefix.push_back('=');
    if (Argument.starts_with(LongPrefix))
      return Argument.substr(LongPrefix.size());
  }
  return std::nullopt;
}

template <typename IntegerT>
[[nodiscard]] Expected<IntegerT> parseInteger(std::string_view Text, std::string_view Description) {
  IntegerT Value = 0;
  const auto Result = std::from_chars(Text.data(), Text.data() + Text.size(), Value);
  if (Result.ec != std::errc{} || Result.ptr != Text.data() + Text.size()) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               std::string(Description) + " must be an unsigned decimal integer");
  }
  return Value;
}

[[nodiscard]] Expected<std::vector<std::string>> splitList(std::string_view Text,
                                                           std::string_view Description) {
  std::vector<std::string> Result;
  std::size_t Begin = 0;
  while (Begin <= Text.size()) {
    const std::size_t Comma = Text.find(',', Begin);
    const std::size_t End = Comma == std::string_view::npos ? Text.size() : Comma;
    if (End == Begin) {
      return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                                 std::string(Description) + " contains an empty element");
    }
    Result.emplace_back(Text.substr(Begin, End - Begin));
    if (Comma == std::string_view::npos)
      break;
    Begin = Comma + 1U;
  }
  return Result;
}

void applyFeatureOverrides(std::vector<std::string> Overrides, std::vector<std::string> &Features) {
  for (std::string &Override : Overrides) {
    if (!Override.empty() && Override.front() != '+' && Override.front() != '-')
      Override.insert(Override.begin(), '+');
    const std::string_view Name = std::string_view(Override).substr(1);
    std::erase_if(Features, [Name](const std::string &Feature) {
      return Feature.size() > 1 && std::string_view(Feature).substr(1) == Name;
    });
    Features.push_back(std::move(Override));
  }
}

[[nodiscard]] std::string normalizeRV64Triple(std::string_view Triple) {
  if (Triple == "riscv64" || Triple == "riscv64-linux-gnu")
    return "riscv64-unknown-linux-gnu";
  return std::string(Triple);
}

[[nodiscard]] Expected<rcc::tooling::BackendOption> parseBackendOption(std::string_view Text,
                                                                       std::string_view Category) {
  const std::size_t Equal = Text.find('=');
  if (Equal == std::string_view::npos || Equal == 0) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               std::string(Category) + " option must use name=value syntax");
  }
  return rcc::tooling::BackendOption{std::string(Text.substr(0, Equal)),
                                     std::string(Text.substr(Equal + 1U))};
}

[[nodiscard]] Expected<void> setOperation(CommandLine &Command, Operation Action) {
  if (Command.Action != Operation::Run && Command.Action != Action) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               "command-line operations are mutually exclusive");
  }
  Command.Action = Action;
  return {};
}

[[nodiscard]] Expected<CachePolicy> parseCachePolicy(std::string_view Value) {
  if (Value == "off")
    return CachePolicy::Off;
  if (Value == "read")
    return CachePolicy::Read;
  if (Value == "write")
    return CachePolicy::Write;
  if (Value == "read-write")
    return CachePolicy::ReadWrite;
  return rcc::makeUnexpected(ErrorCode::InvalidInvocation, "unknown cache policy");
}

[[nodiscard]] Expected<void> parseFuzzLayers(std::string_view Value, CommandLine &Command) {
  auto OperationResult = setOperation(Command, Operation::Fuzz);
  if (!OperationResult)
    return OperationResult;
  if (Value == "all") {
    Command.FuzzOptions.Layers.clear();
    return {};
  }
  auto Values = splitList(Value, "fuzz layer list");
  if (!Values)
    return std::unexpected(Values.error());
  Command.FuzzOptions.Layers.clear();
  for (const std::string &LayerName : *Values) {
    auto Layer = rcc::tooling::parseMIRFuzzLayer(LayerName);
    if (!Layer)
      return std::unexpected(Layer.error());
    Command.FuzzOptions.Layers.push_back(*Layer);
  }
  return {};
}

[[nodiscard]] Expected<CommandLine> parseCommandLine(int ArgumentCount, char *Arguments[]) {
  CommandLine Command;
  bool ExplicitPipeline = false;
  bool UsedRunPass = false;
  bool UsedPasses = false;
  rcc::CodeGenCommandFlags CodeGenFlags;
  std::vector<std::string_view> ArgumentViews;
  ArgumentViews.reserve(static_cast<std::size_t>(ArgumentCount));
  for (int Index = 0; Index < ArgumentCount; ++Index)
    ArgumentViews.emplace_back(Arguments[Index]);
  for (int Index = 1; Index < ArgumentCount; ++Index) {
    const std::string_view Argument(Arguments[Index]);
    if (Argument == "--help" || Argument == "-h") {
      Command.ShowHelp = true;
      continue;
    }
    if (Argument == "--verify") {
      auto Set = setOperation(Command, Operation::Verify);
      if (!Set)
        return std::unexpected(Set.error());
      continue;
    }
    if (Argument == "--list-passes") {
      auto Set = setOperation(Command, Operation::ListPasses);
      if (!Set)
        return std::unexpected(Set.error());
      continue;
    }
    if (Argument == "--stage-snapshots") {
      Command.Controls.CaptureStageSnapshots = true;
      continue;
    }
    if (Argument == "-simplify-mir" || Argument == "--simplify-mir") {
      Command.Controls.SimplifyMIR = true;
      continue;
    }
    if (Argument == "-verify-machineinstrs" || Argument == "--verify-machineinstrs") {
      Command.Controls.VerifyEach = true;
      continue;
    }
    if (Argument == "--no-verify-each") {
      Command.Controls.VerifyEach = false;
      continue;
    }
    if (Argument == "--no-change-detection") {
      Command.Controls.DetectUnreportedChanges = false;
      continue;
    }
    if (Argument == "--print-actions") {
      Command.PrintActions = true;
      continue;
    }
    std::size_t CodeGenIndex = static_cast<std::size_t>(Index);
    auto CodeGenFlag = rcc::parseCodeGenCommandFlag(ArgumentViews, CodeGenIndex, CodeGenFlags);
    if (!CodeGenFlag)
      return std::unexpected(CodeGenFlag.error());
    if (*CodeGenFlag) {
      Index = static_cast<int>(CodeGenIndex);
      continue;
    }

    auto Output = takeOption(Argument, "-o", Index, ArgumentCount, Arguments);
    if (!Output)
      return std::unexpected(Output.error());
    if (*Output) {
      Command.OutputPath = std::string(**Output);
      Command.OutputWasSpecified = true;
      continue;
    }
    auto RunPass = takeOption(Argument, "-run-pass", Index, ArgumentCount, Arguments);
    if (!RunPass)
      return std::unexpected(RunPass.error());
    if (*RunPass) {
      if (UsedPasses)
        return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                                   "-run-pass and -passes cannot be combined");
      auto Names = splitList(**RunPass, "pass pipeline");
      if (!Names)
        return std::unexpected(Names.error());
      if (!UsedRunPass)
        Command.Invocation.PassPipeline.clear();
      Command.Invocation.PassPipeline.insert(Command.Invocation.PassPipeline.end(),
                                             std::make_move_iterator(Names->begin()),
                                             std::make_move_iterator(Names->end()));
      UsedRunPass = true;
      ExplicitPipeline = true;
      continue;
    }
    auto Passes = takeOption(Argument, "-passes", Index, ArgumentCount, Arguments);
    if (!Passes)
      return std::unexpected(Passes.error());
    if (*Passes) {
      if (UsedRunPass || UsedPasses)
        return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                                   "-run-pass and -passes cannot be combined");
      auto Names = splitList(**Passes, "pass pipeline");
      if (!Names)
        return std::unexpected(Names.error());
      Command.Invocation.PassPipeline = std::move(*Names);
      UsedPasses = true;
      ExplicitPipeline = true;
      continue;
    }

    const auto ParseSelector = [&](std::string_view Name,
                                   std::optional<std::string> &Destination) -> Expected<bool> {
      auto Value = takeOption(Argument, Name, Index, ArgumentCount, Arguments);
      if (!Value)
        return std::unexpected(Value.error());
      if (!*Value)
        return false;
      Destination = std::string(**Value);
      return true;
    };
    auto StopBefore = ParseSelector("-stop-before", Command.Controls.StopBefore);
    if (!StopBefore)
      return std::unexpected(StopBefore.error());
    if (*StopBefore)
      continue;
    auto StopAfter = ParseSelector("-stop-after", Command.Controls.StopAfter);
    if (!StopAfter)
      return std::unexpected(StopAfter.error());
    if (*StopAfter)
      continue;
    auto StartBefore = ParseSelector("-start-before", Command.Controls.StartBefore);
    if (!StartBefore)
      return std::unexpected(StartBefore.error());
    if (*StartBefore)
      continue;
    auto StartAfter = ParseSelector("-start-after", Command.Controls.StartAfter);
    if (!StartAfter)
      return std::unexpected(StartAfter.error());
    if (*StartAfter)
      continue;

    auto InputLanguage = takeOption(Argument, "-x", Index, ArgumentCount, Arguments);
    if (!InputLanguage)
      return std::unexpected(InputLanguage.error());
    if (*InputLanguage) {
      if (**InputLanguage != "mir") {
        return rcc::makeUnexpected(ErrorCode::InvalidInvocation, "input language must be 'mir'");
      }
      continue;
    }

    const auto ParseSelectorList = [&](std::string_view Name,
                                       std::vector<std::string> &Destination) -> Expected<bool> {
      auto Value = takeOption(Argument, Name, Index, ArgumentCount, Arguments);
      if (!Value)
        return std::unexpected(Value.error());
      if (!*Value)
        return false;
      auto Selectors = splitList(**Value, Name);
      if (!Selectors)
        return std::unexpected(Selectors.error());
      Destination.insert(Destination.end(), std::make_move_iterator(Selectors->begin()),
                         std::make_move_iterator(Selectors->end()));
      return true;
    };
    auto PrintBefore = ParseSelectorList("-print-before", Command.Controls.PrintBefore);
    if (!PrintBefore)
      return std::unexpected(PrintBefore.error());
    if (*PrintBefore)
      continue;
    auto PrintAfter = ParseSelectorList("-print-after", Command.Controls.PrintAfter);
    if (!PrintAfter)
      return std::unexpected(PrintAfter.error());
    if (*PrintAfter)
      continue;

    auto Reproduce = takeOption(Argument, "--reproduce", Index, ArgumentCount, Arguments);
    if (!Reproduce)
      return std::unexpected(Reproduce.error());
    if (*Reproduce) {
      auto Set = setOperation(Command, Operation::Reproduce);
      if (!Set)
        return std::unexpected(Set.error());
      Command.ReproducerPath = std::string(**Reproduce);
      continue;
    }
    auto Reduce = takeOption(Argument, "--reduce", Index, ArgumentCount, Arguments);
    if (!Reduce)
      return std::unexpected(Reduce.error());
    if (*Reduce) {
      Operation Mode;
      if (**Reduce == "validity")
        Mode = Operation::ReduceValidity;
      else if (**Reduce == "failure")
        Mode = Operation::ReduceFailure;
      else
        return rcc::makeUnexpected(ErrorCode::InvalidInvocation, "unknown reducer mode");
      auto Set = setOperation(Command, Mode);
      if (!Set)
        return std::unexpected(Set.error());
      continue;
    }
    auto Fuzz = takeOption(Argument, "--fuzz", Index, ArgumentCount, Arguments);
    if (!Fuzz)
      return std::unexpected(Fuzz.error());
    if (*Fuzz) {
      auto Parsed = parseFuzzLayers(**Fuzz, Command);
      if (!Parsed)
        return std::unexpected(Parsed.error());
      continue;
    }
    auto FuzzIterations =
        takeOption(Argument, "--fuzz-iterations", Index, ArgumentCount, Arguments);
    if (!FuzzIterations)
      return std::unexpected(FuzzIterations.error());
    if (*FuzzIterations) {
      auto Count = parseInteger<std::size_t>(**FuzzIterations, "fuzz iteration count");
      if (!Count)
        return std::unexpected(Count.error());
      Command.FuzzOptions.Iterations = *Count;
      continue;
    }
    auto CrashReproducer =
        takeOption(Argument, "--crash-reproducer", Index, ArgumentCount, Arguments);
    if (!CrashReproducer)
      return std::unexpected(CrashReproducer.error());
    if (*CrashReproducer) {
      Command.ReproducerPath = std::string(**CrashReproducer);
      continue;
    }
    auto CacheDirectory = takeOption(Argument, "--cache-dir", Index, ArgumentCount, Arguments);
    if (!CacheDirectory)
      return std::unexpected(CacheDirectory.error());
    if (*CacheDirectory) {
      Command.CachePath = std::string(**CacheDirectory);
      continue;
    }
    auto CacheMode = takeOption(Argument, "--cache-policy", Index, ArgumentCount, Arguments);
    if (!CacheMode)
      return std::unexpected(CacheMode.error());
    if (*CacheMode) {
      auto Policy = parseCachePolicy(**CacheMode);
      if (!Policy)
        return std::unexpected(Policy.error());
      Command.Cache = *Policy;
      continue;
    }

    const auto ParseUnsigned = [&](std::string_view Name, auto &Destination,
                                   std::string_view Description) -> Expected<bool> {
      auto Value = takeOption(Argument, Name, Index, ArgumentCount, Arguments);
      if (!Value)
        return std::unexpected(Value.error());
      if (!*Value)
        return false;
      using IntegerT = std::remove_reference_t<decltype(Destination)>;
      auto Parsed = parseInteger<IntegerT>(**Value, Description);
      if (!Parsed)
        return std::unexpected(Parsed.error());
      Destination = *Parsed;
      return true;
    };
    auto Threads =
        ParseUnsigned("--threads", Command.Invocation.Determinism.ThreadCount, "thread count");
    if (!Threads)
      return std::unexpected(Threads.error());
    if (*Threads)
      continue;
    auto Seed = ParseUnsigned("--seed", Command.Invocation.Determinism.RandomSeed, "random seed");
    if (!Seed)
      return std::unexpected(Seed.error());
    if (*Seed) {
      Command.FuzzOptions.Seed = Command.Invocation.Determinism.RandomSeed;
      continue;
    }
    auto Fuel = takeOption(Argument, "--fuel", Index, ArgumentCount, Arguments);
    if (!Fuel)
      return std::unexpected(Fuel.error());
    if (*Fuel) {
      auto Value = parseInteger<std::uint64_t>(**Fuel, "Action fuel");
      if (!Value)
        return std::unexpected(Value.error());
      Command.Invocation.ActionFuel = *Value;
      continue;
    }
    auto EnabledAction = takeOption(Argument, "--enable-action", Index, ArgumentCount, Arguments);
    if (!EnabledAction)
      return std::unexpected(EnabledAction.error());
    if (*EnabledAction) {
      Command.Invocation.EnabledActions.emplace_back(**EnabledAction);
      continue;
    }

    const auto ParseString = [&](std::string_view Name,
                                 std::string &Destination) -> Expected<bool> {
      auto Value = takeOption(Argument, Name, Index, ArgumentCount, Arguments);
      if (!Value)
        return std::unexpected(Value.error());
      if (!*Value)
        return false;
      Destination = std::string(**Value);
      return true;
    };
    auto DataLayout = ParseString("--data-layout", Command.Invocation.DataLayout);
    if (!DataLayout)
      return std::unexpected(DataLayout.error());
    if (*DataLayout)
      continue;
    auto AssemblerFlavor = ParseString("--assembler-flavor", Command.Invocation.AssemblerFlavor);
    if (!AssemblerFlavor)
      return std::unexpected(AssemblerFlavor.error());
    if (*AssemblerFlavor)
      continue;
    auto AssemblerVersion = ParseString("--assembler-version", Command.Invocation.AssemblerVersion);
    if (!AssemblerVersion)
      return std::unexpected(AssemblerVersion.error());
    if (*AssemblerVersion)
      continue;
    auto Layout = takeOption(Argument, "--layout", Index, ArgumentCount, Arguments);
    if (!Layout)
      return std::unexpected(Layout.error());
    if (*Layout) {
      auto Value = rcc::tooling::parseLayoutOwnership(**Layout);
      if (!Value)
        return std::unexpected(Value.error());
      Command.Invocation.Layout = *Value;
      continue;
    }
    auto TargetOption = takeOption(Argument, "--target-option", Index, ArgumentCount, Arguments);
    if (!TargetOption)
      return std::unexpected(TargetOption.error());
    if (*TargetOption) {
      auto Value = parseBackendOption(**TargetOption, "target");
      if (!Value)
        return std::unexpected(Value.error());
      const auto Existing = std::ranges::find(Command.Invocation.TargetOptions, Value->Name,
                                              &rcc::tooling::BackendOption::Name);
      if (Existing == Command.Invocation.TargetOptions.end())
        Command.Invocation.TargetOptions.push_back(std::move(*Value));
      else
        Existing->Value = std::move(Value->Value);
      continue;
    }
    auto AssemblerOption =
        takeOption(Argument, "--assembler-option", Index, ArgumentCount, Arguments);
    if (!AssemblerOption)
      return std::unexpected(AssemblerOption.error());
    if (*AssemblerOption) {
      auto Value = parseBackendOption(**AssemblerOption, "assembler");
      if (!Value)
        return std::unexpected(Value.error());
      const auto Existing = std::ranges::find(Command.Invocation.AssemblerOptions, Value->Name,
                                              &rcc::tooling::BackendOption::Name);
      if (Existing == Command.Invocation.AssemblerOptions.end())
        Command.Invocation.AssemblerOptions.push_back(std::move(*Value));
      else
        Existing->Value = std::move(Value->Value);
      continue;
    }

    if (Argument == "-" || !Argument.starts_with('-')) {
      if (!Command.InputPath.empty())
        return rcc::makeUnexpected(ErrorCode::InvalidInvocation, "input path is specified twice");
      Command.InputPath = std::string(Argument);
      continue;
    }
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               "unknown command-line option '" + std::string(Argument) + "'");
  }

  if (Command.ShowHelp)
    return Command;
  if (CodeGenFlags.Triple)
    Command.Invocation.Triple = normalizeRV64Triple(*CodeGenFlags.Triple);
  if (CodeGenFlags.CPU)
    Command.Invocation.CPU = *CodeGenFlags.CPU;
  if (CodeGenFlags.TuneCPU)
    Command.Invocation.TuneCPU = *CodeGenFlags.TuneCPU;
  if (CodeGenFlags.ABI)
    Command.Invocation.ABI = *CodeGenFlags.ABI;
  applyFeatureOverrides(std::move(CodeGenFlags.FeatureOverrides), Command.Invocation.Features);
  if (CodeGenFlags.Relocation)
    Command.Invocation.Relocation = *CodeGenFlags.Relocation;
  if (CodeGenFlags.Code)
    Command.Invocation.Code = *CodeGenFlags.Code;
  if (CodeGenFlags.Optimization)
    Command.Invocation.Optimization = *CodeGenFlags.Optimization;
  const bool HasLimitedCodeGenPipeline = Command.Controls.StartBefore ||
                                         Command.Controls.StartAfter ||
                                         Command.Controls.StopBefore || Command.Controls.StopAfter;
  if (UsedRunPass && HasLimitedCodeGenPipeline) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               "-run-pass cannot be used with a limited code-generation pipeline");
  }
  if (ExplicitPipeline || HasLimitedCodeGenPipeline) {
    Command.Output = BackendOutputKind::FinalMIR;
  } else {
    switch (CodeGenFlags.FileType) {
    case rcc::CodeGenFileType::AssemblyFile:
      Command.Output = BackendOutputKind::Assembly;
      break;
    case rcc::CodeGenFileType::ObjectFile:
      Command.Output = BackendOutputKind::RelocatableObject;
      break;
    case rcc::CodeGenFileType::Null:
      Command.Output = BackendOutputKind::Assembly;
      Command.DiscardOutput = true;
      break;
    }
  }
  Command.DiscardOutput =
      CodeGenFlags.FileTypeWasSpecified && CodeGenFlags.FileType == rcc::CodeGenFileType::Null;
  if (Command.Action == Operation::Reproduce) {
    if (!Command.InputPath.empty())
      return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                                 "reproducer replay does not accept a separate MIR input");
  } else if (Command.InputPath.empty()) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation, "a MIR input path is required");
  }
  if (Command.Cache != CachePolicy::Off && Command.CachePath.empty()) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               "a non-off cache policy requires --cache-dir");
  }
  if (Command.Action != Operation::Run && Command.Action != Operation::Reproduce &&
      Command.Cache != CachePolicy::Off) {
    return rcc::makeUnexpected(ErrorCode::InvalidInvocation,
                               "cache policies only apply to run and reproduce operations");
  }
  auto Canonical = Command.Invocation.canonicalizeAndVerify();
  if (!Canonical)
    return std::unexpected(Canonical.error());
  return Command;
}

[[nodiscard]] Expected<std::string> readInput(std::string_view Path) {
  if (Path == "-")
    return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
  std::ifstream Input(std::string(Path), std::ios::binary);
  if (!Input)
    return rcc::makeUnexpected(ErrorCode::IoError,
                               "cannot open input file '" + std::string(Path) + "'");
  return std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
}

[[nodiscard]] Expected<void> writeOutput(std::string_view Path,
                                         std::span<const std::uint8_t> Bytes) {
  if (Path == "-") {
    if (!Bytes.empty()) {
      std::cout.write(reinterpret_cast<const char *>(Bytes.data()),
                      static_cast<std::streamsize>(Bytes.size()));
    }
    if (!std::cout)
      return rcc::makeUnexpected(ErrorCode::IoError, "cannot write standard output");
    return {};
  }
  std::ofstream Output(std::string(Path), std::ios::binary | std::ios::trunc);
  if (!Output)
    return rcc::makeUnexpected(ErrorCode::IoError,
                               "cannot open output file '" + std::string(Path) + "'");
  if (!Bytes.empty()) {
    Output.write(reinterpret_cast<const char *>(Bytes.data()),
                 static_cast<std::streamsize>(Bytes.size()));
  }
  Output.close();
  if (!Output)
    return rcc::makeUnexpected(ErrorCode::IoError,
                               "cannot write output file '" + std::string(Path) + "'");
  return {};
}

[[nodiscard]] Expected<void> writeText(std::string_view Path, std::string_view Text) {
  const auto *Bytes = reinterpret_cast<const std::uint8_t *>(Text.data());
  return writeOutput(Path, std::span<const std::uint8_t>(Bytes, Text.size()));
}

[[nodiscard]] Expected<BackendOutputKind> outputFromArtifact(ArtifactKind Artifact) {
  switch (Artifact) {
  case ArtifactKind::PreLegalizerMIR:
    return BackendOutputKind::PreLegalizerMIR;
  case ArtifactKind::LegalizedMIR:
    return BackendOutputKind::LegalizedMIR;
  case ArtifactKind::RegisterBankMIR:
    return BackendOutputKind::RegisterBankMIR;
  case ArtifactKind::SelectedMIR:
    return BackendOutputKind::SelectedMIR;
  case ArtifactKind::PostSSAMIR:
    return BackendOutputKind::PostSSAMIR;
  case ArtifactKind::PostRegisterAllocationMIR:
    return BackendOutputKind::PostRegisterAllocationMIR;
  case ArtifactKind::FinalMIR:
    return BackendOutputKind::FinalMIR;
  case ArtifactKind::Assembly:
    return BackendOutputKind::Assembly;
  case ArtifactKind::RelocatableObject:
    return BackendOutputKind::RelocatableObject;
  }
  return rcc::makeUnexpected(ErrorCode::ReproducerError, "invalid reproducer artifact kind");
}

void printSnapshots(const BackendRunResult &Result) {
  for (const rcc::tooling::PipelineSnapshot &Snapshot : Result.Pipeline.Snapshots) {
    std::cerr << "*** " << rcc::tooling::spelling(Snapshot.Stage);
    if (!Snapshot.Pass.empty())
      std::cerr << ' ' << Snapshot.Pass << '#' << Snapshot.Occurrence;
    if (!Snapshot.Function.empty())
      std::cerr << " [" << Snapshot.Function << ']';
    std::cerr << " ***\n" << Snapshot.MIR;
    if (!Snapshot.MIR.ends_with('\n'))
      std::cerr << '\n';
  }
}

void printActions(const BackendRunResult &Result) {
  for (const rcc::MachineActionEvent &Action : Result.Actions) {
    std::cerr << (Action.Accepted ? "accepted " : "rejected ") << Action.ID.toString()
              << " cost=" << Action.Cost << '\n';
  }
}

[[nodiscard]] Expected<void> writePreExecutionReproducer(const CommandLine &Command,
                                                         const CanonicalMIR &Input,
                                                         const rcc::TargetDescription &Target) {
  if (Command.ReproducerPath.empty())
    return {};
  auto Manifest = rcc::tooling::createBackendManifest(
      rcc::tooling::ManifestKind::Reproducer, rcc::tooling::manifestArtifactKind(Command.Output),
      Command.Invocation, Target, Input.Text);
  if (!Manifest)
    return std::unexpected(Manifest.error());
  return rcc::tooling::writeReproducer(Command.ReproducerPath, *Manifest, Input.Text);
}

[[nodiscard]] Expected<BackendRunResult>
runWithCache(const CommandLine &Command, CanonicalMIR Input, const rcc::TargetDescription &Target,
             rcc::MachinePassInstrumentation *Instrumentation = nullptr) {
  if (Command.Cache == CachePolicy::Off) {
    return rcc::tooling::runBackend(std::move(Input), Target, Command.Invocation, Command.Output,
                                    Command.Controls, Instrumentation);
  }
  auto Kind = rcc::tooling::cacheArtifactKind(Command.Output);
  if (!Kind)
    return std::unexpected(Kind.error());
  const ArtifactKind CacheKind = *Kind;
  if (Command.Cache == CachePolicy::Read || Command.Cache == CachePolicy::ReadWrite) {
    auto Cached = rcc::tooling::loadCacheEntry(Command.CachePath, CacheKind, Command.Invocation,
                                               Target, Input.Text);
    if (Cached) {
      return BackendRunResult{
          .Kind = Command.Output,
          .Output = std::move(Cached->Artifact),
          .Pipeline = {},
          .Actions = {},
      };
    }
    if (Command.Cache == CachePolicy::Read || Cached.error().code() != ErrorCode::CacheMiss)
      return std::unexpected(Cached.error());
  }

  const std::string CanonicalInput = Input.Text;
  auto Run = rcc::tooling::runBackend(std::move(Input), Target, Command.Invocation, Command.Output,
                                      Command.Controls, Instrumentation);
  if (!Run)
    return std::unexpected(Run.error());
  if (Command.Cache == CachePolicy::Write || Command.Cache == CachePolicy::ReadWrite) {
    auto Manifest = rcc::tooling::createBackendManifest(
        rcc::tooling::ManifestKind::Cache, CacheKind, Command.Invocation, Target, CanonicalInput,
        std::span<const std::uint8_t>(Run->Output));
    if (!Manifest)
      return std::unexpected(Manifest.error());
    auto Stored =
        rcc::tooling::storeCacheEntry(Command.CachePath, *Manifest, CanonicalInput, Run->Output);
    if (!Stored)
      return std::unexpected(Stored.error());
  }
  return Run;
}

[[nodiscard]] Expected<void> runCommand(const CommandLine &OriginalCommand) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return std::unexpected(Target.error());
  CommandLine Command = OriginalCommand;

  if (Command.Action == Operation::Reproduce) {
    auto Package = rcc::tooling::loadReproducer(Command.ReproducerPath, *Target);
    if (!Package)
      return std::unexpected(Package.error());
    auto Output = outputFromArtifact(Package->Manifest.Artifact);
    if (!Output)
      return std::unexpected(Output.error());
    Command.Output = *Output;
    Command.Invocation = Package->Manifest.Invocation;
    Command.ReproducerPath.clear();
    auto Run = runWithCache(Command, std::move(Package->Input), *Target);
    if (!Run)
      return std::unexpected(Run.error());
    printSnapshots(*Run);
    if (Command.PrintActions)
      printActions(*Run);
    return writeOutput(Command.OutputPath, Run->Output);
  }

  auto Source = readInput(Command.InputPath);
  if (!Source)
    return std::unexpected(Source.error());
  const std::string SourceName = Command.InputPath == "-" ? "<stdin>" : Command.InputPath;
  auto Input = rcc::tooling::parseAndCanonicalizeMIR(SourceName, std::move(*Source), *Target,
                                                     Command.Invocation);
  if (!Input)
    return std::unexpected(Input.error());

  if (Command.Action == Operation::Verify) {
    if (Command.OutputWasSpecified)
      return writeText(Command.OutputPath, Input->Text);
    return {};
  }
  if (Command.Action == Operation::ListPasses) {
    std::vector<std::string> Names;
    for (std::size_t Index = 0; Index < Input->Document.unit().functionCount(); ++Index) {
      const rcc::MachineFunction *Function = Input->Document.unit().functionAt(Index);
      if (Function == nullptr)
        return rcc::makeUnexpected(ErrorCode::InternalError, "MIR input contains a null function");
      auto FunctionNames = rcc::riscv::canonicalPipelinePassNames(*Function);
      if (!FunctionNames)
        return std::unexpected(FunctionNames.error());
      for (std::string &Name : *FunctionNames) {
        if (std::ranges::find(Names, Name) == Names.end())
          Names.push_back(std::move(Name));
      }
    }
    std::string Output;
    for (const std::string &Name : Names) {
      Output.append(Name);
      Output.push_back('\n');
    }
    return writeText(Command.OutputPath, Output);
  }
  if (Command.Action == Operation::Fuzz) {
    auto Result = rcc::tooling::fuzzMIR(Command.InputPath, Input->Text, Command.Invocation, *Target,
                                        Command.FuzzOptions);
    if (!Result)
      return std::unexpected(Result.error());
    std::string Output = "seed=" + std::to_string(Result->Seed) +
                         " executed=" + std::to_string(Result->executed()) +
                         " accepted=" + std::to_string(Result->accepted()) +
                         " rejected=" + std::to_string(Result->rejected()) + "\n";
    for (const rcc::tooling::MIRFuzzLayerResult &Layer : Result->Layers) {
      Output.append(rcc::tooling::spelling(Layer.Layer));
      Output.append(": executed=");
      Output.append(std::to_string(Layer.Executed));
      Output.append(" accepted=");
      Output.append(std::to_string(Layer.Accepted));
      Output.append(" rejected=");
      Output.append(std::to_string(Layer.Rejected));
      Output.push_back('\n');
    }
    return writeText(Command.OutputPath, Output);
  }
  if (Command.Action == Operation::ReduceValidity) {
    rcc::tooling::MIRReducerOptions Options;
    Options.Mode = rcc::tooling::ReductionMode::PreserveValidity;
    auto Reduced = rcc::tooling::reduceMIR(Command.InputPath, Input->Text, Command.Invocation,
                                           *Target, {}, std::move(Options));
    if (!Reduced)
      return std::unexpected(Reduced.error());
    return writeText(Command.OutputPath, Reduced->MIR);
  }
  if (Command.Action == Operation::ReduceFailure) {
    auto InitialForRun = rcc::tooling::parseAndCanonicalizeMIR("reducer-initial.mir", Input->Text,
                                                               *Target, Command.Invocation);
    if (!InitialForRun)
      return std::unexpected(InitialForRun.error());
    auto InitialRun = rcc::tooling::runBackend(
        std::move(*InitialForRun), *Target, Command.Invocation, Command.Output, Command.Controls);
    if (InitialRun) {
      return rcc::makeUnexpected(ErrorCode::ReductionFailed,
                                 "failure-preserving reduction requires an initially failing run");
    }
    const ErrorCode FailureCode = InitialRun.error().code();
    const auto Interesting = [&](std::string_view MIR,
                                 const BackendInvocation &Invocation) -> Expected<bool> {
      auto Candidate = rcc::tooling::parseAndCanonicalizeMIR("reducer-interesting.mir",
                                                             std::string(MIR), *Target, Invocation);
      if (!Candidate)
        return std::unexpected(Candidate.error());
      auto Run = rcc::tooling::runBackend(std::move(*Candidate), *Target, Invocation,
                                          Command.Output, Command.Controls);
      return !Run && Run.error().code() == FailureCode;
    };
    rcc::tooling::MIRReducerOptions Options;
    Options.Mode = rcc::tooling::ReductionMode::PreserveFailure;
    auto Reduced = rcc::tooling::reduceMIR(Command.InputPath, Input->Text, Command.Invocation,
                                           *Target, Interesting, std::move(Options));
    if (!Reduced)
      return std::unexpected(Reduced.error());
    return writeText(Command.OutputPath, Reduced->MIR);
  }

  auto Reproducer = writePreExecutionReproducer(Command, *Input, *Target);
  if (!Reproducer)
    return std::unexpected(Reproducer.error());

  std::vector<rcc::tooling::StructuredDiagnostic> FailureDiagnostics;
  std::mutex FailureDiagnosticsMutex;
  rcc::MachinePassInstrumentation Instrumentation;
  const auto RecordFunctionFailure = [&](std::string_view Rule, std::string_view Pass,
                                         const rcc::MachineFunction &Function,
                                         const Error &Failure) {
    rcc::tooling::StructuredDiagnostic Diagnostic;
    Diagnostic.Category = std::string(rcc::getErrorCodeName(Failure.code()));
    Diagnostic.Stage =
        std::string(rcc::tooling::spelling(rcc::tooling::stageForFunction(Function)));
    Diagnostic.Pass = std::string(Pass);
    Diagnostic.Function = std::string(Function.name());
    Diagnostic.Rule = std::string(Rule);
    Diagnostic.Message = std::string(Failure.message());
    std::scoped_lock Lock(FailureDiagnosticsMutex);
    FailureDiagnostics.push_back(std::move(Diagnostic));
  };
  Instrumentation.addFailureCallback(
      [&](std::string_view Pass, const rcc::MachineFunction &Function, const Error &Failure) {
        RecordFunctionFailure("machine-pass", Pass, Function, Failure);
      });
  Instrumentation.addAnalysisFailureCallback(
      [&](std::string_view Analysis, const rcc::MachineFunction &Function, const Error &Failure) {
        RecordFunctionFailure("machine-analysis", Analysis, Function, Failure);
      });
  Instrumentation.addUnitFailureCallback(
      [&](std::string_view Pass, const rcc::CompilationUnit &, const Error &Failure) {
        rcc::tooling::StructuredDiagnostic Diagnostic;
        Diagnostic.Category = std::string(rcc::getErrorCodeName(Failure.code()));
        Diagnostic.Stage = "compilation-unit";
        Diagnostic.Pass = std::string(Pass);
        Diagnostic.Rule = "compilation-unit-pass";
        Diagnostic.Message = std::string(Failure.message());
        std::scoped_lock Lock(FailureDiagnosticsMutex);
        FailureDiagnostics.push_back(std::move(Diagnostic));
      });
  Instrumentation.addUnitAnalysisFailureCallback(
      [&](std::string_view Analysis, const rcc::CompilationUnit &, const Error &Failure) {
        rcc::tooling::StructuredDiagnostic Diagnostic;
        Diagnostic.Category = std::string(rcc::getErrorCodeName(Failure.code()));
        Diagnostic.Stage = "compilation-unit";
        Diagnostic.Pass = std::string(Analysis);
        Diagnostic.Rule = "compilation-unit-analysis";
        Diagnostic.Message = std::string(Failure.message());
        std::scoped_lock Lock(FailureDiagnosticsMutex);
        FailureDiagnostics.push_back(std::move(Diagnostic));
      });

  auto Run = runWithCache(Command, std::move(*Input), *Target,
                          Command.ReproducerPath.empty() ? nullptr : &Instrumentation);
  if (!Run) {
    if (!Command.ReproducerPath.empty()) {
      if (FailureDiagnostics.empty()) {
        rcc::tooling::StructuredDiagnostic Diagnostic;
        Diagnostic.Category = std::string(rcc::getErrorCodeName(Run.error().code()));
        Diagnostic.Stage = "driver";
        Diagnostic.Rule = "backend-driver";
        Diagnostic.Message = std::string(Run.error().message());
        FailureDiagnostics.push_back(std::move(Diagnostic));
      }
      std::ranges::sort(FailureDiagnostics, [](const auto &Left, const auto &Right) {
        if (Left.Function != Right.Function)
          return Left.Function < Right.Function;
        if (Left.Stage != Right.Stage)
          return Left.Stage < Right.Stage;
        if (Left.Pass != Right.Pass)
          return Left.Pass < Right.Pass;
        if (Left.Rule != Right.Rule)
          return Left.Rule < Right.Rule;
        if (Left.Category != Right.Category)
          return Left.Category < Right.Category;
        return Left.Message < Right.Message;
      });
      auto Updated = rcc::tooling::updateReproducerDiagnostics(Command.ReproducerPath,
                                                               std::move(FailureDiagnostics));
      if (!Updated) {
        return rcc::makeUnexpected(ErrorCode::ReproducerError,
                                   "backend failed with '" + std::string(Run.error().message()) +
                                       "' and reproducer diagnostic update failed with '" +
                                       std::string(Updated.error().message()) + "'");
      }
    }
    return std::unexpected(Run.error());
  }
  printSnapshots(*Run);
  if (Command.PrintActions)
    printActions(*Run);
  if (Command.DiscardOutput)
    return {};
  return writeOutput(Command.OutputPath, Run->Output);
}

} // namespace

int main(int ArgumentCount, char *Arguments[]) {
  auto Command = parseCommandLine(ArgumentCount, Arguments);
  if (!Command) {
    std::cerr << "rcc-backend-mir: " << Command.error().message() << '\n';
    printUsage(std::cerr);
    return 1;
  }
  if (Command->ShowHelp) {
    printUsage(std::cout);
    return 0;
  }
  auto Result = runCommand(*Command);
  if (!Result) {
    if (Result.error().code() == ErrorCode::ParseError) {
      std::cerr << Result.error().message() << '\n';
      return 1;
    }
    std::cerr << "rcc-backend-mir: " << rcc::getErrorCodeName(Result.error().code()) << ": "
              << Result.error().message() << '\n';
    return 1;
  }
  return 0;
}
