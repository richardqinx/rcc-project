// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineMemOperand.h"

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] std::uint64_t effectiveAlignment(std::uint64_t BaseAlignment,
                                               std::int64_t Offset) noexcept {
  if (Offset == 0)
    return BaseAlignment;
  const auto OffsetBits = static_cast<std::uint64_t>(Offset);
  const std::uint64_t OffsetAlignment = std::uint64_t{1} << std::countr_zero(OffsetBits);
  return std::min(BaseAlignment, OffsetAlignment);
}

[[nodiscard]] bool isValidAtomicLoadOrdering(AtomicOrdering Ordering) noexcept {
  return Ordering == AtomicOrdering::Unordered || Ordering == AtomicOrdering::Monotonic ||
         Ordering == AtomicOrdering::Acquire || Ordering == AtomicOrdering::SequentiallyConsistent;
}

[[nodiscard]] bool isValidAtomicStoreOrdering(AtomicOrdering Ordering) noexcept {
  return Ordering == AtomicOrdering::Unordered || Ordering == AtomicOrdering::Monotonic ||
         Ordering == AtomicOrdering::Release || Ordering == AtomicOrdering::SequentiallyConsistent;
}

[[nodiscard]] bool isValidAtomicReadModifyWriteOrdering(AtomicOrdering Ordering) noexcept {
  return Ordering == AtomicOrdering::Monotonic || Ordering == AtomicOrdering::Acquire ||
         Ordering == AtomicOrdering::Release || Ordering == AtomicOrdering::AcquireRelease ||
         Ordering == AtomicOrdering::SequentiallyConsistent;
}

[[nodiscard]] bool isValidCompareExchangeFailureOrdering(AtomicOrdering Ordering) noexcept {
  return Ordering == AtomicOrdering::NotAtomic || Ordering == AtomicOrdering::Monotonic ||
         Ordering == AtomicOrdering::Acquire || Ordering == AtomicOrdering::SequentiallyConsistent;
}

[[nodiscard]] bool isFailureOrderingNoStrongerThanSuccess(AtomicOrdering Failure,
                                                          AtomicOrdering Success) noexcept {
  switch (Failure) {
  case AtomicOrdering::NotAtomic:
    return true;
  case AtomicOrdering::Monotonic:
    return isValidAtomicReadModifyWriteOrdering(Success);
  case AtomicOrdering::Acquire:
    return Success == AtomicOrdering::Acquire || Success == AtomicOrdering::AcquireRelease ||
           Success == AtomicOrdering::SequentiallyConsistent;
  case AtomicOrdering::SequentiallyConsistent:
    return Success == AtomicOrdering::SequentiallyConsistent;
  case AtomicOrdering::Unordered:
  case AtomicOrdering::Release:
  case AtomicOrdering::AcquireRelease:
    return false;
  }

  return false;
}

[[nodiscard]] Expected<void> verifyAtomicDescription(const MachineMemOperandDesc &Description) {
  const bool IsLoad = Description.Accesses.has(MemoryAccessFlag::Load);
  const bool IsStore = Description.Accesses.has(MemoryAccessFlag::Store);

  switch (Description.AtomicKind) {
  case AtomicOperation::None:
    if (Description.SuccessOrdering != AtomicOrdering::NotAtomic ||
        Description.FailureOrdering != AtomicOrdering::NotAtomic) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "non-atomic memory operands cannot carry atomic orderings");
    }
    return {};
  case AtomicOperation::Load:
    if (!IsLoad || IsStore) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "atomic load memory operands must be load-only");
    }
    if (!isValidAtomicLoadOrdering(Description.SuccessOrdering) ||
        Description.FailureOrdering != AtomicOrdering::NotAtomic) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "atomic load memory operand has an invalid ordering");
    }
    return {};
  case AtomicOperation::Store:
    if (IsLoad || !IsStore) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "atomic store memory operands must be store-only");
    }
    if (!isValidAtomicStoreOrdering(Description.SuccessOrdering) ||
        Description.FailureOrdering != AtomicOrdering::NotAtomic) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "atomic store memory operand has an invalid ordering");
    }
    return {};
  case AtomicOperation::ReadModifyWrite:
    if (!IsLoad || !IsStore) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "atomic read-modify-write memory operands must load and store");
    }
    if (!isValidAtomicReadModifyWriteOrdering(Description.SuccessOrdering) ||
        Description.FailureOrdering != AtomicOrdering::NotAtomic) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "atomic read-modify-write memory operand has an invalid ordering");
    }
    return {};
  case AtomicOperation::CompareExchange:
    if (!IsLoad || !IsStore) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "compare-exchange memory operands must load and store");
    }
    if (!isValidAtomicReadModifyWriteOrdering(Description.SuccessOrdering) ||
        !isValidCompareExchangeFailureOrdering(Description.FailureOrdering) ||
        !isFailureOrderingNoStrongerThanSuccess(Description.FailureOrdering,
                                                Description.SuccessOrdering)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "compare-exchange memory operand has incompatible orderings");
    }
    return {};
  }

  return makeUnexpected(ErrorCode::InternalError, "unknown atomic operation");
}

} // namespace

