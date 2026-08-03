// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEMEMOPERAND_H
#define RCC_CODEGEN_MACHINEMEMOPERAND_H

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineIRTypes.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace rcc {

enum class MemoryAccessFlag : std::uint8_t {
  Load,
  Store,
  Volatile,
  Invariant,
  Dereferenceable,
  NonTemporal,
  Count,
};

class MemoryAccessFlags final {
public:
  constexpr MemoryAccessFlags() noexcept = default;

  MemoryAccessFlags(std::initializer_list<MemoryAccessFlag> Flags) noexcept;

  [[nodiscard]] constexpr bool has(MemoryAccessFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    if (Index >= static_cast<std::uint8_t>(MemoryAccessFlag::Count))
      return false;
    return (Mask & (std::uint8_t{1} << Index)) != 0;
  }

  constexpr void set(MemoryAccessFlag Flag) noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    if (Index < static_cast<std::uint8_t>(MemoryAccessFlag::Count))
      Mask |= std::uint8_t{1} << Index;
  }

  constexpr void clear(MemoryAccessFlag Flag) noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    if (Index < static_cast<std::uint8_t>(MemoryAccessFlag::Count))
      Mask &= static_cast<std::uint8_t>(~(std::uint8_t{1} << Index));
  }

  [[nodiscard]] constexpr std::uint8_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(MemoryAccessFlags Left,
                                   MemoryAccessFlags Right) noexcept = default;

private:
  std::uint8_t Mask = 0;
};

enum class AtomicOperation : std::uint8_t {
  None,
  Load,
  Store,
  ReadModifyWrite,
  CompareExchange,
};

enum class AtomicOrdering : std::uint8_t {
  NotAtomic,
  Unordered,
  Monotonic,
  Acquire,
  Release,
  AcquireRelease,
  SequentiallyConsistent,
};

class MemorySize final {
public:
  enum class Kind : std::uint8_t {
    Unknown,
    Fixed,
    Scalable,
  };

  constexpr MemorySize() noexcept = default;

  [[nodiscard]] static Expected<MemorySize> fixed(std::uint64_t Bytes);
  [[nodiscard]] static Expected<MemorySize> scalable(std::uint64_t MinimumBytes);

  [[nodiscard]] constexpr Kind kind() const noexcept {
    return SizeKind;
  }

  [[nodiscard]] constexpr bool isKnown() const noexcept {
    return SizeKind != Kind::Unknown;
  }

  [[nodiscard]] constexpr bool isScalable() const noexcept {
    return SizeKind == Kind::Scalable;
  }

  [[nodiscard]] constexpr std::uint64_t minimumBytes() const noexcept {
    return MinimumBytes;
  }

  friend constexpr bool operator==(MemorySize Left, MemorySize Right) noexcept = default;

private:
  constexpr MemorySize(Kind SizeKind, std::uint64_t MinimumBytes) noexcept
      : SizeKind(SizeKind), MinimumBytes(MinimumBytes) {}

  Kind SizeKind = Kind::Unknown;
  std::uint64_t MinimumBytes = 0;
};

struct MemoryOffsetRange final {
  std::int64_t Lower = 0;
  std::int64_t Upper = 0;

  [[nodiscard]] static Expected<MemoryOffsetRange> create(std::int64_t Lower, std::int64_t Upper);

  friend constexpr bool operator==(MemoryOffsetRange Left,
                                   MemoryOffsetRange Right) noexcept = default;
};

class MemoryBase final {
public:
  enum class Kind : std::uint8_t {
    Unknown,
    FrameIndex,
    ConstantPool,
    JumpTable,
    GlobalSymbol,
    ExternalSymbol,
  };

  MemoryBase() = default;

  [[nodiscard]] static MemoryBase frameIndex(FrameIndex Index);
  [[nodiscard]] static MemoryBase constantPool(ConstantPoolIndex Index);
  [[nodiscard]] static MemoryBase jumpTable(JumpTableIndex Index);
  [[nodiscard]] static MemoryBase globalSymbol(std::string Name);
  [[nodiscard]] static MemoryBase externalSymbol(std::string Name);

  [[nodiscard]] Kind kind() const noexcept {
    return BaseKind;
  }

  [[nodiscard]] std::optional<FrameIndex> frameIndex() const noexcept;
  [[nodiscard]] std::optional<ConstantPoolIndex> constantPoolIndex() const noexcept;
  [[nodiscard]] std::optional<JumpTableIndex> jumpTableIndex() const noexcept;
  [[nodiscard]] std::optional<std::string_view> symbolName() const noexcept;

  friend bool operator==(const MemoryBase &Left, const MemoryBase &Right) = default;

private:
  using Storage =
      std::variant<std::monostate, FrameIndex, ConstantPoolIndex, JumpTableIndex, std::string>;

  MemoryBase(Kind BaseKind, Storage Value) : BaseKind(BaseKind), Value(std::move(Value)) {}

  Kind BaseKind = Kind::Unknown;
  Storage Value;
};

struct MachineMemOperandDesc final {
  MemoryAccessFlags Accesses;
  AtomicOperation AtomicKind = AtomicOperation::None;
  AtomicOrdering SuccessOrdering = AtomicOrdering::NotAtomic;
  AtomicOrdering FailureOrdering = AtomicOrdering::NotAtomic;
  SyncScopeID SyncScope;
  MemorySize Size;
  Alignment Align;
  std::uint32_t AddressSpace = 0;
  MemoryBase Base;
  std::int64_t Offset = 0;
  std::optional<MemoryOffsetRange> OffsetRange;
  std::optional<MemoryAliasSourceID> AliasSource;
  std::uint16_t TargetFlags = 0;

  friend bool operator==(const MachineMemOperandDesc &Left,
                         const MachineMemOperandDesc &Right) = default;
};

class MachineMemOperand final {
public:
  MachineMemOperand(const MachineMemOperand &) = default;
  MachineMemOperand &operator=(const MachineMemOperand &) = default;
  MachineMemOperand(MachineMemOperand &&) noexcept = default;
  MachineMemOperand &operator=(MachineMemOperand &&) noexcept = default;
  ~MachineMemOperand() = default;

  [[nodiscard]] static Expected<MachineMemOperand> create(MachineMemOperandDesc Description);

  [[nodiscard]] const MachineMemOperandDesc &description() const noexcept {
    return Description;
  }

  [[nodiscard]] bool isLoad() const noexcept {
    return Description.Accesses.has(MemoryAccessFlag::Load);
  }

  [[nodiscard]] bool isStore() const noexcept {
    return Description.Accesses.has(MemoryAccessFlag::Store);
  }

  [[nodiscard]] bool isAtomic() const noexcept {
    return Description.AtomicKind != AtomicOperation::None;
  }

  [[nodiscard]] bool isVolatile() const noexcept {
    return Description.Accesses.has(MemoryAccessFlag::Volatile);
  }

  friend bool operator==(const MachineMemOperand &Left, const MachineMemOperand &Right) = default;

private:
  explicit MachineMemOperand(MachineMemOperandDesc Description)
      : Description(std::move(Description)) {}

  MachineMemOperandDesc Description;
};

[[nodiscard]] std::string_view getAtomicOrderingName(AtomicOrdering Ordering) noexcept;

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEMEMOPERAND_H
