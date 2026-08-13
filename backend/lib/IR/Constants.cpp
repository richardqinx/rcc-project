// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/IR/Constants.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace rcc {

const GenericConstantType &GenericConstantType::resolved() const noexcept {
  constexpr std::size_t MaximumResolvedDepth = 1024;
  const GenericConstantType *Result = this;
  std::size_t Depth = 0;
  while (Result->Kind == GenericConstantTypeKind::Named && Result->Elements.size() == 1 &&
         Depth++ <= MaximumResolvedDepth) {
    Result = &Result->Elements.front();
  }
  return *Result;
}

namespace {

constexpr std::size_t MaximumVerificationDepth = 1024;
constexpr std::size_t MaximumVerificationNodes = std::size_t{1} << 24U;

[[nodiscard]] bool isAggregateType(GenericConstantTypeKind Kind) noexcept {
  return Kind == GenericConstantTypeKind::Array || Kind == GenericConstantTypeKind::Vector ||
         Kind == GenericConstantTypeKind::Struct;
}

[[nodiscard]] bool sameResolvedType(const GenericConstantType &Left,
                                    const GenericConstantType &Right) noexcept {
  return Left.resolved() == Right.resolved();
}

[[nodiscard]] bool isIntegerScalarType(const GenericConstantType &Type) noexcept {
  const GenericConstantTypeKind Kind = Type.resolved().Kind;
  return Kind == GenericConstantTypeKind::Integer || Kind == GenericConstantTypeKind::Byte;
}

[[nodiscard]] std::optional<std::uint32_t>
integerScalarBitWidth(const GenericConstantType &Type) noexcept {
  const GenericConstantType &Resolved = Type.resolved();
  if (Resolved.Kind == GenericConstantTypeKind::Integer)
    return Resolved.BitWidth;
  if (Resolved.Kind == GenericConstantTypeKind::Byte)
    return 8;
  return std::nullopt;
}

[[nodiscard]] bool isPointerScalarType(const GenericConstantType &Type) noexcept {
  return Type.resolved().Kind == GenericConstantTypeKind::Pointer;
}

[[nodiscard]] bool isFloatingScalarType(const GenericConstantType &Type) noexcept {
  return Type.resolved().Kind == GenericConstantTypeKind::Floating;
}

struct VectorShape final {
  std::uint64_t ElementCount = 0;
  bool Scalable = false;