MemoryAccessFlags::MemoryAccessFlags(std::initializer_list<MemoryAccessFlag> Flags) noexcept {
  for (const MemoryAccessFlag Flag : Flags)
    set(Flag);
}

Expected<MemorySize> MemorySize::fixed(std::uint64_t Bytes) {
  if (Bytes == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "fixed memory size must be nonzero");
  return MemorySize(Kind::Fixed, Bytes);
}

Expected<MemorySize> MemorySize::scalable(std::uint64_t MinimumBytes) {
  if (MinimumBytes == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "scalable memory size minimum must be nonzero");
  }
  return MemorySize(Kind::Scalable, MinimumBytes);
}

Expected<MemoryOffsetRange> MemoryOffsetRange::create(std::int64_t Lower, std::int64_t Upper) {
  if (Lower > Upper) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "memory offset range lower bound cannot exceed its upper bound");
  }
  return MemoryOffsetRange{Lower, Upper};
}

MemoryBase MemoryBase::frameIndex(FrameIndex Index) {
  return MemoryBase(Kind::FrameIndex, Index);
}

MemoryBase MemoryBase::constantPool(ConstantPoolIndex Index) {
  return MemoryBase(Kind::ConstantPool, Index);
}

MemoryBase MemoryBase::jumpTable(JumpTableIndex Index) {
  return MemoryBase(Kind::JumpTable, Index);
}

MemoryBase MemoryBase::globalSymbol(std::string Name) {
  return MemoryBase(Kind::GlobalSymbol, std::move(Name));
}

MemoryBase MemoryBase::externalSymbol(std::string Name) {
  return MemoryBase(Kind::ExternalSymbol, std::move(Name));
}

MemoryBase MemoryBase::irValue(std::string Name) {
  return MemoryBase(Kind::IRValue, std::move(Name));
}

Expected<MemoryBase> MemoryBase::irConstant(GenericConstantData Constant) {
  auto Verification = Constant.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (Constant.Type.Kind != GenericConstantTypeKind::Pointer) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine memory IR constant base must have pointer type");
  }
  return MemoryBase(Kind::IRConstant, std::move(Constant));
}

MemoryBase MemoryBase::stack() {
  return MemoryBase(Kind::Stack, std::monostate{});
}

MemoryBase MemoryBase::got() {
  return MemoryBase(Kind::GOT, std::monostate{});
}

MemoryBase MemoryBase::constantPoolPseudo() {
  return MemoryBase(Kind::ConstantPoolPseudo, std::monostate{});
}

MemoryBase MemoryBase::jumpTablePseudo() {
  return MemoryBase(Kind::JumpTablePseudo, std::monostate{});
}

MemoryBase MemoryBase::callEntryGlobal(std::string Name) {
  return MemoryBase(Kind::CallEntryGlobal, std::move(Name));
}

