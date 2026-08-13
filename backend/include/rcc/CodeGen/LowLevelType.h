// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LOWLEVELTYPE_H
#define RCC_CODEGEN_LOWLEVELTYPE_H

#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace rcc {

class LowLevelType final {
public:
  enum class Kind : std::uint8_t {
    Invalid,
    Token,
    CategoryNeutralScalar,
    Pointer,
    Vector,
    VectorPointer,
  };

  static constexpr std::uint32_t MaxBitWidth = (std::uint32_t{1} << 16) - 1;
  static constexpr std::uint32_t MaxElementCount = (std::uint32_t{1} << 16) - 1;
  static constexpr std::uint32_t MaxAddressSpace = (std::uint32_t{1} << 24) - 1;

  constexpr LowLevelType() noexcept = default;

  [[nodiscard]] static constexpr LowLevelType token() noexcept {
    return LowLevelType(encode(Kind::Token, 0, 0, 0, false));
  }

  [[nodiscard]] static Expected<LowLevelType> makeCategoryNeutralScalar(std::uint64_t BitWidth);
  [[nodiscard]] static Expected<LowLevelType> makePointer(std::uint64_t AddressSpace,
                                                          std::uint64_t BitWidth);
  [[nodiscard]] static Expected<LowLevelType> makeFixedVector(std::uint64_t ElementCount,
                                                              LowLevelType ElementType);
  [[nodiscard]] static Expected<LowLevelType> makeScalableVector(std::uint64_t MinimumElementCount,
                                                                 LowLevelType ElementType);

  [[nodiscard]] constexpr Kind kind() const noexcept {
    return static_cast<Kind>((RawValue >> KindShift) & KindMask);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return kind() != Kind::Invalid;
  }

  [[nodiscard]] constexpr bool isToken() const noexcept {
    return kind() == Kind::Token;
  }

  [[nodiscard]] constexpr bool isCategoryNeutralScalar() const noexcept {
    return kind() == Kind::CategoryNeutralScalar;
  }

  [[nodiscard]] constexpr bool isPointer() const noexcept {
    return kind() == Kind::Pointer;
  }

  [[nodiscard]] constexpr bool isVectorValue() const noexcept {
    return kind() == Kind::Vector;
  }

  [[nodiscard]] constexpr bool isPointerVector() const noexcept {
    return kind() == Kind::VectorPointer;
  }

  [[nodiscard]] constexpr bool isPointerOrPointerVector() const noexcept {
    return isPointer() || isPointerVector();
  }

  [[nodiscard]] constexpr bool isVector() const noexcept {
    return kind() == Kind::Vector || kind() == Kind::VectorPointer;
  }

  [[nodiscard]] constexpr bool isScalableVector() const noexcept {
    return isVector() && ((RawValue >> ScalableShift) & std::uint64_t{1}) != 0;
  }

  [[nodiscard]] constexpr bool isScalarValue() const noexcept {
    return isCategoryNeutralScalar() || isPointer();
  }

  [[nodiscard]] constexpr std::uint32_t scalarBitWidth() const noexcept {
    return static_cast<std::uint32_t>((RawValue >> BitWidthShift) & BitWidthMask);
  }

  [[nodiscard]] constexpr std::uint32_t minimumElementCount() const noexcept {
    if (!isVector())
      return 0;
    return static_cast<std::uint32_t>((RawValue >> ElementCountShift) & ElementCountMask);
  }

  [[nodiscard]] constexpr std::uint64_t minimumBitWidth() const noexcept {
    return isVector() ? static_cast<std::uint64_t>(scalarBitWidth()) * minimumElementCount()
                      : scalarBitWidth();
  }

  [[nodiscard]] std::optional<std::uint32_t> addressSpace() const noexcept;
  [[nodiscard]] std::optional<LowLevelType> scalarElementType() const noexcept;

  [[nodiscard]] constexpr std::uint64_t rawValue() const noexcept {
    return RawValue;
  }

  [[nodiscard]] std::string toString() const;

  friend constexpr bool operator==(LowLevelType Left, LowLevelType Right) noexcept = default;

private:
  static constexpr unsigned BitWidthShift = 0;
  static constexpr unsigned ElementCountShift = 16;
  static constexpr unsigned AuxiliaryShift = 32;
  static constexpr unsigned ScalableShift = 56;
  static constexpr unsigned KindShift = 60;

  static constexpr std::uint64_t BitWidthMask = (std::uint64_t{1} << 16) - 1;
  static constexpr std::uint64_t ElementCountMask = (std::uint64_t{1} << 16) - 1;
  static constexpr std::uint64_t AuxiliaryMask = (std::uint64_t{1} << 24) - 1;
  static constexpr std::uint64_t KindMask = (std::uint64_t{1} << 4) - 1;

  explicit constexpr LowLevelType(std::uint64_t RawValue) noexcept : RawValue(RawValue) {}

  [[nodiscard]] static constexpr std::uint64_t encode(Kind TypeKind, std::uint32_t BitWidth,
                                                      std::uint32_t ElementCount,
                                                      std::uint32_t Auxiliary,
                                                      bool IsScalable) noexcept {
    return (static_cast<std::uint64_t>(TypeKind) << KindShift) |
           (static_cast<std::uint64_t>(IsScalable) << ScalableShift) |
           (static_cast<std::uint64_t>(Auxiliary) << AuxiliaryShift) |
           (static_cast<std::uint64_t>(ElementCount) << ElementCountShift) |
           (static_cast<std::uint64_t>(BitWidth) << BitWidthShift);
  }

  [[nodiscard]] static Expected<LowLevelType> makeVector(std::uint64_t ElementCount,
                                                         bool IsScalable, LowLevelType ElementType);

  [[nodiscard]] constexpr std::uint32_t auxiliaryValue() const noexcept {
    return static_cast<std::uint32_t>((RawValue >> AuxiliaryShift) & AuxiliaryMask);
  }

  std::uint64_t RawValue = 0;
};

static_assert(sizeof(LowLevelType) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<LowLevelType>);

} // namespace rcc

template <> struct std::hash<rcc::LowLevelType> {
  [[nodiscard]] std::size_t operator()(rcc::LowLevelType Type) const noexcept {
    return std::hash<std::uint64_t>{}(Type.rawValue());
  }
};

#endif // RCC_CODEGEN_LOWLEVELTYPE_H
