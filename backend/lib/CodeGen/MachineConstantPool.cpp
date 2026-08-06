// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineConstantPool.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] const TargetConstant *
getTargetConstant(const MachineConstantPoolValue &Value) noexcept {
  if (const auto *Target = std::get_if<TargetConstant>(&Value))
    return Target;
  if (const auto *Expression = std::get_if<TargetExpressionConstant>(&Value))
    return &Expression->Expression;
  return nullptr;
}

[[nodiscard]] Expected<void> verifyTargetConstant(const MachineConstantPoolEntryDesc &Description,
                                                  const TargetConstant &Constant) {
  auto Verification = Constant.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  auto RequiredAlignment = Constant.requiredAlignment();
  if (!RequiredAlignment)
    return std::unexpected(RequiredAlignment.error());
  if (Description.Align.bytes() < RequiredAlignment->bytes()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "constant pool alignment is weaker than target constant requirement");
  }

  auto Emission = Constant.emit();
  if (!Emission)
    return std::unexpected(Emission.error());
  if (Emission->Bytes.size() != Description.Size) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target constant emitted size does not match its pool entry size");
  }

  auto RelocationKind = Constant.relocationKind();
  if (!RelocationKind)
    return std::unexpected(RelocationKind.error());
  const bool HasRelocation = RelocationKind->has_value() || !Emission->Fixups.empty();
  if (HasRelocation == (Description.Relocation == ConstantPoolRelocationMode::None)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target constant relocation contract disagrees with its pool entry");
  }
  return {};
}

[[nodiscard]] std::optional<std::uint64_t>
fixedGenericStoreSize(const GenericConstantType &Type) noexcept {
  switch (Type.Kind) {
  case GenericConstantTypeKind::Integer:
    return (static_cast<std::uint64_t>(Type.BitWidth) + 7U) / 8U;
  case GenericConstantTypeKind::Byte:
    return 1;
  case GenericConstantTypeKind::Floating:
    return (static_cast<std::uint64_t>(getFloatSemanticsBitWidth(Type.Semantics).value_or(0)) +
            7U) /
           8U;
  case GenericConstantTypeKind::Array:
  case GenericConstantTypeKind::Vector: {
    if (Type.Scalable || Type.Elements.size() != 1)
      return std::nullopt;
    const auto ElementSize = fixedGenericStoreSize(Type.Elements.front());
    if (!ElementSize ||
        (*ElementSize != 0 &&
         Type.ElementCount > std::numeric_limits<std::uint64_t>::max() / *ElementSize)) {
      return std::nullopt;
    }
    return *ElementSize * Type.ElementCount;
  }
  default:
    return std::nullopt;
  }
}

} // namespace

ConstantPoolEntryKind MachineConstantPoolEntryDesc::kind() const noexcept {
  switch (Value.index()) {
  case 0:
    return ConstantPoolEntryKind::Generic;
  case 1:
    return ConstantPoolEntryKind::Target;
  case 2:
    return ConstantPoolEntryKind::SymbolDifference;
  case 3:
    return ConstantPoolEntryKind::BlockAddress;
  case 4:
    return ConstantPoolEntryKind::TargetExpression;
  default:
    return ConstantPoolEntryKind::Generic;
  }
}

