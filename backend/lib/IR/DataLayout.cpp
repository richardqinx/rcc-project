// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/IR/DataLayout.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {
namespace {

constexpr std::uint64_t MaximumDataLayoutSize = (std::uint64_t{1} << 24U) - 1U;
constexpr std::uint64_t MaximumDataLayoutAlignment = (std::uint64_t{1} << 16U) - 1U;
constexpr std::size_t MaximumDataLayoutStringLength = std::size_t{1} << 24U;
constexpr std::size_t MaximumTypeLayoutDepth = 1024;

[[nodiscard]] Expected<std::uint64_t> checkedMultiply(std::uint64_t Left, std::uint64_t Right,
                                                      std::string_view Description) {
  if (Left != 0 && Right > std::numeric_limits<std::uint64_t>::max() / Left)
    return makeUnexpected(ErrorCode::Unsupported, std::string(Description) + " is too large");
  return Left * Right;
}

[[nodiscard]] Expected<std::uint64_t> checkedAdd(std::uint64_t Left, std::uint64_t Right,
                                                 std::string_view Description) {
  if (Right > std::numeric_limits<std::uint64_t>::max() - Left)
    return makeUnexpected(ErrorCode::Unsupported, std::string(Description) + " is too large");
  return Left + Right;
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t AlignmentValue) {
  if (AlignmentValue == 0 || !std::has_single_bit(AlignmentValue))
    return makeUnexpected(ErrorCode::InvalidArgument, "LLVM type alignment is invalid");
  const std::uint64_t Mask = AlignmentValue - 1U;
  if (Value > std::numeric_limits<std::uint64_t>::max() - Mask)
    return makeUnexpected(ErrorCode::Unsupported, "LLVM type allocation size is too large");
  return (Value + Mask) & ~Mask;
}

} // namespace

Expected<std::uint64_t> DataLayout::parseUnsignedField(std::string_view Text,
                                                       std::string_view Description) {
  std::uint64_t Value = 0;
  const auto Parsed = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  if (Text.empty() || Parsed.ec != std::errc{} || Parsed.ptr != Text.data() + Text.size()) {
    return makeUnexpected(ErrorCode::ParseError,
                          "LLVM data layout has an invalid " + std::string(Description));
  }
  return Value;
}

Expected<std::uint64_t> DataLayout::alignmentBytes(std::string_view Text, bool AllowZero,
                                                   std::string_view Description) {
  auto Bits = parseUnsignedField(Text, Description);
  if (!Bits)
    return std::unexpected(Bits.error());
  if (*Bits == 0 && AllowZero)
    return 1;
  if (*Bits > MaximumDataLayoutAlignment || *Bits < 8 || (*Bits % 8U) != 0 ||
      !std::has_single_bit(*Bits)) {
    return makeUnexpected(ErrorCode::ParseError,
                          "LLVM data layout " + std::string(Description) +
                              " must be a nonzero power-of-two byte alignment");
  }
  return *Bits / 8U;
}

std::vector<std::string_view> DataLayout::split(std::string_view Text, char Separator) {
  std::vector<std::string_view> Parts;
  while (true) {
    const std::size_t Position = Text.find(Separator);
    Parts.push_back(Text.substr(0, Position));
    if (Position == std::string_view::npos)
      return Parts;
    Text.remove_prefix(Position + 1U);
  }
}

void DataLayout::setPrimitiveSpec(PrimitiveSpec Entry) {
  const auto Existing =
      std::ranges::find_if(PrimitiveSpecs, [&Entry](const PrimitiveSpec &Candidate) {
        return Candidate.Kind == Entry.Kind && Candidate.BitWidth == Entry.BitWidth;
      });
  if (Existing == PrimitiveSpecs.end())
    PrimitiveSpecs.push_back(Entry);
  else
    *Existing = Entry;
}

void DataLayout::setPointerSpec(PointerSpec Entry) {
  const auto Existing = std::ranges::find_if(PointerSpecs, [&Entry](const PointerSpec &Candidate) {
    return Candidate.AddressSpace == Entry.AddressSpace;
  });
  if (Existing == PointerSpecs.end())
    PointerSpecs.push_back(Entry);
  else
    *Existing = Entry;
}

