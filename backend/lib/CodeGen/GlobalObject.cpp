// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalObject.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] bool validText(const std::string &Text) noexcept {
  return !Text.empty() && Text.find('\0') == std::string::npos &&
         Text.find('\n') == std::string::npos && Text.find('\r') == std::string::npos;
}

[[nodiscard]] bool validValueSize(std::uint8_t Size) noexcept {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}

[[nodiscard]] Expected<std::uint64_t> addSize(std::uint64_t Left, std::uint64_t Right) {
  std::uint64_t Result = 0;
  if (__builtin_add_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::VerificationError, "global initializer size overflows");
  return Result;
}

[[nodiscard]] Expected<std::uint64_t> multiplySize(std::uint64_t Left, std::uint64_t Right) {
  std::uint64_t Result = 0;
  if (__builtin_mul_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::VerificationError, "global initializer size overflows");
  return Result;
}

[[nodiscard]] Expected<std::uint64_t> initializerSize(const GlobalInitializer &Initializer,
                                                      std::size_t Depth, std::size_t MaximumDepth) {
  if (Depth > MaximumDepth)
    return makeUnexpected(ErrorCode::VerificationError, "global initializer is too deeply nested");
  switch (Initializer.kind()) {
  case GlobalInitializer::Kind::Invalid:
    return makeUnexpected(ErrorCode::VerificationError, "global initializer is invalid");
  case GlobalInitializer::Kind::Integer: {
    const auto *Value = Initializer.getIf<IntegerImmediate>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "integer initializer storage is invalid");
    auto Verified = Value->verify();
    if (!Verified)
      return std::unexpected(Verified.error());
    return (static_cast<std::uint64_t>(Value->bitWidth()) + 7U) / 8U;
  }
  case GlobalInitializer::Kind::Floating: {
    const auto *Value = Initializer.getIf<FloatingPointBits>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "floating initializer storage is invalid");
    return (static_cast<std::uint64_t>(Value->bitWidth()) + 7U) / 8U;
  }
  case GlobalInitializer::Kind::Bytes: {
    const auto *Value = Initializer.getIf<std::vector<std::byte>>();
    if (Value == nullptr || Value->empty())
      return makeUnexpected(ErrorCode::VerificationError, "byte initializer cannot be empty");
    return static_cast<std::uint64_t>(Value->size());
  }
  case GlobalInitializer::Kind::ZeroFill: {
    const auto *Value = Initializer.getIf<GlobalZeroFill>();
    if (Value == nullptr || Value->Size == 0)
      return makeUnexpected(ErrorCode::VerificationError, "zero-fill initializer has zero size");
    return Value->Size;
  }
  case GlobalInitializer::Kind::Repeated: {
    const auto *Value = Initializer.getIf<GlobalRepeatedInitializer>();
    if (Value == nullptr || Value->Count == 0 || !Value->Value)
      return makeUnexpected(ErrorCode::VerificationError, "repeated initializer is invalid");
    auto ElementSize = initializerSize(*Value->Value, Depth + 1, MaximumDepth);
    if (!ElementSize)
      return std::unexpected(ElementSize.error());
    return multiplySize(*ElementSize, Value->Count);
  }
  case GlobalInitializer::Kind::Aggregate: {
    const auto *Value = Initializer.getIf<GlobalAggregateInitializer>();
    if (Value == nullptr || Value->Elements.empty())
      return makeUnexpected(ErrorCode::VerificationError, "aggregate initializer cannot be empty");
    std::uint64_t Size = 0;
    for (const GlobalInitializerRef &Element : Value->Elements) {
      if (!Element)
        return makeUnexpected(ErrorCode::VerificationError, "aggregate initializer is null");
      auto ElementSize = initializerSize(*Element, Depth + 1, MaximumDepth);
      if (!ElementSize)
        return std::unexpected(ElementSize.error());
      auto Updated = addSize(Size, *ElementSize);
      if (!Updated)
        return std::unexpected(Updated.error());
      Size = *Updated;
    }
    return Size;
  }
  case GlobalInitializer::Kind::SymbolAddress: {
    const auto *Value = Initializer.getIf<GlobalSymbolAddress>();
    if (Value == nullptr || !validText(Value->Symbol) || !validValueSize(Value->Size)) {
      return makeUnexpected(ErrorCode::VerificationError, "symbol-address initializer is invalid");
    }
    return Value->Size;
  }
  case GlobalInitializer::Kind::SymbolDifference: {
    const auto *Value = Initializer.getIf<GlobalSymbolDifference>();
    if (Value == nullptr || !validText(Value->Minuend) || !validText(Value->Subtrahend) ||
        !validValueSize(Value->Size)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "symbol-difference initializer is invalid");
    }
    return Value->Size;
  }
  case GlobalInitializer::Kind::Target: {
    const auto *Value = Initializer.getIf<TargetConstant>();
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "target initializer storage is invalid");
    auto Emission = Value->emit();
    if (!Emission)
      return std::unexpected(Emission.error());
    return static_cast<std::uint64_t>(Emission->Bytes.size());
  }
  case GlobalInitializer::Kind::AlignmentPadding: {
    const auto *Value = Initializer.getIf<GlobalAlignmentPadding>();
    if (Value == nullptr || Value->Size == 0)
      return makeUnexpected(ErrorCode::VerificationError, "alignment padding has zero size");
    return Value->Size;
  }
  case GlobalInitializer::Kind::UndefinedStorage: {
    const auto *Value = Initializer.getIf<GlobalUndefinedStorage>();
    if (Value == nullptr || Value->Size == 0)
      return makeUnexpected(ErrorCode::VerificationError, "undefined storage has zero size");
    return Value->Size;
  }
  }
  return makeUnexpected(ErrorCode::InternalError, "global initializer kind is invalid");
}

} // namespace