Expected<void> MachineConstantPoolEntryDesc::verify() const {
  if (Size == 0)
    return makeUnexpected(ErrorCode::VerificationError, "constant pool entry size must be nonzero");
  if (Placement.Section >= ConstantPoolSectionKind::Count) {
    return makeUnexpected(ErrorCode::VerificationError, "constant pool section kind is invalid");
  }
  if (Placement.MaximumDistance && *Placement.MaximumDistance == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "constant pool maximum distance must be nonzero");
  }
  if ((Placement.Section == ConstantPoolSectionKind::Target) != (Placement.TargetFlags != 0)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "target constant pool placement requires exactly one target identity");
  }
  if (Placement.Section == ConstantPoolSectionKind::MergeableReadOnly &&
      Merge != ConstantPoolMergeKind::Exact) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "mergeable constant section requires exact entry merging");
  }

  switch (kind()) {
  case ConstantPoolEntryKind::Generic: {
    const auto *Generic = std::get_if<GenericConstantData>(&Value);
    if (Generic == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic constant pool entry has an invalid payload");
    }
    auto Verification = Generic->verify();
    if (!Verification)
      return Verification;
    if (const auto FixedSize = fixedGenericStoreSize(Generic->Type);
        FixedSize && *FixedSize != Size) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "generic constant store size does not match its pool entry size");
    }
    if (!Generic->referencesSymbol() && Relocation != ConstantPoolRelocationMode::None) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "symbol-free generic constant cannot require relocation");
    }
    return {};
  }
  case ConstantPoolEntryKind::Target:
  case ConstantPoolEntryKind::TargetExpression: {
    const TargetConstant *Target = getTargetConstant(Value);
    if (Target == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "target constant variant is inconsistent");
    return verifyTargetConstant(*this, *Target);
  }
  case ConstantPoolEntryKind::SymbolDifference: {
    const auto *Difference = std::get_if<SymbolDifferenceConstant>(&Value);
    if (Difference == nullptr || Difference->Minuend.empty() || Difference->Subtrahend.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "symbol-difference constant requires two symbols");
    }
    if (Relocation == ConstantPoolRelocationMode::None) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "symbol-difference constant requires a relocation mode");
    }
    return {};
  }
  case ConstantPoolEntryKind::BlockAddress: {
    const auto *Address = std::get_if<BlockAddressConstant>(&Value);
    if (Address == nullptr || Address->FunctionName.empty() || Address->BlockName.empty()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "block-address constant requires a function and block name");
    }
    if (Relocation == ConstantPoolRelocationMode::None) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "block-address constant requires a relocation mode");
    }
    return {};
  }
  }
  return makeUnexpected(ErrorCode::InternalError, "constant pool entry kind is unhandled");
}

Expected<MachineConstantPoolEntryDesc> MachineConstantPoolEntryDesc::clone() const {
  MachineConstantPoolEntryDesc Result;
  Result.Size = Size;
  Result.Align = Align;
  Result.Merge = Merge;
  Result.Relocation = Relocation;
  Result.Placement = Placement;

  if (const auto *Generic = std::get_if<GenericConstantData>(&Value)) {
    Result.Value = *Generic;
  } else if (const auto *Target = std::get_if<TargetConstant>(&Value)) {
    auto Cloned = Target->clone();
    if (!Cloned)
      return std::unexpected(Cloned.error());
    Result.Value = std::move(*Cloned);
  } else if (const auto *Difference = std::get_if<SymbolDifferenceConstant>(&Value)) {
    Result.Value = *Difference;
  } else if (const auto *Address = std::get_if<BlockAddressConstant>(&Value)) {
    Result.Value = *Address;
  } else if (const auto *Expression = std::get_if<TargetExpressionConstant>(&Value)) {
    auto Cloned = Expression->Expression.clone();
    if (!Cloned)
      return std::unexpected(Cloned.error());
    Result.Value = TargetExpressionConstant{std::move(*Cloned)};
  } else {
    return makeUnexpected(ErrorCode::InternalError, "constant pool value is unhandled");
  }
  return Result;
}

bool MachineConstantPoolEntryDesc::semanticallyEquals(
    const MachineConstantPoolEntryDesc &Other) const noexcept {
  if (Size != Other.Size || Align != Other.Align || Merge != Other.Merge ||
      Relocation != Other.Relocation || Placement != Other.Placement || kind() != Other.kind()) {
    return false;
  }

  if (const auto *Generic = std::get_if<GenericConstantData>(&Value)) {
    const auto *OtherGeneric = std::get_if<GenericConstantData>(&Other.Value);
    return OtherGeneric != nullptr && *Generic == *OtherGeneric;
  }
  if (const auto *Target = std::get_if<TargetConstant>(&Value)) {
    const auto *OtherTarget = std::get_if<TargetConstant>(&Other.Value);
    return OtherTarget != nullptr && Target->equals(*OtherTarget);
  }
  if (const auto *Difference = std::get_if<SymbolDifferenceConstant>(&Value)) {
    const auto *OtherDifference = std::get_if<SymbolDifferenceConstant>(&Other.Value);
    return OtherDifference != nullptr && *Difference == *OtherDifference;
  }
  if (const auto *Address = std::get_if<BlockAddressConstant>(&Value)) {
    const auto *OtherAddress = std::get_if<BlockAddressConstant>(&Other.Value);
    return OtherAddress != nullptr && *Address == *OtherAddress;
  }
  if (const auto *Expression = std::get_if<TargetExpressionConstant>(&Value)) {
    const auto *OtherExpression = std::get_if<TargetExpressionConstant>(&Other.Value);
    return OtherExpression != nullptr && Expression->Expression.equals(OtherExpression->Expression);
  }
  return false;
}

