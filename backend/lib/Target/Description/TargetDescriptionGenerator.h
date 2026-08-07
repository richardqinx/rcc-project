// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_DESCRIPTION_TARGETDESCRIPTIONGENERATOR_H
#define RCC_TARGET_DESCRIPTION_TARGETDESCRIPTIONGENERATOR_H

#include "rcc/Support/TextDiagnostic.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rcc::targetgen {

struct GeneratedDescription final {
  std::string RegisterHeader;
  std::string InstructionHeader;
  std::string TargetTables;
};

struct DescriptionGenerationResult final {
  std::optional<GeneratedDescription> Output;
  std::vector<TextDiagnostic> Diagnostics;

  [[nodiscard]] bool succeeded() const noexcept {
    return Output.has_value() && Diagnostics.empty();
  }
};

[[nodiscard]] DescriptionGenerationResult generateDescription(std::string SourceName,
                                                              std::string SourceText);

} // namespace rcc::targetgen

#endif // RCC_TARGET_DESCRIPTION_TARGETDESCRIPTIONGENERATOR_H
