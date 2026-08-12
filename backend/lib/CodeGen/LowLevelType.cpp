// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LowLevelType.h"

#include <array>
#include <string>

namespace rcc {
namespace {

[[nodiscard]] LLT::Kind getVectorKind(LLT ElementType) noexcept {
  switch (ElementType.kind()) {
  case LLT::Kind::AnyScalar:
    return LLT::Kind::VectorAny;
  case LLT::Kind::Integer:
    return LLT::Kind::VectorInteger;
  case LLT::Kind::Float:
    return LLT::Kind::VectorFloat;
  case LLT::Kind::Pointer:
    return LLT::Kind::VectorPointer;
  case LLT::Kind::Invalid:
  case LLT::Kind::Token:
  case LLT::Kind::VectorAny:
  case LLT::Kind::VectorInteger:
  case LLT::Kind::VectorFloat:
  case LLT::Kind::VectorPointer:
    return LLT::Kind::Invalid;
  }

  return LLT::Kind::Invalid;
}

} // namespace

Expected<LLT> LLT::makeScalar(Kind TypeKind, std::uint64_t BitWidth) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LLT bit width must be in the range [1, 65535]");
  }

  return LLT(encode(TypeKind, static_cast<std::uint32_t>(BitWidth), 0, 0, false));
}

Expected<LLT> LLT::makeAnyScalar(std::uint64_t BitWidth) {
  return makeScalar(Kind::AnyScalar, BitWidth);
}

Expected<LLT> LLT::makeInteger(std::uint64_t BitWidth) {
  return makeScalar(Kind::Integer, BitWidth);
}

Expected<LLT> LLT::makeFloat(FloatSemantics Semantics) {
  const auto BitWidth = getFloatSemanticsBitWidth(Semantics);
  if (!BitWidth) {
    return makeUnexpected(ErrorCode::InvalidArgument, "unknown floating-point semantics");
  }

  return LLT(encode(Kind::Float, *BitWidth, 0, static_cast<std::uint32_t>(Semantics), false));
}

Expected<LLT> LLT::makePointer(std::uint64_t AddressSpace, std::uint64_t BitWidth) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LLT pointer bit width must be in the range [1, 65535]");
  }
  if (AddressSpace > MaxAddressSpace) {
    return makeUnexpected(ErrorCode::InvalidArgument, "LLT address space must fit in 24 bits");
  }

  return LLT(encode(Kind::Pointer, static_cast<std::uint32_t>(BitWidth), 0,
                    static_cast<std::uint32_t>(AddressSpace), false));
}

Expected<LLT> LLT::makeFixedVector(std::uint64_t ElementCount, LLT ElementType) {
  return makeVector(ElementCount, false, ElementType);
}

Expected<LLT> LLT::makeScalableVector(std::uint64_t MinimumElementCount, LLT ElementType) {
  return makeVector(MinimumElementCount, true, ElementType);
}

Expected<LLT> LLT::makeVector(std::uint64_t ElementCount, bool IsScalable, LLT ElementType) {
  if (ElementCount == 0 || ElementCount > MaxElementCount) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LLT vector element count must be in the range [1, 65535]");
  }
  if (!IsScalable && ElementCount == 1) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "a fixed LLT vector must contain at least two elements");
  }

  const Kind VectorKind = getVectorKind(ElementType);
  if (VectorKind == Kind::Invalid) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LLT vector elements must be non-vector value types");
  }

  std::uint32_t Auxiliary = 0;
  if (ElementType.isPointer())
    Auxiliary = *ElementType.addressSpace();
  else if (ElementType.isFloat())
    Auxiliary = static_cast<std::uint32_t>(*ElementType.floatSemantics());

  return LLT(encode(VectorKind, ElementType.scalarBitWidth(),
                    static_cast<std::uint32_t>(ElementCount), Auxiliary, IsScalable));
}

std::optional<std::uint32_t> LLT::addressSpace() const noexcept {
  if (!isPointerOrPointerVector())
    return std::nullopt;
  return auxiliaryValue();
}

std::optional<FloatSemantics> LLT::floatSemantics() const noexcept {
  if (!isFloatOrFloatVector())
    return std::nullopt;
  return static_cast<FloatSemantics>(auxiliaryValue());
}

std::optional<LLT> LLT::scalarElementType() const noexcept {
  switch (kind()) {
  case Kind::VectorAny:
    return LLT(encode(Kind::AnyScalar, scalarBitWidth(), 0, 0, false));
  case Kind::VectorInteger:
    return LLT(encode(Kind::Integer, scalarBitWidth(), 0, 0, false));
  case Kind::VectorFloat:
    return LLT(encode(Kind::Float, scalarBitWidth(), 0, auxiliaryValue(), false));
  case Kind::VectorPointer:
    return LLT(encode(Kind::Pointer, scalarBitWidth(), 0, auxiliaryValue(), false));
  case Kind::Invalid:
  case Kind::Token:
  case Kind::AnyScalar:
  case Kind::Integer:
  case Kind::Float:
  case Kind::Pointer:
    return std::nullopt;
  }

  return std::nullopt;
}

std::string LLT::toString() const {
  if (!isValid())
    return "LLT_invalid";
  if (isToken())
    return "s0";

  std::string ScalarSpelling;
  switch (kind()) {
  case Kind::AnyScalar:
  case Kind::VectorAny:
    ScalarSpelling = "s" + std::to_string(scalarBitWidth());
    break;
  case Kind::Integer:
  case Kind::VectorInteger:
    ScalarSpelling = "i" + std::to_string(scalarBitWidth());
    break;
  case Kind::Float:
  case Kind::VectorFloat:
    ScalarSpelling =
        std::string(getFloatSemanticsSpelling(static_cast<FloatSemantics>(auxiliaryValue())));
    break;
  case Kind::Pointer:
  case Kind::VectorPointer:
    ScalarSpelling =
        "p" + std::to_string(auxiliaryValue()) + ":" + std::to_string(scalarBitWidth());
    break;
  case Kind::Invalid:
  case Kind::Token:
    break;
  }

  if (!isVector())
    return ScalarSpelling;

  std::string Result = "<";
  if (isScalableVector())
    Result += "vscale x ";
  Result += std::to_string(minimumElementCount());
  Result += " x ";
  Result += ScalarSpelling;
  Result += ">";
  return Result;
}

} // namespace rcc
