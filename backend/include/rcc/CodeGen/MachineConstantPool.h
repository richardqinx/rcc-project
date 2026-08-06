// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECONSTANTPOOL_H
#define RCC_CODEGEN_MACHINECONSTANTPOOL_H

#include "rcc/CodeGen/GenericConstant.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

enum class ConstantPoolEntryKind : std::uint8_t {
  Generic,
  Target,
  SymbolDifference,
  BlockAddress,
  TargetExpression,
};

enum class ConstantPoolMergeKind : std::uint8_t {
  Never,
  Exact,
};

enum class ConstantPoolRelocationMode : std::uint8_t {
  None,
  Static,
  PositionIndependent,
  Target,
};

enum class ConstantPoolSectionKind : std::uint8_t {
  TargetDefault,
  ReadOnly,
  MergeableReadOnly,
  Data,
  TextAdjacent,
  Target,
  Count,
};

struct ConstantPoolPlacement final {
  ConstantPoolSectionKind Section = ConstantPoolSectionKind::TargetDefault;
  bool UniqueSection = false;
  std::optional<std::uint64_t> MaximumDistance;
  std::uint64_t TargetFlags = 0;

  friend bool operator==(const ConstantPoolPlacement &Left,
                         const ConstantPoolPlacement &Right) = default;
};

struct SymbolDifferenceConstant final {
  std::string Minuend;
  std::string Subtrahend;
  std::int64_t Addend = 0;

  friend bool operator==(const SymbolDifferenceConstant &Left,
                         const SymbolDifferenceConstant &Right) = default;
};

struct BlockAddressConstant final {
  std::string FunctionName;
  std::string BlockName;
  std::int64_t Addend = 0;

  friend bool operator==(const BlockAddressConstant &Left,
                         const BlockAddressConstant &Right) = default;
};

struct TargetExpressionConstant final {
  TargetConstant Expression;
};

using MachineConstantPoolValue =
    std::variant<GenericConstantData, TargetConstant, SymbolDifferenceConstant,
                 BlockAddressConstant, TargetExpressionConstant>;

struct MachineConstantPoolEntryDesc final {
  MachineConstantPoolEntryDesc() = default;
  MachineConstantPoolEntryDesc(const MachineConstantPoolEntryDesc &) = delete;
  MachineConstantPoolEntryDesc &operator=(const MachineConstantPoolEntryDesc &) = delete;
  MachineConstantPoolEntryDesc(MachineConstantPoolEntryDesc &&) noexcept = default;
  MachineConstantPoolEntryDesc &operator=(MachineConstantPoolEntryDesc &&) noexcept = default;
  ~MachineConstantPoolEntryDesc() = default;

  MachineConstantPoolValue Value;
  std::uint64_t Size = 0;
  Alignment Align;
  ConstantPoolMergeKind Merge = ConstantPoolMergeKind::Never;
  ConstantPoolRelocationMode Relocation = ConstantPoolRelocationMode::None;
  ConstantPoolPlacement Placement;

  [[nodiscard]] ConstantPoolEntryKind kind() const noexcept;
  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<MachineConstantPoolEntryDesc> clone() const;
  [[nodiscard]] bool semanticallyEquals(const MachineConstantPoolEntryDesc &Other) const noexcept;
};

class MachineConstantPoolEntry final {
public:
  [[nodiscard]] ConstantPoolIndex index() const noexcept {
    return Index;
  }

  [[nodiscard]] bool isTombstone() const noexcept {
    return std::holds_alternative<std::monostate>(Description);
  }

  [[nodiscard]] const MachineConstantPoolEntryDesc *description() const noexcept {
    return std::get_if<MachineConstantPoolEntryDesc>(&Description);
  }

  [[nodiscard]] Expected<MachineConstantPoolEntryDesc> cloneDescription() const;

private:
  MachineConstantPoolEntry(ConstantPoolIndex Index, MachineConstantPoolEntryDesc Description)
      : Index(Index), Description(std::move(Description)) {}

  ConstantPoolIndex Index;
  std::variant<std::monostate, MachineConstantPoolEntryDesc> Description;

  friend class MachineConstantPool;
};

class MachineConstantPool final {
public:
  MachineConstantPool() = default;
  MachineConstantPool(const MachineConstantPool &) = delete;
  MachineConstantPool &operator=(const MachineConstantPool &) = delete;
  MachineConstantPool(MachineConstantPool &&) noexcept = default;
  MachineConstantPool &operator=(MachineConstantPool &&) noexcept = default;
  ~MachineConstantPool() = default;

  [[nodiscard]] Expected<ConstantPoolIndex> addEntry(MachineConstantPoolEntryDesc Description);
  [[nodiscard]] Expected<void> updateEntry(ConstantPoolIndex Index,
                                           MachineConstantPoolEntryDesc Description);
  [[nodiscard]] Expected<void> eraseEntry(ConstantPoolIndex Index);

  [[nodiscard]] const MachineConstantPoolEntry *entry(ConstantPoolIndex Index) const noexcept;
  [[nodiscard]] std::span<const MachineConstantPoolEntry> entries() const noexcept {
    return Entries;
  }

  [[nodiscard]] std::size_t liveEntryCount() const noexcept;

  [[nodiscard]] bool isFrozen() const noexcept {
    return Frozen;
  }

  [[nodiscard]] Expected<void> freeze();
  [[nodiscard]] Expected<void> verify() const;

private:
  [[nodiscard]] Expected<MachineConstantPoolEntry *> mutableEntry(ConstantPoolIndex Index);
  [[nodiscard]] const MachineConstantPoolEntry *
  findEquivalent(const MachineConstantPoolEntryDesc &Description,
                 std::optional<ConstantPoolIndex> Excluded = std::nullopt) const noexcept;

  std::vector<MachineConstantPoolEntry> Entries;
  bool Frozen = false;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECONSTANTPOOL_H
