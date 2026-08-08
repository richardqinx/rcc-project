// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GENERICCONSTANT_H
#define RCC_CODEGEN_GENERICCONSTANT_H

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rcc {

enum class GenericConstantTypeKind : std::uint8_t {
  Invalid,
  Void,
  Label,
  Token,
  Metadata,
  Integer,
  Byte,
  Floating,
  Pointer,
  Array,
  Vector,
  Struct,
  Function,
  Named,
};

struct GenericConstantType final {
  GenericConstantTypeKind Kind = GenericConstantTypeKind::Invalid;
  std::uint32_t BitWidth = 0;
  FloatSemantics Semantics = FloatSemantics::IEEEHalf;
  std::uint32_t AddressSpace = 0;
  std::uint64_t ElementCount = 0;
  bool Scalable = false;
  bool Packed = false;
  bool VarArg = false;
  bool NumericName = false;
  std::string Name;
  std::vector<GenericConstantType> Elements;

  [[nodiscard]] static Expected<GenericConstantType> integer(std::uint32_t BitWidth);
  [[nodiscard]] static GenericConstantType primitive(GenericConstantTypeKind Kind) noexcept;
  [[nodiscard]] static GenericConstantType byte() noexcept;
  [[nodiscard]] static Expected<GenericConstantType> floating(FloatSemantics Semantics);
  [[nodiscard]] static GenericConstantType pointer(std::uint32_t AddressSpace = 0) noexcept;
  [[nodiscard]] static Expected<GenericConstantType> array(std::uint64_t ElementCount,
                                                           GenericConstantType Element);
  [[nodiscard]] static Expected<GenericConstantType>
  vector(std::uint64_t ElementCount, GenericConstantType Element, bool Scalable = false);
  [[nodiscard]] static Expected<GenericConstantType>
  structure(std::vector<GenericConstantType> Elements, bool Packed = false);
  [[nodiscard]] static Expected<GenericConstantType>
  function(GenericConstantType Return, std::vector<GenericConstantType> Parameters,
           bool VarArg = false);
  [[nodiscard]] static Expected<GenericConstantType> named(std::string Name,
                                                           bool NumericName = false);

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const GenericConstantType &Left,
                         const GenericConstantType &Right) = default;
};

enum class GenericConstantKind : std::uint8_t {
  Invalid,
  Integer,
  Floating,
  NullPointer,
  ZeroInitializer,
  Undef,
  Poison,
  Aggregate,
  ByteString,
  GlobalAddress,
  BlockAddress,
  DSOLocalEquivalent,
  NoCFI,
  PointerAuth,
  Expression,
};

enum class GenericConstantExpressionOpcode : std::uint8_t {
  Invalid,
  Trunc,
  BitCast,
  AddressSpaceCast,
  IntToPtr,
  PtrToAddr,
  PtrToInt,
  Add,
  Sub,
  Xor,
  GetElementPtr,
  ShuffleVector,
  InsertElement,
  ExtractElement,
  Splat,
};

enum class GenericConstantExpressionFlag : std::uint8_t {
  NoUnsignedWrap,
  NoSignedWrap,
  InBounds,
  NoUnsignedSignedWrap,
  GEPNoUnsignedWrap,
  Count,
};

class GenericConstantExpressionFlags final {
public:
  constexpr GenericConstantExpressionFlags() noexcept = default;

  [[nodiscard]] constexpr bool has(GenericConstantExpressionFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    if (Index >= static_cast<std::uint8_t>(GenericConstantExpressionFlag::Count))
      return false;
    return (Mask & (std::uint8_t{1} << Index)) != 0;
  }

  constexpr void set(GenericConstantExpressionFlag Flag) noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    if (Index < static_cast<std::uint8_t>(GenericConstantExpressionFlag::Count))
      Mask |= std::uint8_t{1} << Index;
  }

  [[nodiscard]] constexpr std::uint8_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(GenericConstantExpressionFlags Left,
                                   GenericConstantExpressionFlags Right) noexcept = default;

private:
  std::uint8_t Mask = 0;
};

struct GenericConstantData final {
  GenericConstantType Type;
  GenericConstantKind Kind = GenericConstantKind::Invalid;
  std::optional<IntegerImmediate> IntegerValue;
  std::optional<FloatingPointBits> FloatingValue;
  std::vector<std::byte> ByteValue;
  std::vector<GenericConstantData> Elements;
  std::string Symbol;
  std::string Block;
  bool NumericSymbol = false;
  bool NumericBlock = false;
  GenericConstantExpressionOpcode ExpressionOpcode = GenericConstantExpressionOpcode::Invalid;
  GenericConstantExpressionFlags ExpressionFlags;
  std::optional<GenericConstantType> SourceElementType;
  std::optional<IntegerImmediate> InRangeStart;
  std::optional<IntegerImmediate> InRangeEnd;

  [[nodiscard]] static Expected<GenericConstantData> integer(IntegerImmediate Value);
  [[nodiscard]] static Expected<GenericConstantData> floating(FloatingPointBits Value);
  [[nodiscard]] static Expected<GenericConstantData> byteString(GenericConstantType Type,
                                                                std::vector<std::byte> Bytes);

  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] bool referencesSymbol() const noexcept;

  friend bool operator==(const GenericConstantData &Left,
                         const GenericConstantData &Right) = default;
};

} // namespace rcc

#endif // RCC_CODEGEN_GENERICCONSTANT_H