Expected<DataLayout> DataLayout::parse(std::string_view LayoutString) {
  DataLayout Result;
  if (LayoutString.empty())
    return Result;
  if (LayoutString.size() > MaximumDataLayoutStringLength) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "LLVM data layout exceeds the maximum string length");
  }
  Result.Explicit = true;
  Result.setPointerSpec({.AddressSpace = 0,
                         .SizeInBits = 64,
                         .Alignment = {.ABI = 8, .Preferred = 8},
                         .AddressSpaceName = {}});
  for (const PrimitiveSpec Entry : {
           PrimitiveSpec{'i', 1, {1, 1}},
           PrimitiveSpec{'i', 8, {1, 1}},
           PrimitiveSpec{'i', 16, {2, 2}},
           PrimitiveSpec{'i', 32, {4, 4}},
           PrimitiveSpec{'i', 64, {4, 8}},
           PrimitiveSpec{'f', 16, {2, 2}},
           PrimitiveSpec{'f', 32, {4, 4}},
           PrimitiveSpec{'f', 64, {8, 8}},
           PrimitiveSpec{'f', 128, {16, 16}},
           PrimitiveSpec{'v', 64, {8, 8}},
           PrimitiveSpec{'v', 128, {16, 16}},
       }) {
    Result.setPrimitiveSpec(Entry);
  }

  for (const std::string_view Component : split(LayoutString, '-')) {
    if (Component.empty())
      return makeUnexpected(ErrorCode::ParseError, "LLVM data layout contains an empty component");
    if (Component.front() == 'p') {
      const std::size_t Colon = Component.find(':');
      if (Colon == std::string_view::npos)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer data layout component has no size");
      std::string_view AddressSpaceText = Component.substr(1, Colon - 1U);
      bool HasExternalState = false;
      bool HasUnstableRepresentation = false;
      bool HasExplicitNullValue = false;
      while (!AddressSpaceText.empty()) {
        const char Flag = AddressSpaceText.front();
        if (Flag == 'e') {
          HasExternalState = true;
        } else if (Flag == 'u') {
          HasUnstableRepresentation = true;
        } else if (Flag == 'z' || Flag == 'o') {
          if (HasExplicitNullValue) {
            return makeUnexpected(ErrorCode::ParseError,
                                  "LLVM pointer data layout has conflicting null-pointer flags");
          }
          HasExplicitNullValue = true;
        } else {
          break;
        }
        AddressSpaceText.remove_prefix(1);
      }

      std::uint64_t AddressSpace = 0;
      std::string AddressSpaceName;
      if (!AddressSpaceText.empty()) {
        std::size_t NumberLength = 0;
        while (NumberLength < AddressSpaceText.size() && AddressSpaceText[NumberLength] >= '0' &&
               AddressSpaceText[NumberLength] <= '9') {
          ++NumberLength;
        }
        if (NumberLength != 0) {
          auto Parsed =
              parseUnsignedField(AddressSpaceText.substr(0, NumberLength), "pointer address space");
          if (!Parsed)
            return std::unexpected(Parsed.error());
          AddressSpace = *Parsed;
          AddressSpaceText.remove_prefix(NumberLength);
        }
        if (!AddressSpaceText.empty()) {
          if (AddressSpaceText.size() <= 2 || AddressSpaceText.front() != '(' ||
              AddressSpaceText.back() != ')') {
            return makeUnexpected(ErrorCode::ParseError,
                                  "LLVM pointer address space or name is invalid");
          }
          AddressSpaceName = std::string(AddressSpaceText.substr(1, AddressSpaceText.size() - 2));
          if (AddressSpaceName.size() == 1 &&
              (AddressSpaceName.front() == 'P' || AddressSpaceName.front() == 'G' ||
               AddressSpaceName.front() == 'A')) {
            return makeUnexpected(ErrorCode::ParseError,
                                  "LLVM pointer address space uses a reserved name");
          }
        }
      }
      if (AddressSpace == 0 && (HasExternalState || HasUnstableRepresentation)) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM address space zero cannot be unstable or external");
      }
      if (!AddressSpaceName.empty() &&
          std::ranges::any_of(Result.PointerSpecs, [&AddressSpaceName](const PointerSpec &Entry) {
            return Entry.AddressSpaceName == AddressSpaceName;
          })) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer address-space name is duplicated");
      }
      const auto Fields = split(Component.substr(Colon + 1U), ':');
      if (Fields.size() < 2 || Fields.size() > 4 || AddressSpace > MaximumDataLayoutSize) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer data layout component has invalid fields");
      }
      auto Size = parseUnsignedField(Fields[0], "pointer size");
      auto ABI = alignmentBytes(Fields[1], false, "pointer ABI alignment");
      auto Preferred = Fields.size() >= 3
                           ? alignmentBytes(Fields[2], false, "pointer preferred alignment")
                           : ABI;
      if (!Size || !ABI || !Preferred || *Size == 0 || *Size > MaximumDataLayoutSize ||
          *Preferred < *ABI) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM pointer data layout component is invalid");
      }
      if (Fields.size() == 4) {
        auto ParsedIndexSize = parseUnsignedField(Fields[3], "pointer index size");
        if (!ParsedIndexSize || *ParsedIndexSize == 0 || *ParsedIndexSize > MaximumDataLayoutSize ||
            *ParsedIndexSize > *Size) {
          return makeUnexpected(ErrorCode::ParseError, "LLVM pointer index size is invalid");
        }
      }
      Result.setPointerSpec({.AddressSpace = static_cast<std::uint32_t>(AddressSpace),
                             .SizeInBits = static_cast<std::uint32_t>(*Size),
                             .Alignment = {.ABI = *ABI, .Preferred = *Preferred},
                             .AddressSpaceName = std::move(AddressSpaceName)});
      continue;
    }

    if (Component == "ve")
      continue;

    if (Component.front() == 'i' || Component.front() == 'f' || Component.front() == 'v') {
      const std::size_t Colon = Component.find(':');
      if (Colon == std::string_view::npos)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM scalar data layout component has no alignment");
      auto Width = parseUnsignedField(Component.substr(1, Colon - 1U), "type alignment bit width");
      const auto Fields = split(Component.substr(Colon + 1U), ':');
      if (!Width || *Width == 0 || *Width > MaximumDataLayoutSize || Fields.empty() ||
          Fields.size() > 2) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM scalar data layout component has invalid fields");
      }
      auto ABI = alignmentBytes(Fields[0], false, "type ABI alignment");
      auto Preferred =
          Fields.size() == 2 ? alignmentBytes(Fields[1], false, "type preferred alignment") : ABI;
      if (!ABI || !Preferred || *Preferred < *ABI)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM scalar data layout alignment is invalid");
      Result.setPrimitiveSpec(
          {Component.front(), static_cast<std::uint32_t>(*Width), {*ABI, *Preferred}});
      continue;
    }

    if (Component.starts_with('a')) {
      const auto Fields = split(Component.substr(1), ':');
      if (Fields.size() < 2 || Fields.size() > 3 || (!Fields[0].empty() && Fields[0] != "0")) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM aggregate data layout component has invalid fields");
      }
      auto ABI = alignmentBytes(Fields[1], true, "aggregate ABI alignment");
      auto Preferred = Fields.size() == 3
                           ? alignmentBytes(Fields[2], false, "aggregate preferred alignment")
                           : ABI;
      if (!ABI || !Preferred || *Preferred < *ABI)
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM aggregate data layout alignment is invalid");
      Result.AggregateAlignment = {.ABI = *ABI, .Preferred = *Preferred};
      continue;
    }

    if (Component == "e" || Component == "E" || Component == "s" || Component == "ve")
      continue;

    if (Component.starts_with("ni:")) {
      const auto AddressSpaces = split(Component.substr(3), ':');
      if (AddressSpaces.empty()) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM non-integral address-space component is empty");
      }
      for (const std::string_view AddressSpace : AddressSpaces) {
        auto Parsed = parseUnsignedField(AddressSpace, "non-integral address space");
        if (!Parsed || *Parsed == 0 || *Parsed > MaximumDataLayoutSize) {
          return makeUnexpected(ErrorCode::ParseError,
                                "LLVM non-integral address space is invalid");
        }
      }
      continue;
    }

    if (Component.starts_with("m:")) {
      const std::string_view Mode = Component.substr(2);
      if (Mode.size() != 1 || (Mode.front() != 'e' && Mode.front() != 'l' && Mode.front() != 'o' &&
                               Mode.front() != 'm' && Mode.front() != 'w' && Mode.front() != 'x' &&
                               Mode.front() != 'a')) {
        return makeUnexpected(ErrorCode::ParseError, "LLVM mangling mode is invalid");
      }
      continue;
    }

    if (Component.starts_with('n')) {
      const auto Widths = split(Component.substr(1), ':');
      if (Widths.empty()) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM native integer-width component is empty");
      }
      for (const std::string_view Width : Widths) {
        auto Parsed = parseUnsignedField(Width, "native integer bit width");
        if (!Parsed || *Parsed == 0 || *Parsed > MaximumDataLayoutSize) {
          return makeUnexpected(ErrorCode::ParseError, "LLVM native integer bit width is invalid");
        }
      }
      continue;
    }

    if (Component.starts_with('S')) {
      auto AlignmentValue = alignmentBytes(Component.substr(1), false, "stack natural alignment");
      if (!AlignmentValue)
        return std::unexpected(AlignmentValue.error());
      continue;
    }

    if (Component.starts_with('F')) {
      const std::string_view Rest = Component.substr(1);
      if (Rest.size() < 2 || (Rest.front() != 'i' && Rest.front() != 'n')) {
        return makeUnexpected(ErrorCode::ParseError,
                              "LLVM function-pointer alignment component is invalid");
      }
      auto AlignmentValue = alignmentBytes(Rest.substr(1), false, "function-pointer ABI alignment");
      if (!AlignmentValue)
        return std::unexpected(AlignmentValue.error());
      continue;
    }

    if (Component.starts_with('P') || Component.starts_with('G') || Component.starts_with('A')) {
      auto AddressSpace = parseUnsignedField(Component.substr(1), "default address space");
      if (!AddressSpace || *AddressSpace > MaximumDataLayoutSize) {
        return makeUnexpected(ErrorCode::ParseError, "LLVM default address space is invalid");
      }
      continue;
    }

    return makeUnexpected(ErrorCode::ParseError, "unsupported LLVM data layout component '" +
                                                     std::string(Component) + "'");
  }
  return Result;
}

