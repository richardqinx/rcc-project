// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TABLEGEN_PARSER_H
#define RCC_TABLEGEN_PARSER_H

#include "rcc/TableGen/Record.h"

#include <optional>
#include <string>
#include <vector>

namespace rcc::targetgen {

struct DescriptionParseResult final {
  std::optional<DescriptionAST> Records;
  std::vector<TextDiagnostic> Diagnostics;
};

[[nodiscard]] DescriptionParseResult parseDescription(std::string SourceName,
                                                      std::string SourceText);

} // namespace rcc::targetgen

#endif // RCC_TABLEGEN_PARSER_H