  friend constexpr bool operator==(VectorShape Left, VectorShape Right) noexcept = default;
};

[[nodiscard]] std::optional<VectorShape> vectorShape(const GenericConstantType &Type) noexcept {
  const GenericConstantType &Resolved = Type.resolved();
  if (Resolved.Kind != GenericConstantTypeKind::Vector)
    return std::nullopt;
  return VectorShape{.ElementCount = Resolved.ElementCount, .Scalable = Resolved.Scalable};
}

[[nodiscard]] const GenericConstantType *
vectorElementType(const GenericConstantType &Type) noexcept {
  const GenericConstantType &Resolved = Type.resolved();
  if (Resolved.Kind != GenericConstantTypeKind::Vector || Resolved.Elements.size() != 1)
    return nullptr;
  return &Resolved.Elements.front();
}

[[nodiscard]] bool hasSameVectorShape(const GenericConstantType &Left,
                                      const GenericConstantType &Right) noexcept {
  const auto LeftShape = vectorShape(Left);
  const auto RightShape = vectorShape(Right);
  return LeftShape == RightShape;
}

[[nodiscard]] bool isIntegerOrIntegerVectorType(const GenericConstantType &Type) noexcept {
  if (isIntegerScalarType(Type))
    return true;
  const GenericConstantType *Element = vectorElementType(Type);
  return Element != nullptr && isIntegerScalarType(*Element);
}

[[nodiscard]] bool isPointerOrPointerVectorType(const GenericConstantType &Type) noexcept {
  if (isPointerScalarType(Type))
    return true;
  const GenericConstantType *Element = vectorElementType(Type);
  return Element != nullptr && isPointerScalarType(*Element);
}

[[nodiscard]] std::optional<std::uint32_t>
pointerAddressSpace(const GenericConstantType &Type) noexcept {
  const GenericConstantType &Resolved = Type.resolved();
  if (Resolved.Kind == GenericConstantTypeKind::Pointer)
    return Resolved.AddressSpace;
  const GenericConstantType *Element = vectorElementType(Resolved);
  if (Element == nullptr)
    return std::nullopt;
  const GenericConstantType &ResolvedElement = Element->resolved();
  if (ResolvedElement.Kind != GenericConstantTypeKind::Pointer)
    return std::nullopt;
  return ResolvedElement.AddressSpace;
}

[[nodiscard]] bool isNonPointerBitCastType(const GenericConstantType &Type) noexcept {
  if (isIntegerScalarType(Type) || isFloatingScalarType(Type))
    return true;
  const GenericConstantType *Element = vectorElementType(Type);
  return Element != nullptr && (isIntegerScalarType(*Element) || isFloatingScalarType(*Element));
}

[[nodiscard]] std::optional<std::uint64_t>
nonPointerFixedBitWidth(const GenericConstantType &Type) noexcept {
  const GenericConstantType &Resolved = Type.resolved();
  if (const auto Width = integerScalarBitWidth(Resolved))
    return *Width;
  if (Resolved.Kind == GenericConstantTypeKind::Floating)
    return getFloatSemanticsBitWidth(Resolved.Semantics);
  const GenericConstantType *Element = vectorElementType(Resolved);
  if (Element == nullptr || Resolved.Scalable)
    return std::nullopt;
  const auto ElementWidth = nonPointerFixedBitWidth(*Element);
  if (!ElementWidth || *ElementWidth == 0 ||
      Resolved.ElementCount > std::numeric_limits<std::uint64_t>::max() / *ElementWidth) {
    return std::nullopt;
  }
  return Resolved.ElementCount * *ElementWidth;
}

[[nodiscard]] bool hasIntegerCastShape(const GenericConstantType &Source,
                                       const GenericConstantType &Destination,
                                       bool RequireNarrowing) noexcept {
  const GenericConstantType &ResolvedSource = Source.resolved();
  const GenericConstantType &ResolvedDestination = Destination.resolved();
  if (const auto SourceWidth = integerScalarBitWidth(ResolvedSource)) {
    const auto DestinationWidth = integerScalarBitWidth(ResolvedDestination);
    return DestinationWidth && (RequireNarrowing ? *SourceWidth > *DestinationWidth
                                                 : *SourceWidth == *DestinationWidth);
  }
  const GenericConstantType *SourceElement = vectorElementType(ResolvedSource);
  const GenericConstantType *DestinationElement = vectorElementType(ResolvedDestination);
  if (SourceElement == nullptr || DestinationElement == nullptr ||
      !hasSameVectorShape(ResolvedSource, ResolvedDestination)) {
    return false;
  }
  return hasIntegerCastShape(*SourceElement, *DestinationElement, RequireNarrowing);
}

[[nodiscard]] bool hasPointerIntegerCastShape(const GenericConstantType &Pointer,
                                              const GenericConstantType &Integer) noexcept {
  const GenericConstantType &ResolvedPointer = Pointer.resolved();
  const GenericConstantType &ResolvedInteger = Integer.resolved();
  if (isPointerScalarType(ResolvedPointer))
    return isIntegerScalarType(ResolvedInteger);
  const GenericConstantType *PointerElement = vectorElementType(ResolvedPointer);
  const GenericConstantType *IntegerElement = vectorElementType(ResolvedInteger);
  return PointerElement != nullptr && IntegerElement != nullptr &&
         hasSameVectorShape(ResolvedPointer, ResolvedInteger) &&
         isPointerScalarType(*PointerElement) && isIntegerScalarType(*IntegerElement);
}

[[nodiscard]] bool isSizedType(const GenericConstantType &Type, std::size_t Depth = 0) noexcept {
  if (Depth > MaximumVerificationDepth)
    return false;
  const GenericConstantType &Resolved = Type.resolved();
  switch (Resolved.Kind) {
  case GenericConstantTypeKind::Integer:
  case GenericConstantTypeKind::Byte:
  case GenericConstantTypeKind::Floating:
  case GenericConstantTypeKind::Pointer:
    return true;
  case GenericConstantTypeKind::Array:
  case GenericConstantTypeKind::Vector:
    return Resolved.Elements.size() == 1 && isSizedType(Resolved.Elements.front(), Depth + 1);
  case GenericConstantTypeKind::Struct:
    return std::ranges::all_of(Resolved.Elements, [Depth](const GenericConstantType &Element) {
      return isSizedType(Element, Depth + 1);
    });
  case GenericConstantTypeKind::Named:
    return Resolved.Elements.size() == 1 && isSizedType(Resolved.Elements.front(), Depth + 1);
  case GenericConstantTypeKind::Invalid:
  case GenericConstantTypeKind::Void:
  case GenericConstantTypeKind::Label:
  case GenericConstantTypeKind::Token:
  case GenericConstantTypeKind::Metadata:
  case GenericConstantTypeKind::Function:
    return false;
  }
  return false;
}

[[nodiscard]] bool signedIntegerLess(const APInt &Left, const APInt &Right) noexcept {
  if (Left.bitWidth() != Right.bitWidth())
    return false;
  if (Left.isNegative() != Right.isNegative())
    return Left.isNegative();
  const auto LeftWords = Left.words();
  const auto RightWords = Right.words();
  for (std::size_t Index = LeftWords.size(); Index != 0; --Index) {
    if (LeftWords[Index - 1U] != RightWords[Index - 1U])
      return LeftWords[Index - 1U] < RightWords[Index - 1U];
  }
  return false;
}

[[nodiscard]] bool hasOnlyValuePayload(const GenericConstantData &Value) noexcept {
  return Value.Symbol.empty() && Value.Block.empty() &&
         Value.ExpressionOpcode == GenericConstantExpressionOpcode::Invalid &&
         Value.ExpressionFlags.rawValue() == 0 && !Value.SourceElementType && !Value.InRangeStart &&
         !Value.InRangeEnd;
}

[[nodiscard]] Expected<void> verifyType(const GenericConstantType &Type, std::size_t Depth,
                                        std::size_t &Nodes) {
  if (Depth > MaximumVerificationDepth || ++Nodes > MaximumVerificationNodes) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generic constant type exceeds structural limits");
  }