DataLayout::AlignmentSpec DataLayout::naturalAlignment(std::uint64_t StoreSize) const noexcept {
  if (StoreSize == 0)
    return {1, 1};
  constexpr std::uint64_t MaximumPowerOfTwo = std::uint64_t{1} << 63U;
  const std::uint64_t LimitedSize =
      std::min<std::uint64_t>(StoreSize, Explicit ? MaximumPowerOfTwo : 16U);
  const std::uint64_t Alignment = std::bit_ceil(LimitedSize);
  return {Alignment, Alignment};
}

DataLayout::AlignmentSpec DataLayout::scalarAlignment(char Kind, std::uint32_t BitWidth,
                                                      std::uint64_t StoreSize) const noexcept {
  if (!Explicit)
    return naturalAlignment(StoreSize);
  const PrimitiveSpec *Selected = nullptr;
  for (const PrimitiveSpec &Entry : PrimitiveSpecs) {
    if (Entry.Kind != Kind)
      continue;
    if (Kind == 'i') {
      const bool BetterMatch =
          Selected == nullptr ||
          (Entry.BitWidth >= BitWidth && Entry.BitWidth < Selected->BitWidth) ||
          (Selected->BitWidth < BitWidth && Entry.BitWidth > Selected->BitWidth);
      if (BetterMatch)
        Selected = &Entry;
    } else if (Entry.BitWidth == BitWidth) {
      Selected = &Entry;
      break;
    }
  }
  return Selected == nullptr ? naturalAlignment(StoreSize) : Selected->Alignment;
}

