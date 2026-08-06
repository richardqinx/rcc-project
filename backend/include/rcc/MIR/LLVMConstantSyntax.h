// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_LLVMCONSTANTSYNTAX_H
#define RCC_MIR_LLVMCONSTANTSYNTAX_H

#include "rcc/CodeGen/GenericConstant.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace rcc::mir {

struct LLVMNamedTypeDefinition final {
  std::string_view Name;
  std::string_view Definition;
  bool NumericName = false;
};

struct LLVMConstantSyntaxOptions final {
  std::uint32_t DefaultPointerSizeInBits = 0;
  std::string_view DataLayout;
  std::span<const LLVMNamedTypeDefinition> NamedTypes;
  std::size_t MaximumDepth = 1024;
  std::size_t MaximumNodes = std::size_t{1} << 24U;
  std::size_t MaximumTokenLength = std::size_t{1} << 24U;
};

[[nodiscard]] Expected<GenericConstantData>
parseLLVMConstant(std::string_view Text, LLVMConstantSyntaxOptions Options = {});

[[nodiscard]] Expected<GenericConstantType> parseLLVMType(std::string_view Text,
                                                          LLVMConstantSyntaxOptions Options = {});

[[nodiscard]] Expected<std::string> printLLVMConstant(const GenericConstantData &Constant);

[[nodiscard]] Expected<std::uint64_t>
llvmConstantStorageSize(const GenericConstantType &Type, LLVMConstantSyntaxOptions Options = {});

[[nodiscard]] Expected<Alignment>
llvmConstantPreferredAlignment(const GenericConstantType &Type,
                               LLVMConstantSyntaxOptions Options = {});

[[nodiscard]] Expected<std::uint32_t> llvmPointerSizeInBits(std::uint32_t AddressSpace,
                                                            LLVMConstantSyntaxOptions Options = {});

} // namespace rcc::mir

#endif // RCC_MIR_LLVMCONSTANTSYNTAX_H
