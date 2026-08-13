// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LowLevelType.h"

#include <array>
#include <string>

namespace rcc {
namespace {

[[nodiscard]] LowLevelType::Kind getVectorKind(LowLevelType ElementType) noexcept {
  switch (ElementType.kind()) {
  case LowLevelType::Kind::CategoryNeutralScalar:
    return LowLevelType::Kind::Vector;
  case LowLevelType::Kind::Pointer:
    return LowLevelType::Kind::VectorPointer;
  case LowLevelType::Kind::Invalid:
  case LowLevelType::Kind::Token:
  case LowLevelType::Kind::Vector:
  case LowLevelType::Kind::VectorPointer:
    return LowLevelType::Kind::Invalid;
  }

  return LowLevelType::Kind::Invalid;
}

} // namespace

Expected<LowLevelType> LowLevelType::makeCategoryNeutralScalar(std::uint64_t BitWidth) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LowLevelType bit width must be in the range [1, 65535]");
  }

  return LowLevelType(
      encode(Kind::CategoryNeutralScalar, static_cast<std::uint32_t>(BitWidth), 0, 0, false));
}

Expected<LowLevelType> LowLevelType::makePointer(std::uint64_t AddressSpace,
                                                 std::uint64_t BitWidth) {
  if (BitWidth == 0 || BitWidth > MaxBitWidth) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LowLevelType pointer bit width must be in the range [1, 65535]");
  }
  if (AddressSpace > MaxAddressSpace) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LowLevelType address space must fit in 24 bits");
  }

  return LowLevelType(encode(Kind::Pointer, static_cast<std::uint32_t>(BitWidth), 0,
                             static_cast<std::uint32_t>(AddressSpace), false));
}

Expected<LowLevelType> LowLevelType::makeFixedVector(std::uint64_t ElementCount,
                                                     LowLevelType ElementType) {
  return makeVector(ElementCount, false, ElementType);
}

Expected<LowLevelType> LowLevelType::makeScalableVector(std::uint64_t MinimumElementCount,
                                                        LowLevelType ElementType) {
  return makeVector(MinimumElementCount, true, ElementType);
}

Expected<LowLevelType> LowLevelType::makeVector(std::uint64_t ElementCount, bool IsScalable,
                                                LowLevelType ElementType) {
  if (ElementCount == 0 || ElementCount > MaxElementCount) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LowLevelType vector element count must be in the range [1, 65535]");
  }
  if (!IsScalable && ElementCount == 1) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "a fixed LowLevelType vector must contain at least two elements");
  }

  const Kind VectorKind = getVectorKind(ElementType);
  if (VectorKind == Kind::Invalid) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "LowLevelType vector elements must be non-vector value types");
  }

  std::uint32_t Auxiliary = 0;
  if (ElementType.isPointer())
    Auxiliary = *ElementType.addressSpace();

  return LowLevelType(encode(VectorKind, ElementType.scalarBitWidth(),
                             static_cast<std::uint32_t>(ElementCount), Auxiliary, IsScalable));
}

std::optional<std::uint32_t> LowLevelType::addressSpace() const noexcept {
  if (!isPointerOrPointerVector())
    return std::nullopt;
  return auxiliaryValue();
}

std::optional<LowLevelType> LowLevelType::scalarElementType() const noexcept {
  switch (kind()) {
  case Kind::Vector:
    return LowLevelType(encode(Kind::CategoryNeutralScalar, scalarBitWidth(), 0, 0, false));
  case Kind::VectorPointer:
    return LowLevelType(encode(Kind::Pointer, scalarBitWidth(), 0, auxiliaryValue(), false));
  case Kind::Invalid:
  case Kind::Token:
  case Kind::CategoryNeutralScalar:
  case Kind::Pointer:
    return std::nullopt;
  }

  return std::nullopt;
}

std::string LowLevelType::toString() const {
  if (!isValid())
    return "LLT_invalid";
  if (isToken())
    return "s0";

  std::string ScalarSpelling;
  switch (kind()) {
  case Kind::CategoryNeutralScalar:
  case Kind::Vector:
    ScalarSpelling = "s" + std::to_string(scalarBitWidth());
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