DataLayout::AlignmentSpec DataLayout::integerAlignment(std::uint32_t BitWidth,
                                                       std::uint64_t StoreSize) const noexcept {
  return scalarAlignment('i', BitWidth, StoreSize);
}

DataLayout::AlignmentSpec DataLayout::floatingAlignment(std::uint32_t BitWidth,
                                                        std::uint64_t StoreSize) const noexcept {
  return scalarAlignment('f', BitWidth, StoreSize);
}

DataLayout::AlignmentSpec DataLayout::vectorAlignment(std::uint32_t BitWidth,
                                                      std::uint64_t StoreSize) const noexcept {
  return scalarAlignment('v', BitWidth, StoreSize);
}

Expected<DataLayout::PointerSpec> DataLayout::getPointerSpec(std::uint32_t AddressSpace) const {
  if (!Explicit) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "pointer size is required to lay out pointer constants");
  }
  auto Entry = std::ranges::find_if(PointerSpecs, [AddressSpace](const PointerSpec &Candidate) {
    return Candidate.AddressSpace == AddressSpace;
  });
  if (Entry == PointerSpecs.end()) {
    Entry = std::ranges::find_if(
        PointerSpecs, [](const PointerSpec &Candidate) { return Candidate.AddressSpace == 0; });
  }
  if (Entry == PointerSpecs.end())
    return makeUnexpected(ErrorCode::InvalidArgument, "LLVM data layout has no pointer layout");
  return *Entry;
}