  const auto RequireNoElements = [&]() -> Expected<void> {
    if (!Type.Elements.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "scalar generic constant type cannot contain child types");
    }
    return {};
  };

  switch (Type.Kind) {
  case GenericConstantTypeKind::Invalid:
    return makeUnexpected(ErrorCode::VerificationError, "generic constant type is invalid");
  case GenericConstantTypeKind::Void:
  case GenericConstantTypeKind::Label:
  case GenericConstantTypeKind::Token:
  case GenericConstantTypeKind::Metadata:
    return RequireNoElements();
  case GenericConstantTypeKind::Integer:
    if (Type.BitWidth == 0 || Type.BitWidth > APInt::MaxBitWidth) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic integer type has an invalid bit width");
    }
    return RequireNoElements();
  case GenericConstantTypeKind::Byte:
    return RequireNoElements();
  case GenericConstantTypeKind::Floating:
    if (!getFloatSemanticsBitWidth(Type.Semantics)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic floating type has unknown semantics");
    }
    return RequireNoElements();
  case GenericConstantTypeKind::Pointer:
    return RequireNoElements();
  case GenericConstantTypeKind::Array:
  case GenericConstantTypeKind::Vector:
    if (Type.Elements.size() != 1 ||
        (Type.Kind == GenericConstantTypeKind::Vector && Type.ElementCount == 0)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic sequential type has an invalid element description");
    }
    return verifyType(Type.Elements.front(), Depth + 1, Nodes);
  case GenericConstantTypeKind::Struct:
    for (const GenericConstantType &Element : Type.Elements) {
      auto Result = verifyType(Element, Depth + 1, Nodes);
      if (!Result)
        return Result;
    }
    return {};
  case GenericConstantTypeKind::Function:
    if (Type.Elements.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic function type has no return type");
    }
    for (const GenericConstantType &Element : Type.Elements) {
      auto Result = verifyType(Element, Depth + 1, Nodes);
      if (!Result)
        return Result;
    }
    return {};
  case GenericConstantTypeKind::Named:
    if (Type.Name.empty() || Type.Elements.size() > 1) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic named type has an invalid identity");
    }
    if (!Type.Elements.empty())
      return verifyType(Type.Elements.front(), Depth + 1, Nodes);
    return {};
  }
  return makeUnexpected(ErrorCode::InternalError, "generic constant type kind is unhandled");
}

