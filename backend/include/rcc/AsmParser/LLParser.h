// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_ASMPARSER_LLPARSER_H
#define RCC_ASMPARSER_LLPARSER_H

#include "rcc/IR/Constants.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace rcc {

struct LLNamedTypeDefinition final {
  std::string_view Name;
  std::string_view Definition;
  bool NumericName = false;
};

struct LLParserOptions final {
  std::span<const LLNamedTypeDefinition> NamedTypes;
  std::size_t MaximumDepth = 1024;
  std::size_t MaximumNodes = std::size_t{1} << 24U;
  std::size_t MaximumTokenLength = std::size_t{1} << 24U;
};

[[nodiscard]] Expected<GenericConstantData> parseLLVMConstant(std::string_view Text,
                                                              LLParserOptions Options = {});

[[nodiscard]] Expected<GenericConstantType> parseLLVMType(std::string_view Text,
                                                          LLParserOptions Options = {});

} // namespace rcc

#endif // RCC_ASMPARSER_LLPARSER_H
