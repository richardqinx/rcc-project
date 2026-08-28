// SPDX-License-Identifier: GPL-3.0-or-later

#include "Common/CodeGenTarget.h"

#include "TableGenBackends.h"
#include "rcc/TableGen/Parser.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rcc::targetgen {

CodeGenTarget::CodeGenTarget(const DescriptionAST &Input, std::string SourceName)
    : Input(Input), SourceName(std::move(SourceName)) {}

[[nodiscard]] std::optional<NormalizedTargetModel> CodeGenTarget::normalize() {
  if (!validateTarget())
    return std::nullopt;

  NormalizedTargetModel Result;
  Result.Target = Input.Target;
  Result.RegisterUnits = Input.RegisterUnits;
  Result.MIROperandFlags = Input.MIROperandFlags;
  if (!normalizeRegisterUnits() || !normalizeRegisters(Result) || !normalizeRegisterBanks(Result) ||
      !normalizeRegisterClasses(Result) || !normalizeRegisterMasks(Result) ||
      !normalizeMIROperandFlags(Result) || !normalizeCallingConventions(Result) ||
      !normalizeNamedOperands(Result) || !normalizeImmediatePredicates(Result) ||
      !normalizeSchedulingClasses(Result) || !normalizeEncodingCategories(Result) ||
      !normalizeAssemblyFormats(Result) || !normalizeInstructions(Result) ||
      !normalizeSelectionRules(Result) || !normalizeCombinerRules(Result) ||
      !normalizeMacroFusionRules(Result) || !normalizeMCPseudoExpansions(Result)) {
    return std::nullopt;
  }
  std::ranges::sort(Result.RegisterUnits, {}, &SourceRegisterUnit::ID);
  std::ranges::sort(Result.Registers, {}, &NormalizedRegister::ID);
  std::ranges::sort(Result.RegisterBanks, {}, &NormalizedRegisterBank::ID);
  std::ranges::sort(Result.RegisterClasses, {}, &NormalizedRegisterClass::ID);
  std::ranges::sort(Result.CallingConventions, {}, &NormalizedCallingConvention::ID);
  std::ranges::sort(Result.NamedOperands, {}, &NormalizedNamedOperand::ID);
  std::ranges::sort(Result.ImmediatePredicates, {}, &NormalizedImmediatePredicate::ID);
  std::ranges::sort(Result.SchedulingClasses, {}, &NormalizedSchedulingClass::ID);
  std::ranges::sort(Result.EncodingCategories, {}, &NormalizedEncodingCategory::ID);
  std::ranges::sort(Result.AssemblyFormats, {}, &NormalizedAssemblyFormat::ID);
  return Result;
}

[[nodiscard]] std::vector<TextDiagnostic> CodeGenTarget::takeDiagnostics() {
  return std::move(Diagnostics);
}

void CodeGenTarget::fail(TextRange Range, std::string Message) {
  Diagnostics.push_back({
      .Severity = DiagnosticSeverity::Error,
      .Category = "target-description",
      .Message = std::move(Message),
      .SourceName = SourceName,
      .Range = Range,
  });
}

