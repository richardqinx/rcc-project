// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_SLOTINDEXES_H
#define RCC_CODEGEN_SLOTINDEXES_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class TargetDescription;

enum class SlotIndexKind : std::uint8_t {
  Block,
  EarlyClobber,
  Register,
  Dead,
};

class SlotIndex final {
public:
  constexpr SlotIndex() noexcept = default;

  [[nodiscard]] static constexpr SlotIndex fromValue(std::uint64_t Value) noexcept {
    return SlotIndex(Value);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr SlotIndexKind kind() const noexcept {
    return static_cast<SlotIndexKind>(Value & KindMask);
  }

  [[nodiscard]] constexpr SlotIndex baseIndex() const noexcept {
    return SlotIndex(Value & ~KindMask);
  }

  [[nodiscard]] constexpr SlotIndex withKind(SlotIndexKind Kind) const noexcept {
    return isValid() ? SlotIndex((Value & ~KindMask) | static_cast<std::uint64_t>(Kind))
                     : SlotIndex{};
  }

  [[nodiscard]] constexpr SlotIndex earlyClobberSlot() const noexcept {
    return withKind(SlotIndexKind::EarlyClobber);
  }

  [[nodiscard]] constexpr SlotIndex registerSlot() const noexcept {
    return withKind(SlotIndexKind::Register);
  }

  [[nodiscard]] constexpr SlotIndex deadSlot() const noexcept {
    return withKind(SlotIndexKind::Dead);
  }

  friend constexpr bool operator==(SlotIndex, SlotIndex) noexcept = default;
  friend constexpr auto operator<=>(SlotIndex, SlotIndex) noexcept = default;

private:
  static constexpr std::uint64_t KindMask = 3;

  explicit constexpr SlotIndex(std::uint64_t Value) noexcept : Value(Value) {}

  std::uint64_t Value = 0;
};

class SlotIndexesAnalysis final {
public:
  class Result final {
  public:
    [[nodiscard]] SlotIndex instructionIndex(const MachineInstr &Instruction) const noexcept;
    [[nodiscard]] SlotIndex slot(const MachineInstr &Instruction) const noexcept {
      return instructionIndex(Instruction);
    }
    [[nodiscard]] SlotIndex blockStart(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] SlotIndex blockEnd(const MachineBasicBlock &Block) const noexcept;
    [[nodiscard]] const MachineInstr *instructionFromIndex(SlotIndex Index) const noexcept;
    [[nodiscard]] const MachineBasicBlock *blockFromIndex(SlotIndex Index) const noexcept;

    [[nodiscard]] Expected<void> insertInstruction(const MachineInstr &Instruction);
    [[nodiscard]] Expected<void> eraseInstruction(const MachineInstr &Instruction);
    [[nodiscard]] Expected<void> renumber(MachineFunction &Function);

    [[nodiscard]] bool invalidate(const MachineFunction &,
                                  const PreservedAnalyses &Preserved) const noexcept;

  private:
    std::unordered_map<MachineInstrStableID, SlotIndex> InstructionSlots;
    std::unordered_map<std::uint64_t, const MachineInstr *> SlotToInstruction;
    std::unordered_map<const MachineBasicBlock *, SlotIndex> BlockStarts;
    std::unordered_map<const MachineBasicBlock *, SlotIndex> BlockEnds;
    std::vector<const MachineBasicBlock *> BlocksByStart;

    friend class SlotIndexesAnalysis;
  };

  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "SlotIndexes";
  }

  [[nodiscard]] static Expected<Result> run(MachineFunction &Function,
                                            const TargetDescription &Target,
                                            MachineFunctionAnalysisManager &Analyses);
};

} // namespace rcc

template <> struct std::hash<rcc::SlotIndex> {
  [[nodiscard]] std::size_t operator()(rcc::SlotIndex Index) const noexcept {
    return std::hash<std::uint64_t>{}(Index.value());
  }
};

#endif // RCC_CODEGEN_SLOTINDEXES_H
