// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_IR_DATALAYOUT_H
#define RCC_IR_DATALAYOUT_H

#include "rcc/IR/Constants.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class DataLayout final {
public:
  [[nodiscard]] static Expected<DataLayout> parse(std::string_view LayoutString);

  [[nodiscard]] Expected<std::uint64_t> getTypeAllocSize(const GenericConstantType &Type) const;
  [[nodiscard]] Expected<std::uint64_t> getABITypeAlign(const GenericConstantType &Type) const;
  [[nodiscard]] Expected<std::uint64_t> getPrefTypeAlign(const GenericConstantType &Type) const;
  [[nodiscard]] Expected<std::uint32_t> getPointerSizeInBits(std::uint32_t AddressSpace = 0) const;

private:
  struct AlignmentSpec final {
    std::uint64_t ABI = 1;
    std::uint64_t Preferred = 1;
  };

  struct PrimitiveSpec final {
    char Kind = '\0';
    std::uint32_t BitWidth = 0;
    AlignmentSpec Alignment;
  };

  struct PointerSpec final {
    std::uint32_t AddressSpace = 0;
    std::uint32_t SizeInBits = 0;
    AlignmentSpec Alignment;
    std::string AddressSpaceName;
  };

  struct TypeLayout final {
    std::uint64_t StoreSize = 0;
    std::uint64_t AllocationSize = 0;
    std::uint64_t ABIAlignment = 1;
    std::uint64_t PreferredAlignment = 1;
  };

  [[nodiscard]] static Expected<std::uint64_t> parseUnsignedField(std::string_view Text,
                                                                  std::string_view Description);
  [[nodiscard]] static Expected<std::uint64_t> alignmentBytes(std::string_view Text, bool AllowZero,
                                                              std::string_view Description);
  [[nodiscard]] static std::vector<std::string_view> split(std::string_view Text, char Separator);

  void setPrimitiveSpec(PrimitiveSpec Entry);
  void setPointerSpec(PointerSpec Entry);
  [[nodiscard]] AlignmentSpec naturalAlignment(std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] AlignmentSpec scalarAlignment(char Kind, std::uint32_t BitWidth,
                                              std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] AlignmentSpec integerAlignment(std::uint32_t BitWidth,
                                               std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] AlignmentSpec floatingAlignment(std::uint32_t BitWidth,
                                                std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] AlignmentSpec vectorAlignment(std::uint32_t BitWidth,
                                              std::uint64_t StoreSize) const noexcept;
  [[nodiscard]] Expected<PointerSpec> getPointerSpec(std::uint32_t AddressSpace) const;
  [[nodiscard]] Expected<TypeLayout> getTypeLayout(const GenericConstantType &Type,
                                                   std::vector<std::string> &NamedTypeStack,
                                                   std::size_t Depth) const;

  bool Explicit = false;
  std::vector<PrimitiveSpec> PrimitiveSpecs;
  std::vector<PointerSpec> PointerSpecs;
  AlignmentSpec AggregateAlignment{1, 8};
};

} // namespace rcc

#endif // RCC_IR_DATALAYOUT_H