GlobalInitializer GlobalInitializer::integer(IntegerImmediate Value) {
  return GlobalInitializer(std::move(Value));
}

GlobalInitializer GlobalInitializer::floating(FloatingPointBits Value) {
  return GlobalInitializer(Value);
}

Expected<GlobalInitializer> GlobalInitializer::bytes(std::vector<std::byte> Value) {
  if (Value.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "byte initializer cannot be empty");
  return GlobalInitializer(std::move(Value));
}

Expected<GlobalInitializer> GlobalInitializer::zeroFill(std::uint64_t Size) {
  if (Size == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "zero-fill initializer size must be nonzero");
  return GlobalInitializer(GlobalZeroFill{.Size = Size});
}

Expected<GlobalInitializer> GlobalInitializer::repeated(std::uint64_t Count,
                                                        GlobalInitializer Value) {
  if (Count == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "initializer repeat count must be nonzero");
  auto Reference = std::make_shared<GlobalInitializer>(std::move(Value));
  auto Verification = Reference->verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return GlobalInitializer(
      GlobalRepeatedInitializer{.Count = Count, .Value = std::move(Reference)});
}

Expected<GlobalInitializer> GlobalInitializer::aggregate(std::vector<GlobalInitializer> Elements) {
  if (Elements.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "aggregate initializer cannot be empty");
  GlobalAggregateInitializer Result;
  Result.Elements.reserve(Elements.size());
  for (GlobalInitializer &Element : Elements) {
    auto Reference = std::make_shared<GlobalInitializer>(std::move(Element));
    auto Verification = Reference->verify();
    if (!Verification)
      return std::unexpected(Verification.error());
    Result.Elements.push_back(std::move(Reference));
  }
  return GlobalInitializer(std::move(Result));
}

Expected<GlobalInitializer> GlobalInitializer::symbolAddress(GlobalSymbolAddress Value) {
  GlobalInitializer Result(std::move(Value));
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<GlobalInitializer> GlobalInitializer::symbolDifference(GlobalSymbolDifference Value) {
  GlobalInitializer Result(std::move(Value));
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<GlobalInitializer> GlobalInitializer::target(TargetConstant Value) {
  GlobalInitializer Result(std::move(Value));
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<GlobalInitializer> GlobalInitializer::alignmentPadding(std::uint64_t Size,
                                                                std::uint8_t Fill) {
  if (Size == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "alignment padding size must be nonzero");
  return GlobalInitializer(GlobalAlignmentPadding{.Size = Size, .Fill = Fill});
}

Expected<GlobalInitializer> GlobalInitializer::undefinedStorage(std::uint64_t Size) {
  if (Size == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "undefined storage size must be nonzero");
  return GlobalInitializer(GlobalUndefinedStorage{.Size = Size});
}

GlobalInitializer::Kind GlobalInitializer::kind() const noexcept {
  return static_cast<Kind>(Storage.index());
}

Expected<void> GlobalInitializer::verify(std::size_t MaximumDepth) const {
  if (MaximumDepth == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "initializer depth limit cannot be zero");
  auto Size = initializerSize(*this, 1, MaximumDepth);
  if (!Size)
    return std::unexpected(Size.error());
  return {};
}

Expected<std::uint64_t> GlobalInitializer::storageSize(std::size_t MaximumDepth) const {
  if (MaximumDepth == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "initializer depth limit cannot be zero");
  return initializerSize(*this, 1, MaximumDepth);
}

Expected<void> GlobalObjectDesc::verify() const {
  if (!validText(Symbol))
    return makeUnexpected(ErrorCode::VerificationError, "global object symbol is invalid");
  auto TypeVerification = ObjectType.verify();
  if (!TypeVerification)
    return std::unexpected(TypeVerification.error());
  if (Linkage >= GlobalLinkage::Count || Visibility >= GlobalVisibility::Count ||
      TLSModel >= GlobalTLSModel::Count || UnnamedAddress >= GlobalUnnamedAddress::Count) {
    return makeUnexpected(ErrorCode::VerificationError, "global object enum field is invalid");
  }
  if (Section && !validText(*Section))
    return makeUnexpected(ErrorCode::VerificationError, "global object section is invalid");
  if (COMDAT && !validText(*COMDAT))
    return makeUnexpected(ErrorCode::VerificationError, "global object COMDAT is invalid");
  if (Size == 0 && !isDeclaration())
    return makeUnexpected(ErrorCode::VerificationError, "defined global object has zero size");
  if (!isDeclaration() &&
      Size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "global object size exceeds the supported assembly range");
  }
  if (Linkage == GlobalLinkage::Common && Initializer)
    return makeUnexpected(ErrorCode::VerificationError, "common global cannot have an initializer");
  if (Initializer) {
    auto InitializerSize = Initializer->storageSize();
    if (!InitializerSize)
      return std::unexpected(InitializerSize.error());
    if (*InitializerSize != Size) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "global initializer size does not match its object size");
    }
  }
  std::unordered_set<std::string> AttributeNames;
  for (const auto &[Name, Value] : TargetAttributes) {
    if (!validText(Name) || Value.find('\0') != std::string::npos ||
        Value.find('\n') != std::string::npos || Value.find('\r') != std::string::npos ||
        !AttributeNames.insert(Name).second) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "global target attribute is invalid or duplicated");
    }
  }
  for (const std::string &Expression : DebugExpressions) {
    if (!validText(Expression))
      return makeUnexpected(ErrorCode::VerificationError, "global debug expression is invalid");
  }
  return {};
}

} // namespace rcc