[[nodiscard]] Expected<void> verifyConstant(const GenericConstantData &Value, std::size_t Depth,
                                            std::size_t &Nodes) {
  if (Depth > MaximumVerificationDepth || ++Nodes > MaximumVerificationNodes) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "generic constant value exceeds structural limits");
  }
  std::size_t TypeNodes = 0;
  auto TypeResult = verifyType(Value.Type, 0, TypeNodes);
  if (!TypeResult)
    return TypeResult;
  const GenericConstantType &EffectiveType = Value.Type.resolved();

  const auto RequireEmptyScalarPayload = [&]() -> Expected<void> {
    if (Value.IntegerValue || Value.FloatingValue || !Value.ByteValue.empty() ||
        !Value.Elements.empty() || !hasOnlyValuePayload(Value)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic constant kind contains an unrelated payload");
    }
    return {};
  };

  switch (Value.Kind) {
  case GenericConstantKind::Invalid:
    return makeUnexpected(ErrorCode::VerificationError, "generic constant value is invalid");
  case GenericConstantKind::Integer:
    if (!Value.IntegerValue || Value.FloatingValue || !Value.ByteValue.empty() ||
        !Value.Elements.empty() || !hasOnlyValuePayload(Value) ||
        (EffectiveType.Kind != GenericConstantTypeKind::Integer &&
         EffectiveType.Kind != GenericConstantTypeKind::Byte) ||
        Value.IntegerValue->bitWidth() !=
            (EffectiveType.Kind == GenericConstantTypeKind::Byte ? 8U : EffectiveType.BitWidth)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic integer constant has an inconsistent payload");
    }
    return Value.IntegerValue->verify();
  case GenericConstantKind::Floating:
    if (!Value.FloatingValue || Value.IntegerValue || !Value.ByteValue.empty() ||
        !Value.Elements.empty() || !hasOnlyValuePayload(Value) ||
        EffectiveType.Kind != GenericConstantTypeKind::Floating ||
        Value.FloatingValue->semantics() != EffectiveType.Semantics) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic floating constant has an inconsistent payload");
    }
    return {};
  case GenericConstantKind::NullPointer:
    if (EffectiveType.Kind != GenericConstantTypeKind::Pointer) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic null constant must have pointer type");
    }
    return RequireEmptyScalarPayload();
  case GenericConstantKind::ZeroInitializer:
  case GenericConstantKind::Undef:
  case GenericConstantKind::Poison:
    if (EffectiveType.Kind == GenericConstantTypeKind::Invalid ||
        EffectiveType.Kind == GenericConstantTypeKind::Function) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic marker constant has an invalid type");
    }
    return RequireEmptyScalarPayload();
  case GenericConstantKind::Aggregate: {
    if (!isAggregateType(EffectiveType.Kind) || Value.IntegerValue || Value.FloatingValue ||
        !Value.ByteValue.empty() || !hasOnlyValuePayload(Value)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic aggregate constant has an inconsistent payload");
    }
    if ((EffectiveType.Kind == GenericConstantTypeKind::Array ||
         EffectiveType.Kind == GenericConstantTypeKind::Vector) &&
        Value.Elements.size() != EffectiveType.ElementCount) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic aggregate constant has the wrong element count");
    }
    if (EffectiveType.Kind == GenericConstantTypeKind::Struct &&
        Value.Elements.size() != EffectiveType.Elements.size()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic struct constant has the wrong element count");
    }
    for (std::size_t Index = 0; Index < Value.Elements.size(); ++Index) {
      const GenericConstantType &ExpectedType =
          EffectiveType.Kind == GenericConstantTypeKind::Struct ? EffectiveType.Elements[Index]
                                                                : EffectiveType.Elements.front();
      if (Value.Elements[Index].Type != ExpectedType) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic aggregate element type does not match its container");
      }
      auto Result = verifyConstant(Value.Elements[Index], Depth + 1, Nodes);
      if (!Result)
        return Result;
    }
    return {};
  }
  case GenericConstantKind::ByteString:
    if (EffectiveType.Kind != GenericConstantTypeKind::Array ||
        EffectiveType.Elements.size() != 1 ||
        (EffectiveType.Elements.front().Kind != GenericConstantTypeKind::Integer &&
         EffectiveType.Elements.front().Kind != GenericConstantTypeKind::Byte) ||
        (EffectiveType.Elements.front().Kind == GenericConstantTypeKind::Integer &&
         EffectiveType.Elements.front().BitWidth != 8) ||
        EffectiveType.ElementCount != Value.ByteValue.size() || Value.IntegerValue ||
        Value.FloatingValue || !Value.Elements.empty() || !hasOnlyValuePayload(Value)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic byte-string constant has an inconsistent type or payload");
    }
    return {};
  case GenericConstantKind::GlobalAddress:
  case GenericConstantKind::DSOLocalEquivalent:
  case GenericConstantKind::NoCFI:
    if (EffectiveType.Kind != GenericConstantTypeKind::Pointer || Value.Symbol.empty() ||
        Value.IntegerValue || Value.FloatingValue || !Value.ByteValue.empty() ||
        !Value.Elements.empty() || !Value.Block.empty() ||
        Value.ExpressionOpcode != GenericConstantExpressionOpcode::Invalid ||
        Value.ExpressionFlags.rawValue() != 0 || Value.SourceElementType || Value.InRangeStart ||
        Value.InRangeEnd) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic global-address constant has an inconsistent payload");
    }
    return {};
  case GenericConstantKind::BlockAddress:
    if (EffectiveType.Kind != GenericConstantTypeKind::Pointer || Value.Symbol.empty() ||
        Value.Block.empty() || Value.IntegerValue || Value.FloatingValue ||
        !Value.ByteValue.empty() || !Value.Elements.empty() ||
        Value.ExpressionOpcode != GenericConstantExpressionOpcode::Invalid ||
        Value.ExpressionFlags.rawValue() != 0 || Value.SourceElementType || Value.InRangeStart ||
        Value.InRangeEnd) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic block-address constant has an inconsistent payload");
    }
    return {};
  case GenericConstantKind::PointerAuth: {
    if (EffectiveType.Kind != GenericConstantTypeKind::Pointer || Value.Elements.size() < 2 ||
        Value.Elements.size() > 5 || Value.IntegerValue || Value.FloatingValue ||
        !Value.ByteValue.empty() || !hasOnlyValuePayload(Value)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic pointer-auth constant has an inconsistent payload");
    }
    for (const GenericConstantData &Element : Value.Elements) {
      auto Result = verifyConstant(Element, Depth + 1, Nodes);
      if (!Result)
        return Result;
    }
    const auto KeyBitWidth = integerScalarBitWidth(Value.Elements[1].Type);
    if (!isPointerScalarType(Value.Elements[0].Type) ||
        !sameResolvedType(Value.Type, Value.Elements[0].Type) || !KeyBitWidth ||
        KeyBitWidth.value() != 32) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic pointer-auth constant has invalid pointer or key operands");
    }
    if (Value.Elements.size() >= 3) {
      const auto DiscriminatorBitWidth = integerScalarBitWidth(Value.Elements[2].Type);
      if (!DiscriminatorBitWidth || DiscriminatorBitWidth.value() != 64) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic pointer-auth constant integer discriminator must be a scalar i64");
      }
    }
    for (std::size_t Index = 3; Index < Value.Elements.size(); ++Index) {
      if (!isPointerScalarType(Value.Elements[Index].Type)) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic pointer-auth constant address operands must be scalar pointers");
      }
    }
    return {};
  }
  case GenericConstantKind::Expression:
    if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::Invalid ||
        Value.Elements.empty() || Value.IntegerValue || Value.FloatingValue ||
        !Value.ByteValue.empty() || !Value.Symbol.empty() || !Value.Block.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic constant expression has an inconsistent payload");
    }
    if ((Value.InRangeStart.has_value() != Value.InRangeEnd.has_value()) ||
        ((Value.InRangeStart || Value.InRangeEnd || Value.SourceElementType) &&
         Value.ExpressionOpcode != GenericConstantExpressionOpcode::GetElementPtr)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic constant expression has misplaced GEP state");
    }
    if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr &&
        !Value.SourceElementType) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic getelementptr constant has no source element type");
    }
    for (const GenericConstantData &Element : Value.Elements) {
      auto Result = verifyConstant(Element, Depth + 1, Nodes);
      if (!Result)
        return Result;
    }

    const bool IsBinary = Value.ExpressionOpcode == GenericConstantExpressionOpcode::Add ||
                          Value.ExpressionOpcode == GenericConstantExpressionOpcode::Sub ||
                          Value.ExpressionOpcode == GenericConstantExpressionOpcode::Xor;
    const bool IsGEP = Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr;
    const std::uint8_t Flags = Value.ExpressionFlags.rawValue();
    const std::uint8_t BinaryFlags =
        (std::uint8_t{1} << static_cast<std::uint8_t>(
             GenericConstantExpressionFlag::NoUnsignedWrap)) |
        (std::uint8_t{1} << static_cast<std::uint8_t>(GenericConstantExpressionFlag::NoSignedWrap));
    const std::uint8_t GEPFlags =
        (std::uint8_t{1} << static_cast<std::uint8_t>(GenericConstantExpressionFlag::InBounds)) |
        (std::uint8_t{1} << static_cast<std::uint8_t>(
             GenericConstantExpressionFlag::NoUnsignedSignedWrap)) |
        (std::uint8_t{1} << static_cast<std::uint8_t>(
             GenericConstantExpressionFlag::GEPNoUnsignedWrap));
    if ((!IsBinary && !IsGEP && Flags != 0) || (IsBinary && (Flags & ~BinaryFlags) != 0) ||
        (IsGEP && (Flags & ~GEPFlags) != 0)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic constant expression has invalid opcode flags");
    }

    const auto RequireExactOperandCount = [&Value](std::size_t Count) -> Expected<void> {
      if (Value.Elements.size() != Count) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic constant expression has the wrong operand count");
      }
      return {};
    };
    const GenericConstantType &FirstType = Value.Elements.front().Type;
    switch (Value.ExpressionOpcode) {
    case GenericConstantExpressionOpcode::Trunc:
      if (auto Count = RequireExactOperandCount(1); !Count)
        return Count;
      if (!hasIntegerCastShape(FirstType, Value.Type, true)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic trunc constant has incompatible integer types");
      }
      return {};
    case GenericConstantExpressionOpcode::BitCast: {
      if (auto Count = RequireExactOperandCount(1); !Count)
        return Count;
      const bool SourceIsPointer = isPointerOrPointerVectorType(FirstType);
      const bool DestinationIsPointer = isPointerOrPointerVectorType(Value.Type);
      if (SourceIsPointer || DestinationIsPointer) {
        const auto SourceAddressSpace = pointerAddressSpace(FirstType);
        const auto DestinationAddressSpace = pointerAddressSpace(Value.Type);
        if (!SourceIsPointer || !DestinationIsPointer || !SourceAddressSpace ||
            !DestinationAddressSpace || *SourceAddressSpace != *DestinationAddressSpace ||
            !hasSameVectorShape(FirstType, Value.Type)) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "generic bitcast constant has incompatible pointer types");
        }
        return {};
      }
      const auto SourceWidth = nonPointerFixedBitWidth(FirstType);
      const auto DestinationWidth = nonPointerFixedBitWidth(Value.Type);
      if (!isNonPointerBitCastType(FirstType) || !isNonPointerBitCastType(Value.Type) ||
          !SourceWidth || !DestinationWidth || *SourceWidth != *DestinationWidth ||
          !hasSameVectorShape(FirstType, Value.Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic bitcast constant has incompatible fixed-width types");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::AddressSpaceCast: {
      if (auto Count = RequireExactOperandCount(1); !Count)
        return Count;
      const auto SourceAddressSpace = pointerAddressSpace(FirstType);
      const auto DestinationAddressSpace = pointerAddressSpace(Value.Type);
      if (!SourceAddressSpace || !DestinationAddressSpace ||
          *SourceAddressSpace == *DestinationAddressSpace ||
          !hasSameVectorShape(FirstType, Value.Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic addrspacecast constant has incompatible pointer types");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::IntToPtr:
      if (auto Count = RequireExactOperandCount(1); !Count)
        return Count;
      if (!hasPointerIntegerCastShape(Value.Type, FirstType)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic inttoptr constant has incompatible operand types");
      }
      return {};
    case GenericConstantExpressionOpcode::PtrToAddr:
    case GenericConstantExpressionOpcode::PtrToInt:
      if (auto Count = RequireExactOperandCount(1); !Count)
        return Count;
      if (!hasPointerIntegerCastShape(FirstType, Value.Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic pointer-to-integer constant has incompatible operand types");
      }
      return {};
    case GenericConstantExpressionOpcode::Add:
    case GenericConstantExpressionOpcode::Sub:
    case GenericConstantExpressionOpcode::Xor:
      if (auto Count = RequireExactOperandCount(2); !Count)
        return Count;
      if (!isIntegerOrIntegerVectorType(Value.Type) ||
          !sameResolvedType(Value.Elements[0].Type, Value.Type) ||
          !sameResolvedType(Value.Elements[1].Type, Value.Type)) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic binary constant requires equal integer operand and result types");
      }
      return {};
    case GenericConstantExpressionOpcode::GetElementPtr: {
      if (!Value.SourceElementType || !isSizedType(*Value.SourceElementType) ||
          !isPointerOrPointerVectorType(FirstType) || !isPointerOrPointerVectorType(Value.Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic getelementptr constant has invalid source or pointer types");
      }
      const auto BaseAddressSpace = pointerAddressSpace(FirstType);
      const auto ResultAddressSpace = pointerAddressSpace(Value.Type);
      if (!BaseAddressSpace || !ResultAddressSpace || *BaseAddressSpace != *ResultAddressSpace) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic getelementptr constant changes pointer address space");
      }
      std::optional<VectorShape> RequiredResultShape = vectorShape(FirstType);
      for (std::size_t Index = 1; Index < Value.Elements.size(); ++Index) {
        const GenericConstantType &IndexType = Value.Elements[Index].Type;
        if (!isIntegerOrIntegerVectorType(IndexType)) {
          return makeUnexpected(ErrorCode::VerificationError,
                                "generic getelementptr index must be an integer or integer vector");
        }
        if (const auto IndexShape = vectorShape(IndexType)) {
          if (RequiredResultShape && *RequiredResultShape != *IndexShape) {
            return makeUnexpected(
                ErrorCode::VerificationError,
                "generic getelementptr vector index has an incompatible element count");
          }
          RequiredResultShape = *IndexShape;
        }
      }
      if (RequiredResultShape != vectorShape(Value.Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic getelementptr result has an incompatible vector shape");
      }
      if (Value.InRangeStart && (Value.InRangeStart->bitWidth() != Value.InRangeEnd->bitWidth() ||
                                 !signedIntegerLess(*Value.InRangeStart, *Value.InRangeEnd))) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic getelementptr inrange interval is invalid");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::ShuffleVector: {
      if (auto Count = RequireExactOperandCount(3); !Count)
        return Count;
      const GenericConstantType *InputElement = vectorElementType(FirstType);
      const GenericConstantType *MaskElement = vectorElementType(Value.Elements[2].Type);
      const GenericConstantType *ResultElement = vectorElementType(Value.Type);
      const auto InputShape = vectorShape(FirstType);
      const auto MaskShape = vectorShape(Value.Elements[2].Type);
      const auto ResultShape = vectorShape(Value.Type);
      if (InputElement == nullptr || MaskElement == nullptr || ResultElement == nullptr ||
          !InputShape || !MaskShape || !ResultShape ||
          !sameResolvedType(FirstType, Value.Elements[1].Type) ||
          !integerScalarBitWidth(*MaskElement) || *integerScalarBitWidth(*MaskElement) != 32 ||
          InputShape->Scalable != MaskShape->Scalable ||
          !sameResolvedType(*InputElement, *ResultElement) ||
          ResultShape->ElementCount != MaskShape->ElementCount ||
          ResultShape->Scalable != MaskShape->Scalable) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic shufflevector constant has incompatible operand types");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::InsertElement: {
      if (auto Count = RequireExactOperandCount(3); !Count)
        return Count;
      const GenericConstantType *Element = vectorElementType(FirstType);
      if (Element == nullptr || !sameResolvedType(Value.Type, FirstType) ||
          !sameResolvedType(Value.Elements[1].Type, *Element) ||
          !isIntegerScalarType(Value.Elements[2].Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic insertelement constant has incompatible operand types");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::ExtractElement: {
      if (auto Count = RequireExactOperandCount(2); !Count)
        return Count;
      const GenericConstantType *Element = vectorElementType(FirstType);
      if (Element == nullptr || !sameResolvedType(Value.Type, *Element) ||
          !isIntegerScalarType(Value.Elements[1].Type)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic extractelement constant has incompatible operand types");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::Splat: {
      if (auto Count = RequireExactOperandCount(1); !Count)
        return Count;
      const GenericConstantType *Element = vectorElementType(Value.Type);
      if (Element == nullptr || !sameResolvedType(FirstType, *Element)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "generic splat constant has incompatible operand types");
      }
      return {};
    }
    case GenericConstantExpressionOpcode::Invalid:
      break;
    }
    return makeUnexpected(ErrorCode::InternalError,
                          "generic constant expression opcode is unhandled");
  }
  return makeUnexpected(ErrorCode::InternalError, "generic constant value kind is unhandled");
}

} // namespace

Expected<GenericConstantType> GenericConstantType::integer(std::uint32_t Width) {
  if (Width == 0 || Width > APInt::MaxBitWidth)
    return makeUnexpected(ErrorCode::InvalidArgument, "generic integer type width is invalid");
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Integer;
  Type.BitWidth = Width;
  return Type;
}

GenericConstantType GenericConstantType::primitive(GenericConstantTypeKind PrimitiveKind) noexcept {
  GenericConstantType Type;
  Type.Kind = PrimitiveKind;
  return Type;
}

GenericConstantType GenericConstantType::byte() noexcept {
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Byte;
  Type.BitWidth = 8;
  return Type;
}

Expected<GenericConstantType> GenericConstantType::floating(FloatSemantics ValueSemantics) {
  if (!getFloatSemanticsBitWidth(ValueSemantics))
    return makeUnexpected(ErrorCode::InvalidArgument, "generic floating semantics is unknown");
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Floating;
  Type.Semantics = ValueSemantics;
  Type.BitWidth = getFloatSemanticsBitWidth(ValueSemantics).value_or(0);
  return Type;
}

GenericConstantType GenericConstantType::pointer(std::uint32_t PointerAddressSpace) noexcept {
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Pointer;
  Type.AddressSpace = PointerAddressSpace;
  return Type;
}

Expected<GenericConstantType> GenericConstantType::array(std::uint64_t Count,
                                                         GenericConstantType Element) {
  auto Verification = Element.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Array;
  Type.ElementCount = Count;
  Type.Elements.push_back(std::move(Element));
  return Type;
}

Expected<GenericConstantType>
GenericConstantType::vector(std::uint64_t Count, GenericConstantType Element, bool IsScalable) {
  if (Count == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "generic vector type cannot be empty");
  auto Verification = Element.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Vector;
  Type.ElementCount = Count;
  Type.Scalable = IsScalable;
  Type.Elements.push_back(std::move(Element));
  return Type;
}

Expected<GenericConstantType>
GenericConstantType::structure(std::vector<GenericConstantType> MemberTypes, bool IsPacked) {
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Struct;
  Type.Packed = IsPacked;
  Type.Elements = std::move(MemberTypes);
  auto Verification = Type.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Type;
}

Expected<GenericConstantType>
GenericConstantType::function(GenericConstantType Return,
                              std::vector<GenericConstantType> Parameters, bool IsVarArg) {
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Function;
  Type.VarArg = IsVarArg;
  Type.Elements.reserve(Parameters.size() + 1);
  Type.Elements.push_back(std::move(Return));
  for (GenericConstantType &Parameter : Parameters)
    Type.Elements.push_back(std::move(Parameter));
  auto Verification = Type.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Type;
}

Expected<GenericConstantType> GenericConstantType::named(std::string TypeName, bool IsNumericName) {
  if (TypeName.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "generic named type cannot be empty");
  GenericConstantType Type;
  Type.Kind = GenericConstantTypeKind::Named;
  Type.Name = std::move(TypeName);
  Type.NumericName = IsNumericName;
  return Type;
}

Expected<void> GenericConstantType::verify() const {
  std::size_t Nodes = 0;
  return verifyType(*this, 0, Nodes);
}

Expected<GenericConstantData> GenericConstantData::integer(APInt Value) {
  auto Type = GenericConstantType::integer(Value.bitWidth());
  if (!Type)
    return std::unexpected(Type.error());
  GenericConstantData Result;
  Result.Type = std::move(*Type);
  Result.Kind = GenericConstantKind::Integer;
  Result.IntegerValue = std::move(Value);
  return Result;
}

Expected<GenericConstantData> GenericConstantData::floating(APFloat Value) {
  auto Type = GenericConstantType::floating(Value.semantics());
  if (!Type)
    return std::unexpected(Type.error());
  GenericConstantData Result;
  Result.Type = std::move(*Type);
  Result.Kind = GenericConstantKind::Floating;
  Result.FloatingValue = Value;
  return Result;
}

Expected<GenericConstantData> GenericConstantData::byteString(GenericConstantType ValueType,
                                                              std::vector<std::byte> Bytes) {
  GenericConstantData Result;
  Result.Type = std::move(ValueType);
  Result.Kind = GenericConstantKind::ByteString;
  Result.ByteValue = std::move(Bytes);
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<void> GenericConstantData::verify() const {
  std::size_t Nodes = 0;
  return verifyConstant(*this, 0, Nodes);
}

bool GenericConstantData::referencesSymbol() const noexcept {
  switch (Kind) {
  case GenericConstantKind::GlobalAddress:
  case GenericConstantKind::BlockAddress:
  case GenericConstantKind::DSOLocalEquivalent:
  case GenericConstantKind::NoCFI:
    return true;
  default:
    break;
  }
  for (const GenericConstantData &Element : Elements) {
    if (Element.referencesSymbol())
      return true;
  }
  return false;
}

} // namespace rcc