Expected<DataLayout::TypeLayout> DataLayout::getTypeLayout(const GenericConstantType &Type,
                                                           std::vector<std::string> &NamedTypeStack,
                                                           std::size_t Depth) const {
  if (Depth > MaximumTypeLayoutDepth)
    return makeUnexpected(ErrorCode::Unsupported, "LLVM type exceeds the layout depth limit");
  auto Verification = Type.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  const auto ScalarLayout = [this](std::uint64_t StoreSize,
                                   AlignmentSpec TypeAlignment) -> Expected<TypeLayout> {
    std::uint64_t AllocationSize = StoreSize;
    if (Explicit) {
      auto Aligned = alignTo(StoreSize, TypeAlignment.ABI);
      if (!Aligned)
        return std::unexpected(Aligned.error());
      AllocationSize = *Aligned;
    }
    return TypeLayout{StoreSize, AllocationSize, TypeAlignment.ABI, TypeAlignment.Preferred};
  };

  switch (Type.Kind) {
  case GenericConstantTypeKind::Integer:
  case GenericConstantTypeKind::Byte: {
    const std::uint32_t Bits = Type.Kind == GenericConstantTypeKind::Byte ? 8U : Type.BitWidth;
    const std::uint64_t StoreSize = (static_cast<std::uint64_t>(Bits) + 7U) / 8U;
    return ScalarLayout(StoreSize, integerAlignment(Bits, StoreSize));
  }
  case GenericConstantTypeKind::Floating: {
    const std::uint32_t Bits = getFloatSemanticsBitWidth(Type.Semantics).value_or(0);
    const std::uint64_t StoreSize = (static_cast<std::uint64_t>(Bits) + 7U) / 8U;
    return ScalarLayout(StoreSize, floatingAlignment(Bits, StoreSize));
  }
  case GenericConstantTypeKind::Pointer: {
    auto Pointer = getPointerSpec(Type.AddressSpace);
    if (!Pointer)
      return std::unexpected(Pointer.error());
    const std::uint64_t StoreSize = (static_cast<std::uint64_t>(Pointer->SizeInBits) + 7U) / 8U;
    return ScalarLayout(StoreSize, Pointer->Alignment);
  }
  case GenericConstantTypeKind::Array: {
    auto Element = getTypeLayout(Type.Elements.front(), NamedTypeStack, Depth + 1);
    if (!Element)
      return std::unexpected(Element.error());
    auto Stride = alignTo(Element->AllocationSize, Element->ABIAlignment);
    if (!Stride)
      return std::unexpected(Stride.error());
    auto Size = checkedMultiply(*Stride, Type.ElementCount, "LLVM array allocation size");
    if (!Size)
      return std::unexpected(Size.error());
    return TypeLayout{*Size, *Size, Element->ABIAlignment, Element->PreferredAlignment};
  }
  case GenericConstantTypeKind::Vector: {
    if (Type.Scalable)
      return makeUnexpected(ErrorCode::Unsupported,
                            "scalable vector has no fixed constant-pool size");
    const GenericConstantType &ElementType = Type.Elements.front().resolved();
    std::uint64_t ElementBits = 0;
    if (ElementType.Kind == GenericConstantTypeKind::Integer)
      ElementBits = ElementType.BitWidth;
    else if (ElementType.Kind == GenericConstantTypeKind::Byte)
      ElementBits = 8;
    else if (ElementType.Kind == GenericConstantTypeKind::Floating)
      ElementBits = getFloatSemanticsBitWidth(ElementType.Semantics).value_or(0);
    else if (ElementType.Kind == GenericConstantTypeKind::Pointer) {
      auto Pointer = getPointerSpec(ElementType.AddressSpace);
      if (!Pointer)
        return std::unexpected(Pointer.error());
      ElementBits = Pointer->SizeInBits;
    } else {
      return makeUnexpected(ErrorCode::Unsupported,
                            "LLVM vector element type has no fixed bit size");
    }
    auto TotalBits = checkedMultiply(ElementBits, Type.ElementCount, "LLVM vector bit size");
    if (!TotalBits || *TotalBits > std::numeric_limits<std::uint64_t>::max() - 7U)
      return makeUnexpected(ErrorCode::Unsupported, "LLVM vector bit size is too large");
    const std::uint64_t StoreSize = (*TotalBits + 7U) / 8U;
    return ScalarLayout(StoreSize,
                        vectorAlignment(static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                            *TotalBits, std::numeric_limits<std::uint32_t>::max())),
                                        StoreSize));
  }
  case GenericConstantTypeKind::Struct: {
    if (Type.Packed) {
      std::uint64_t Size = 0;
      for (const GenericConstantType &MemberType : Type.Elements) {
        auto Member = getTypeLayout(MemberType, NamedTypeStack, Depth + 1);
        if (!Member)
          return std::unexpected(Member.error());
        auto Next = checkedAdd(Size, Member->AllocationSize, "LLVM packed struct allocation size");
        if (!Next)
          return std::unexpected(Next.error());
        Size = *Next;
      }
      return TypeLayout{Size, Size, 1, std::max<std::uint64_t>(AggregateAlignment.Preferred, 1)};
    }
    std::uint64_t Offset = 0;
    std::uint64_t ABIAlignment = std::max<std::uint64_t>(AggregateAlignment.ABI, 1);
    for (const GenericConstantType &MemberType : Type.Elements) {
      auto Member = getTypeLayout(MemberType, NamedTypeStack, Depth + 1);
      if (!Member)
        return std::unexpected(Member.error());
      auto AlignedOffset = alignTo(Offset, Member->ABIAlignment);
      if (!AlignedOffset)
        return std::unexpected(AlignedOffset.error());
      auto Next = checkedAdd(*AlignedOffset, Member->AllocationSize, "LLVM struct allocation size");
      if (!Next)
        return std::unexpected(Next.error());
      Offset = *Next;
      ABIAlignment = std::max(ABIAlignment, Member->ABIAlignment);
    }
    auto Size = alignTo(Offset, ABIAlignment);
    if (!Size)
      return std::unexpected(Size.error());
    return TypeLayout{*Size, *Size, ABIAlignment,
                      std::max(AggregateAlignment.Preferred, ABIAlignment)};
  }
  case GenericConstantTypeKind::Named: {
    if (std::ranges::find(NamedTypeStack, Type.Name) != NamedTypeStack.end()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "named LLVM type definition is recursively unsized");
    }
    NamedTypeStack.push_back(Type.Name);
    Expected<TypeLayout> Result =
        makeUnexpected(ErrorCode::Unsupported, "named LLVM type has no embedded type definition");
    if (Type.Elements.size() == 1)
      Result = getTypeLayout(Type.Elements.front(), NamedTypeStack, Depth + 1);
    NamedTypeStack.pop_back();
    return Result;
  }
  case GenericConstantTypeKind::Invalid:
  case GenericConstantTypeKind::Void:
  case GenericConstantTypeKind::Label:
  case GenericConstantTypeKind::Token:
  case GenericConstantTypeKind::Metadata:
  case GenericConstantTypeKind::Function:
    return makeUnexpected(ErrorCode::Unsupported,
                          "LLVM type is not sized for a machine constant pool");
  }
  return makeUnexpected(ErrorCode::InternalError, "LLVM constant type layout is unhandled");
}