Expected<MachineConstantPoolEntryDesc> MachineConstantPoolEntry::cloneDescription() const {
  const MachineConstantPoolEntryDesc *Desc = description();
  if (Desc == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "constant pool tombstone has no description to clone");
  }
  return Desc->clone();
}

const MachineConstantPoolEntry *
MachineConstantPool::findEquivalent(const MachineConstantPoolEntryDesc &Description,
                                    std::optional<ConstantPoolIndex> Excluded) const noexcept {
  for (const MachineConstantPoolEntry &Entry : Entries) {
    if (Entry.isTombstone() || (Excluded && Entry.index() == *Excluded))
      continue;
    const MachineConstantPoolEntryDesc *Existing = Entry.description();
    if (Existing != nullptr && Existing->semanticallyEquals(Description))
      return &Entry;
  }
  return nullptr;
}

Expected<ConstantPoolIndex>
MachineConstantPool::addEntry(MachineConstantPoolEntryDesc Description) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen constant pool cannot add entries");
  auto Verification = Description.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (Description.Merge == ConstantPoolMergeKind::Exact) {
    if (const MachineConstantPoolEntry *Existing = findEquivalent(Description))
      return Existing->index();
  }
  if (Entries.size() > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported, "constant pool index space is exhausted");
  }

  const ConstantPoolIndex Index =
      ConstantPoolIndex::fromValue(static_cast<std::uint32_t>(Entries.size()));
  Entries.push_back(MachineConstantPoolEntry(Index, std::move(Description)));
  return Index;
}

Expected<MachineConstantPoolEntry *> MachineConstantPool::mutableEntry(ConstantPoolIndex Index) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen constant pool cannot modify entries");
  if (Index.value() >= Entries.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "constant pool index is out of range");
  MachineConstantPoolEntry &Entry = Entries[Index.value()];
  if (Entry.isTombstone())
    return makeUnexpected(ErrorCode::InvalidArgument, "constant pool index is a tombstone");
  return &Entry;
}

Expected<void> MachineConstantPool::updateEntry(ConstantPoolIndex Index,
                                                MachineConstantPoolEntryDesc Description) {
  auto Verification = Description.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Entry = mutableEntry(Index);
  if (!Entry)
    return std::unexpected(Entry.error());
  if (Description.Merge == ConstantPoolMergeKind::Exact &&
      findEquivalent(Description, Index) != nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "constant pool update would create duplicate mergeable entries");
  }
  (*Entry)->Description.emplace<MachineConstantPoolEntryDesc>(std::move(Description));
  return {};
}

Expected<void> MachineConstantPool::eraseEntry(ConstantPoolIndex Index) {
  auto Entry = mutableEntry(Index);
  if (!Entry)
    return std::unexpected(Entry.error());
  (*Entry)->Description.emplace<std::monostate>();
  return {};
}

const MachineConstantPoolEntry *MachineConstantPool::entry(ConstantPoolIndex Index) const noexcept {
  if (Index.value() >= Entries.size() || Entries[Index.value()].isTombstone())
    return nullptr;
  return &Entries[Index.value()];
}

std::size_t MachineConstantPool::liveEntryCount() const noexcept {
  std::size_t Count = 0;
  for (const MachineConstantPoolEntry &Entry : Entries) {
    if (!Entry.isTombstone())
      ++Count;
  }
  return Count;
}

Expected<void> MachineConstantPool::freeze() {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "constant pool is already frozen");
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  Frozen = true;
  return {};
}

Expected<void> MachineConstantPool::verify() const {
  for (std::size_t Position = 0; Position < Entries.size(); ++Position) {
    const MachineConstantPoolEntry &Entry = Entries[Position];
    if (Entry.index() != ConstantPoolIndex::fromValue(static_cast<std::uint32_t>(Position))) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "constant pool index does not match its stable table position");
    }
    const MachineConstantPoolEntryDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    auto Verification = Description->verify();
    if (!Verification)
      return std::unexpected(Verification.error());
    if (Description->Merge == ConstantPoolMergeKind::Exact &&
        findEquivalent(*Description, Entry.index()) != nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "constant pool contains duplicate mergeable entries");
    }
  }
  return {};
}

} // namespace rcc
