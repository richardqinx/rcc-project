// SPDX-License-Identifier: GPL-3.0-or-later

// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_UTILS_TABLEGEN_TABLEGENBACKENDS_H
#define RCC_UTILS_TABLEGEN_TABLEGENBACKENDS_H

#include "Common/CodeGenTarget.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rcc::targetgen {

struct GeneratedDescription final {
  std::string RegisterHeader;
  std::string InstructionHeader;
  std::string TargetTables;
  std::string GlobalISel;
  std::string GlobalISelCombiner;
  std::string MacroFusion;
};

struct DescriptionGenerationResult final {
  std::optional<GeneratedDescription> Output;
  std::vector<TextDiagnostic> Diagnostics;

  [[nodiscard]] bool succeeded() const noexcept {
    return Output.has_value() && Diagnostics.empty();
  }
};

[[nodiscard]] std::string emitRegisterHeader(const NormalizedTargetModel &Model,
                                             std::string_view SourceName);
[[nodiscard]] std::string emitRegisterInfo(const NormalizedTargetModel &Model,
                                           std::string_view SourceName);
[[nodiscard]] std::string emitRegisterBanks(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitInstructionHeader(const NormalizedTargetModel &Model,
                                                std::string_view SourceName);
[[nodiscard]] std::string emitInstructionInfo(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitCallingConventions(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitCodeEmitter(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitAssemblyWriter(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitSubtarget(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitSearchableTables(const NormalizedTargetModel &Model);
[[nodiscard]] std::string emitGlobalISel(const NormalizedTargetModel &Model,
                                         std::string_view SourceName);
[[nodiscard]] std::string emitGlobalISelCombiner(const NormalizedTargetModel &Model,
                                                 std::string_view SourceName);
[[nodiscard]] std::string emitMacroFusion(const NormalizedTargetModel &Model,
                                          std::string_view SourceName);

[[nodiscard]] DescriptionGenerationResult generateDescription(const std::string &SourceName,
                                                              std::string SourceText);

} // namespace rcc::targetgen

#endif // RCC_UTILS_TABLEGEN_TABLEGENBACKENDS_H