[[nodiscard]] bool CodeGenTarget::validateTarget() {
  if (!isIdentifier(Input.Target.Namespace)) {
    fail(Input.Target.Range, "target namespace must be a C++ identifier");
    return false;
  }
  if (Input.Target.Name.empty() || Input.Target.Triple.empty() || Input.Target.CPU.empty() ||
      Input.Target.ABI.empty() || Input.Target.DataLayout.empty() ||
      Input.Target.Features.empty()) {
    fail(Input.Target.Range, "target metadata fields and feature list must be nonempty");
    return false;
  }
  std::unordered_set<std::string> Features;
  for (const std::string &Feature : Input.Target.Features) {
    if (Feature.empty() || !Features.insert(Feature).second) {
      fail(Input.Target.Range, "target feature names must be nonempty and unique");
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool CodeGenTarget::normalizeCallingConventions(NormalizedTargetModel &Result) {
  if (Input.CallingConventions.empty()) {
    fail(Input.Target.Range, "target description requires at least one calling convention");
    return false;
  }
  std::unordered_set<std::uint32_t> IDs;
  std::unordered_set<std::string> Symbols;
  std::unordered_set<std::string> Names;
  for (const SourceCallingConvention &Convention : Input.CallingConventions) {
    const auto IntegerClass = ClassBySymbol.find(Convention.IntegerRegisterClassSymbol);
    const auto FloatingClass = ClassBySymbol.find(Convention.FloatingRegisterClassSymbol);
    const auto PreservedMask = std::ranges::find(Result.RegisterMasks, Convention.PreservedMaskName,
                                                 &NormalizedRegisterMask::Name);
    if (Convention.ID == 0 || !isIdentifier(Convention.Symbol) || Convention.Name.empty() ||
        IntegerClass == ClassBySymbol.end() || FloatingClass == ClassBySymbol.end() ||
        PreservedMask == Result.RegisterMasks.end() || Convention.StackAlignmentBytes == 0 ||
        Convention.StackArgumentAlignmentBytes == 0 ||
        !std::has_single_bit(Convention.StackAlignmentBytes) ||
        !std::has_single_bit(Convention.StackArgumentAlignmentBytes) ||
        Convention.StackAlignmentBytes % Convention.StackArgumentAlignmentBytes != 0 ||
        Convention.ShadowSpaceBytes % Convention.StackArgumentAlignmentBytes != 0 ||
        !IDs.insert(Convention.ID).second || !Symbols.insert(Convention.Symbol).second ||
        !Names.insert(Convention.Name).second) {
      fail(Convention.Range,
           "calling conventions require unique IDs, symbols, names, declared register classes and "
           "preserved masks, compatible power-of-two stack alignments, and aligned shadow space");
      return false;
    }
    const auto IntegerClassDescription = std::ranges::find(
        Result.RegisterClasses, IntegerClass->second, &NormalizedRegisterClass::ID);
    const auto FloatingClassDescription = std::ranges::find(
        Result.RegisterClasses, FloatingClass->second, &NormalizedRegisterClass::ID);
    if (IntegerClassDescription == Result.RegisterClasses.end() ||
        FloatingClassDescription == Result.RegisterClasses.end()) {
      fail(Convention.Range, "calling convention register classes must resolve");
      return false;
    }
    const auto NormalizeRegisterList = [this,
                                        &Convention](const std::vector<std::string> &Symbols,
                                                     const NormalizedRegisterClass &RegisterClass,
                                                     std::vector<std::uint32_t> &Output) {
      std::unordered_set<std::uint32_t> Registers;
      for (const std::string &Symbol : Symbols) {
        const auto Register = RegisterBySymbol.find(Symbol);
        if (Register == RegisterBySymbol.end() ||
            !std::ranges::contains(RegisterClass.Registers, Register->second) ||
            !Registers.insert(Register->second).second) {
          fail(Convention.Range,
               "calling convention register lists must contain distinct members of their class");
          return false;
        }
        Output.push_back(Register->second);
      }
      return true;
    };
    NormalizedCallingConvention Normalized{
        .ID = Convention.ID,
        .Symbol = Convention.Symbol,
        .Name = Convention.Name,
        .IntegerRegisterClass = IntegerClass->second,
        .FloatingRegisterClass = FloatingClass->second,
        .IntegerArgumentRegisters = {},
        .FloatingArgumentRegisters = {},
        .IntegerReturnRegisters = {},
        .FloatingReturnRegisters = {},
        .PreservedMaskName = Convention.PreservedMaskName,
        .StackAlignmentBytes = Convention.StackAlignmentBytes,
        .StackArgumentAlignmentBytes = Convention.StackArgumentAlignmentBytes,
        .ShadowSpaceBytes = Convention.ShadowSpaceBytes,
        .SupportsVarArgs = Convention.SupportsVarArgs,
        .SupportsTailCalls = Convention.SupportsTailCalls,
    };
    if (!NormalizeRegisterList(Convention.IntegerArgumentRegisterSymbols, *IntegerClassDescription,
                               Normalized.IntegerArgumentRegisters) ||
        !NormalizeRegisterList(Convention.FloatingArgumentRegisterSymbols,
                               *FloatingClassDescription, Normalized.FloatingArgumentRegisters) ||
        !NormalizeRegisterList(Convention.IntegerReturnRegisterSymbols, *IntegerClassDescription,
                               Normalized.IntegerReturnRegisters) ||
        !NormalizeRegisterList(Convention.FloatingReturnRegisterSymbols, *FloatingClassDescription,
                               Normalized.FloatingReturnRegisters)) {
      return false;
    }
    CallingConventionBySymbol.emplace(Convention.Symbol, Convention.ID);
    Result.CallingConventions.push_back(std::move(Normalized));
  }
  return true;
}

[[nodiscard]] const NormalizedRegister &findRegister(const NormalizedTargetModel &Model,
                                                     std::uint32_t ID) {
  const auto Position = std::ranges::find(Model.Registers, ID, &NormalizedRegister::ID);
  return *Position;
}
[[nodiscard]] const SourceRegisterUnit &findRegisterUnit(const NormalizedTargetModel &Model,
                                                         std::uint32_t ID) {
  const auto Position = std::ranges::find(Model.RegisterUnits, ID, &SourceRegisterUnit::ID);
  return *Position;
}
[[nodiscard]] const NormalizedRegisterBank &findRegisterBank(const NormalizedTargetModel &Model,
                                                             std::uint32_t ID) {
  const auto Position = std::ranges::find(Model.RegisterBanks, ID, &NormalizedRegisterBank::ID);
  return *Position;
}

DescriptionGenerationResult generateDescription(const std::string &SourceName,
                                                std::string SourceText) {
  DescriptionParseResult ParseResult = parseDescription(SourceName, std::move(SourceText));
  if (!ParseResult.Records)
    return {.Output = std::nullopt, .Diagnostics = std::move(ParseResult.Diagnostics)};

  CodeGenTarget Target(*ParseResult.Records, SourceName);
  auto Model = Target.normalize();
  if (!Model)
    return {.Output = std::nullopt, .Diagnostics = Target.takeDiagnostics()};

  const std::size_t Separator = SourceName.find_last_of("/\\");
  const std::string_view DisplayName = Separator == std::string::npos
                                           ? std::string_view(SourceName)
                                           : std::string_view(SourceName).substr(Separator + 1);
  return {
      .Output =
          GeneratedDescription{
              .RegisterHeader = emitRegisterHeader(*Model, DisplayName),
              .InstructionHeader = emitInstructionHeader(*Model, DisplayName),
              .TargetTables = emitRegisterInfo(*Model, DisplayName) + emitInstructionInfo(*Model) +
                              emitSubtarget(*Model) + emitCodeEmitter(*Model) +
                              emitAssemblyWriter(*Model) + emitSearchableTables(*Model) +
                              emitCallingConventions(*Model),
              .GlobalISel = emitGlobalISel(*Model, DisplayName),
              .GlobalISelCombiner = emitGlobalISelCombiner(*Model, DisplayName),
              .MacroFusion = emitMacroFusion(*Model, DisplayName),
              .MCPseudoLowering = emitMCPseudoLowering(*Model, DisplayName),
          },
      .Diagnostics = {},
  };
}

} // namespace rcc::targetgen
