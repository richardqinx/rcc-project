// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEJUMPTABLEINFO_H
#define RCC_CODEGEN_MACHINEJUMPTABLEINFO_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;

enum class JumpTableEntryEncoding : std::uint8_t {
  BlockAddress,
  GPRelative64BlockAddress,
  GPRelative32BlockAddress,
  LabelDifference32,
  LabelDifference64,
  Inline,
  Custom32,
  Target,
  Count,
};

struct JumpTablePlacement final {
  bool InTextSection = true;
  bool UniqueSection = false;
  std::optional<std::uint64_t> MaximumDistance;
  std::uint64_t TargetFlags = 0;

  friend bool operator==(const JumpTablePlacement &Left, const JumpTablePlacement &Right) = default;
};

struct MachineJumpTableDesc final {
  JumpTableEntryEncoding Encoding = JumpTableEntryEncoding::BlockAddress;
  std::uint8_t EntrySize = 8;
  Alignment Align;
  std::vector<MachineBasicBlock *> Targets;
  JumpTablePlacement Placement;
  std::uint32_t TargetEncoding = 0;

  friend bool operator==(const MachineJumpTableDesc &Left,
                         const MachineJumpTableDesc &Right) = default;
};

class MachineJumpTableEntry final {
public:
  [[nodiscard]] JumpTableIndex index() const noexcept {
    return Index;
  }

  [[nodiscard]] bool isTombstone() const noexcept {
    return std::holds_alternative<std::monostate>(Description);
  }

  [[nodiscard]] bool isPending() const noexcept {
    return std::holds_alternative<PendingDescription>(Description);
  }

  [[nodiscard]] const MachineJumpTableDesc *description() const noexcept {
    return std::get_if<MachineJumpTableDesc>(&Description);
  }

private:
  struct PendingDescription final {};

  MachineJumpTableEntry(JumpTableIndex Index, MachineJumpTableDesc Description)
      : Index(Index), Description(std::move(Description)) {}
  explicit MachineJumpTableEntry(JumpTableIndex Index)
      : Index(Index), Description(PendingDescription{}) {}

  JumpTableIndex Index;
  std::variant<std::monostate, PendingDescription, MachineJumpTableDesc> Description;

  friend class MachineJumpTableInfo;
};

class MachineJumpTableInfo final {
public:
  explicit MachineJumpTableInfo(MachineFunction &Owner) noexcept : Owner(&Owner) {}

  MachineJumpTableInfo(const MachineJumpTableInfo &) = delete;
  MachineJumpTableInfo &operator=(const MachineJumpTableInfo &) = delete;
  MachineJumpTableInfo(MachineJumpTableInfo &&) = delete;
  MachineJumpTableInfo &operator=(MachineJumpTableInfo &&) = delete;
  ~MachineJumpTableInfo() = default;

  [[nodiscard]] Expected<JumpTableIndex> createTable(MachineJumpTableDesc Description);
  [[nodiscard]] Expected<JumpTableIndex> reserveTable();
  [[nodiscard]] Expected<void> defineReservedTable(JumpTableIndex Index,
                                                   MachineJumpTableDesc Description);
  [[nodiscard]] Expected<void> updateTable(JumpTableIndex Index, MachineJumpTableDesc Description);
  [[nodiscard]] Expected<void> eraseTable(JumpTableIndex Index);

  [[nodiscard]] const MachineJumpTableEntry *table(JumpTableIndex Index) const noexcept;
  [[nodiscard]] std::span<const MachineJumpTableEntry> tables() const noexcept {
    return Tables;
  }

  [[nodiscard]] std::size_t liveTableCount() const noexcept;

  [[nodiscard]] bool isFrozen() const noexcept {
    return Frozen;
  }

  [[nodiscard]] Expected<void> freeze();
  [[nodiscard]] Expected<void> verify() const;

private:
  [[nodiscard]] Expected<void> verifyDescription(const MachineJumpTableDesc &Description) const;
  [[nodiscard]] Expected<MachineJumpTableEntry *> mutableTable(JumpTableIndex Index);

  MachineFunction *Owner;
  std::vector<MachineJumpTableEntry> Tables;
  bool Frozen = false;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEJUMPTABLEINFO_H
