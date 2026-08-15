// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CommandFlags.h"

#include <optional>
#include <string>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] Expected<std::optional<std::string_view>>
takeValue(std::span<const std::string_view> Arguments, std::size_t &Index, std::string_view Name) {
  const std::string_view Argument = Arguments[Index];
  const std::string LongName =
      Name.starts_with('-') && !Name.starts_with("--") ? "-" + std::string(Name) : std::string();
  if (Argument == Name || (!LongName.empty() && Argument == LongName)) {
    if (Index + 1U >= Arguments.size()) {
      return makeUnexpected(ErrorCode::InvalidInvocation,
                            "missing value for command-line option '" + std::string(Name) + "'");
    }
    return Arguments[++Index];
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

template <typename ValueT, typename ParserT>
[[nodiscard]] Expected<bool> parseValueFlag(std::span<const std::string_view> Arguments,
                                            std::size_t &Index, std::string_view Name,
                                            std::optional<ValueT> &Destination, ParserT Parser) {
  auto Value = takeValue(Arguments, Index, Name);
  if (!Value)
    return std::unexpected(Value.error());
  if (!*Value)
    return false;
  auto Parsed = Parser(**Value);
  if (!Parsed)
    return std::unexpected(Parsed.error());
  Destination = std::move(*Parsed);
  return true;
}

} // namespace

std::string_view spelling(RelocationModel Value) noexcept {
  switch (Value) {
  case RelocationModel::Static:
    return "static";
  case RelocationModel::PositionIndependent:
    return "pic";
  }
  return "invalid";
}

std::string_view spelling(CodeModel Value) noexcept {
  switch (Value) {
  case CodeModel::Small:
    return "small";
  case CodeModel::Medium:
    return "medium";
  case CodeModel::Large:
    return "large";
  }
  return "invalid";
}

std::string_view spelling(OptimizationLevel Value) noexcept {
  switch (Value) {
  case OptimizationLevel::O0:
    return "O0";
  case OptimizationLevel::O1:
    return "O1";
  case OptimizationLevel::O2:
    return "O2";
  case OptimizationLevel::O3:
    return "O3";
  case OptimizationLevel::Os:
    return "Os";
  case OptimizationLevel::Oz:
    return "Oz";
  }
  return "invalid";
}

std::string_view spelling(CodeGenFileType Value) noexcept {
  switch (Value) {
  case CodeGenFileType::AssemblyFile:
    return "asm";
  case CodeGenFileType::ObjectFile:
    return "obj";
  case CodeGenFileType::Null:
    return "null";
  }
  return "invalid";
}

Expected<RelocationModel> parseRelocationModel(std::string_view Value) {
  if (Value == "static")
    return RelocationModel::Static;
  if (Value == "pic")
    return RelocationModel::PositionIndependent;
  return makeUnexpected(ErrorCode::InvalidInvocation, "unknown relocation model");
}

Expected<CodeModel> parseCodeModel(std::string_view Value) {
  if (Value == "small")
    return CodeModel::Small;
  if (Value == "medium")
    return CodeModel::Medium;
  if (Value == "large")
    return CodeModel::Large;
  return makeUnexpected(ErrorCode::InvalidInvocation, "unknown code model");
}

Expected<OptimizationLevel> parseOptimizationLevel(std::string_view Value) {
  if (Value == "O0")
    return OptimizationLevel::O0;
  if (Value == "O1")
    return OptimizationLevel::O1;
  if (Value == "O2")
    return OptimizationLevel::O2;
  if (Value == "O3")
    return OptimizationLevel::O3;
  if (Value == "Os")
    return OptimizationLevel::Os;
  if (Value == "Oz")
    return OptimizationLevel::Oz;
  return makeUnexpected(ErrorCode::InvalidInvocation, "unknown optimization level");
}

Expected<CodeGenFileType> parseCodeGenFileType(std::string_view Value) {
  if (Value == "asm")
    return CodeGenFileType::AssemblyFile;
  if (Value == "obj")
    return CodeGenFileType::ObjectFile;
  if (Value == "null")
    return CodeGenFileType::Null;
  return makeUnexpected(ErrorCode::InvalidInvocation, "unknown file type");
}

Expected<GlobalISelAbortMode> parseGlobalISelAbortMode(std::string_view Value) {
  if (Value == "0")
    return GlobalISelAbortMode::Disable;
  if (Value == "1")
    return GlobalISelAbortMode::Enable;
  if (Value == "2")
    return GlobalISelAbortMode::DisableWithDiagnostic;
  return makeUnexpected(ErrorCode::InvalidInvocation, "-global-isel-abort requires 0, 1, or 2");
}

Expected<bool> parseCodeGenCommandFlag(std::span<const std::string_view> Arguments,
                                       std::size_t &Index, CodeGenCommandFlags &Flags) {
  const std::string_view Argument = Arguments[Index];
  const std::string_view Optimization = Argument.starts_with("--")  ? Argument.substr(2)
                                        : Argument.starts_with('-') ? Argument.substr(1)
                                                                    : std::string_view{};
  if ((Argument.starts_with('-') && !Argument.starts_with("---")) &&
      (Optimization == "O0" || Optimization == "O1" || Optimization == "O2" ||
       Optimization == "O3" || Optimization == "Os" || Optimization == "Oz")) {
    auto Level = parseOptimizationLevel(Optimization);
    if (!Level)
      return std::unexpected(Level.error());
    Flags.Optimization = *Level;
    return true;
  }

  const auto ParseString = [&](std::string_view Name,
                               std::optional<std::string> &Destination) -> Expected<bool> {
    auto Value = takeValue(Arguments, Index, Name);
    if (!Value)
      return std::unexpected(Value.error());
    if (!*Value)
      return false;
    Destination = std::string(**Value);
    return true;
  };
  for (const auto &[Name, Destination] :
       {std::pair{"-mtriple", &Flags.Triple}, std::pair{"-mcpu", &Flags.CPU},
        std::pair{"-mtune", &Flags.TuneCPU}, std::pair{"-target-abi", &Flags.ABI}}) {
    auto Parsed = ParseString(Name, *Destination);
    if (!Parsed)
      return std::unexpected(Parsed.error());
    if (*Parsed)
      return true;
  }

  auto Features = takeValue(Arguments, Index, "-mattr");
  if (!Features)
    return std::unexpected(Features.error());
  if (*Features) {
    std::size_t Begin = 0;
    while (Begin <= (**Features).size()) {
      const std::size_t Comma = (**Features).find(',', Begin);
      const std::size_t End = Comma == std::string_view::npos ? (**Features).size() : Comma;
      if (End == Begin) {
        return makeUnexpected(ErrorCode::InvalidInvocation,
                              "target feature list contains an empty element");
      }
      Flags.FeatureOverrides.emplace_back((**Features).substr(Begin, End - Begin));
      if (Comma == std::string_view::npos)
        break;
      Begin = Comma + 1U;
    }
    return true;
  }

  auto Relocation =
      parseValueFlag(Arguments, Index, "-relocation-model", Flags.Relocation, parseRelocationModel);
  if (!Relocation)
    return std::unexpected(Relocation.error());
  if (*Relocation)
    return true;
  auto Code = parseValueFlag(Arguments, Index, "-code-model", Flags.Code, parseCodeModel);
  if (!Code)
    return std::unexpected(Code.error());
  if (*Code)
    return true;

  auto FileType = takeValue(Arguments, Index, "-filetype");
  if (!FileType)
    return std::unexpected(FileType.error());
  if (*FileType) {
    auto Parsed = parseCodeGenFileType(**FileType);
    if (!Parsed)
      return std::unexpected(Parsed.error());
    Flags.FileType = *Parsed;
    Flags.FileTypeWasSpecified = true;
    return true;
  }
  return false;
}

} // namespace rcc
