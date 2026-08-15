// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_COMMANDFLAGS_H
#define RCC_CODEGEN_COMMANDFLAGS_H

#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

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

enum class CodeGenFileType : std::uint8_t {
  AssemblyFile,
  ObjectFile,
  Null,
};

enum class GlobalISelAbortMode : std::uint8_t {
  Disable,
  Enable,
  DisableWithDiagnostic,
};

struct CodeGenCommandFlags final {
  std::optional<std::string> Triple;
  std::optional<std::string> CPU;
  std::optional<std::string> TuneCPU;
  std::optional<std::string> ABI;
  std::vector<std::string> FeatureOverrides;
  std::optional<RelocationModel> Relocation;
  std::optional<CodeModel> Code;
  std::optional<OptimizationLevel> Optimization;
  CodeGenFileType FileType = CodeGenFileType::AssemblyFile;
  bool FileTypeWasSpecified = false;
};

[[nodiscard]] std::string_view spelling(RelocationModel Value) noexcept;
[[nodiscard]] std::string_view spelling(CodeModel Value) noexcept;
[[nodiscard]] std::string_view spelling(OptimizationLevel Value) noexcept;
[[nodiscard]] std::string_view spelling(CodeGenFileType Value) noexcept;

[[nodiscard]] Expected<RelocationModel> parseRelocationModel(std::string_view Value);
[[nodiscard]] Expected<CodeModel> parseCodeModel(std::string_view Value);
[[nodiscard]] Expected<OptimizationLevel> parseOptimizationLevel(std::string_view Value);
[[nodiscard]] Expected<CodeGenFileType> parseCodeGenFileType(std::string_view Value);
[[nodiscard]] Expected<GlobalISelAbortMode> parseGlobalISelAbortMode(std::string_view Value);
[[nodiscard]] Expected<bool> parseCodeGenCommandFlag(std::span<const std::string_view> Arguments,
                                                     std::size_t &Index,
                                                     CodeGenCommandFlags &Flags);

} // namespace rcc

#endif // RCC_CODEGEN_COMMANDFLAGS_H
