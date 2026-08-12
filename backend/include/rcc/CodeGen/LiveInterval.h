// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LIVEINTERVAL_H
#define RCC_CODEGEN_LIVEINTERVAL_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/SlotIndexes.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace rcc {

class LiveValueNumberID final {
public:
  constexpr LiveValueNumberID() noexcept = default;

  [[nodiscard]] static constexpr LiveValueNumberID fromValue(std::uint32_t Value) noexcept {
    return LiveValueNumberID(Value);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(LiveValueNumberID, LiveValueNumberID) noexcept = default;
  friend constexpr auto operator<=>(LiveValueNumberID, LiveValueNumberID) noexcept = default;

private:
  explicit constexpr LiveValueNumberID(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = 0;
};

enum class LiveValueState : std::uint8_t {
  Normal,
  Phi,
  Unused,
  Redefined,
};

struct LiveValueNumber final {
  LiveValueNumberID ID;
  SlotIndex Definition;
  LiveValueState State = LiveValueState::Normal;
  std::optional<MachineInstrStableID> Origin;

  friend bool operator==(const LiveValueNumber &, const LiveValueNumber &) = default;
};

struct LiveSegment final {
  SlotIndex Start;
  SlotIndex End;
  LiveValueNumberID Value;

  [[nodiscard]] constexpr bool empty() const noexcept {
    return !(Start < End);
  }

  [[nodiscard]] constexpr bool contains(SlotIndex Position) const noexcept {
    return !(Position < Start) && Position < End;
  }

  [[nodiscard]] constexpr bool overlaps(const LiveSegment &Other) const noexcept {
    return Start < Other.End && Other.Start < End;
  }

  friend constexpr bool operator==(const LiveSegment &, const LiveSegment &) noexcept = default;
};

class LiveRange final {
public:
  [[nodiscard]] std::span<const LiveSegment> segments() const noexcept {
    return Segments;
  }
  [[nodiscard]] std::span<const LiveValueNumber> values() const noexcept {
    return Values;
  }
  [[nodiscard]] bool empty() const noexcept {
    return Segments.empty();
  }

  [[nodiscard]] const LiveSegment *segmentAt(SlotIndex Position) const noexcept;
  [[nodiscard]] const LiveValueNumber *value(LiveValueNumberID ID) const noexcept;
  [[nodiscard]] const LiveValueNumber *valueAt(SlotIndex Position) const noexcept;
  [[nodiscard]] bool liveAt(SlotIndex Position) const noexcept;
  [[nodiscard]] bool overlaps(const LiveRange &Other) const noexcept;

  [[nodiscard]] Expected<LiveValueNumberID>
  createValue(SlotIndex Definition, LiveValueState State = LiveValueState::Normal,
              std::optional<MachineInstrStableID> Origin = std::nullopt);
  [[nodiscard]] Expected<void> addSegment(LiveSegment Segment);
  [[nodiscard]] Expected<void> extend(LiveValueNumberID Value, SlotIndex Start, SlotIndex End);
  [[nodiscard]] Expected<void> remove(SlotIndex Start, SlotIndex End);
  [[nodiscard]] Expected<void> shrinkToUses(std::span<const SlotIndex> Uses);
  [[nodiscard]] Expected<void> verify() const;

private:
  void mergeAdjacent();

  std::vector<LiveSegment> Segments;
  std::vector<LiveValueNumber> Values;
};

struct LiveSubRange final {
  LaneMask Lanes;
  LiveRange Range;
};

struct LiveInterval final {
  Register Reg;
  LiveRange MainRange;
  std::vector<LiveSubRange> SubRanges;
  std::uint64_t SpillWeight = 0;
  bool Spillable = true;
  std::vector<PhysicalRegister> AllocationHints;
  std::optional<Register> SplitParent;
  std::optional<Register> Origin;

  [[nodiscard]] bool overlaps(const LiveInterval &Other) const noexcept {
    return MainRange.overlaps(Other.MainRange);
  }
};

} // namespace rcc

template <> struct std::hash<rcc::LiveValueNumberID> {
  [[nodiscard]] std::size_t operator()(rcc::LiveValueNumberID ID) const noexcept {
    return std::hash<std::uint32_t>{}(ID.value());
  }
};

#endif // RCC_CODEGEN_LIVEINTERVAL_H