Expected<std::uint64_t> DataLayout::getTypeAllocSize(const GenericConstantType &Type) const {
  std::vector<std::string> NamedTypeStack;
  auto Layout = getTypeLayout(Type, NamedTypeStack, 0);
  if (!Layout)
    return std::unexpected(Layout.error());
  return Layout->AllocationSize;
}

Expected<std::uint64_t> DataLayout::getABITypeAlign(const GenericConstantType &Type) const {
  std::vector<std::string> NamedTypeStack;
  auto Layout = getTypeLayout(Type, NamedTypeStack, 0);
  if (!Layout)
    return std::unexpected(Layout.error());
  return Layout->ABIAlignment;
}

Expected<std::uint64_t> DataLayout::getPrefTypeAlign(const GenericConstantType &Type) const {
  std::vector<std::string> NamedTypeStack;
  auto Layout = getTypeLayout(Type, NamedTypeStack, 0);
  if (!Layout)
    return std::unexpected(Layout.error());
  return Layout->PreferredAlignment;
}

Expected<std::uint32_t> DataLayout::getPointerSizeInBits(std::uint32_t AddressSpace) const {
  auto Pointer = getPointerSpec(AddressSpace);
  if (!Pointer)
    return std::unexpected(Pointer.error());
  return Pointer->SizeInBits;
}

} // namespace rcc