MemoryBase MemoryBase::callEntryExternal(std::string Name) {
  return MemoryBase(Kind::CallEntryExternal, std::move(Name));
}

MemoryBase MemoryBase::custom(std::string Name) {
  return MemoryBase(Kind::Custom, std::move(Name));
}

std::optional<FrameIndex> MemoryBase::frameIndex() const noexcept {
  if (BaseKind != Kind::FrameIndex)
    return std::nullopt;
  return std::get<FrameIndex>(Value);
}

std::optional<ConstantPoolIndex> MemoryBase::constantPoolIndex() const noexcept {
  if (BaseKind != Kind::ConstantPool)
    return std::nullopt;
  return std::get<ConstantPoolIndex>(Value);
}

std::optional<JumpTableIndex> MemoryBase::jumpTableIndex() const noexcept {
  if (BaseKind != Kind::JumpTable)
    return std::nullopt;
  return std::get<JumpTableIndex>(Value);
}

std::optional<std::string_view> MemoryBase::symbolName() const noexcept {
  if (BaseKind != Kind::GlobalSymbol && BaseKind != Kind::ExternalSymbol &&
      BaseKind != Kind::IRValue && BaseKind != Kind::CallEntryGlobal &&
      BaseKind != Kind::CallEntryExternal && BaseKind != Kind::Custom) {
    return std::nullopt;
  }
  return std::get<std::string>(Value);
}

const GenericConstantData *MemoryBase::constantValue() const noexcept {
  if (BaseKind != Kind::IRConstant)
    return nullptr;
  return std::get_if<GenericConstantData>(&Value);
}

Expected<MachineMemOperand> MachineMemOperand::create(MachineMemOperandDesc Description) {
  const bool IsLoad = Description.Accesses.has(MemoryAccessFlag::Load);
  const bool IsStore = Description.Accesses.has(MemoryAccessFlag::Store);
  if (!IsLoad && !IsStore) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine memory operand must describe a load or store");
  }
  if (Description.AddressSpace > LowLevelType::MaxAddressSpace) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine memory operand address space must fit in 24 bits");
  }
  if (Description.BaseAlign) {
    const std::uint64_t Effective =
        effectiveAlignment(Description.BaseAlign->bytes(), Description.Offset);
    if (Effective != Description.Align.bytes()) {
      return makeUnexpected(
          ErrorCode::InvalidArgument,
          "machine memory operand alignment is inconsistent with base alignment and offset");
    }
    if (*Description.BaseAlign == Description.Align)
      Description.BaseAlign.reset();
  }
  if (Description.Accesses.has(MemoryAccessFlag::Invariant) && (!IsLoad || IsStore)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "invariant memory operands must be load-only");
  }

  const auto AtomicResult = verifyAtomicDescription(Description);
  if (!AtomicResult)
    return std::unexpected(AtomicResult.error());

  const auto SymbolName = Description.Base.symbolName();
  if (SymbolName && SymbolName->empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "memory base symbol name cannot be empty");
  }
  const std::array MetadataReferences{
      Description.TBAA,          Description.AliasScope,
      Description.NoAlias,       Description.NoAliasAddressSpace,
      Description.RangeMetadata,
  };
  if (std::ranges::any_of(MetadataReferences, [](const auto &Reference) {
        return Reference.has_value() && !Reference->isValid();
      })) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine memory operand contains an invalid metadata reference");
  }

  return MachineMemOperand(std::move(Description));
}

std::string_view getAtomicOrderingName(AtomicOrdering Ordering) noexcept {
  switch (Ordering) {
  case AtomicOrdering::NotAtomic:
    return "not-atomic";
  case AtomicOrdering::Unordered:
    return "unordered";
  case AtomicOrdering::Monotonic:
    return "monotonic";
  case AtomicOrdering::Acquire:
    return "acquire";
  case AtomicOrdering::Release:
    return "release";
  case AtomicOrdering::AcquireRelease:
    return "acquire-release";
  case AtomicOrdering::SequentiallyConsistent:
    return "sequentially-consistent";
  }

  return "unknown";
}

} // namespace rcc
