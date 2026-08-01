// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFRAMEINFO_H
#define RCC_CODEGEN_MACHINEFRAMEINFO_H

#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

enum class StackObjectKind : std::uint8_t {
  Default,
  FixedABI,
  SpillSlot,
  VariableSized,
  DynamicAlloca,
  CalleeSaved,
  EmergencySpill,
  Target,
  Count,
};

class StackID final {
public:
  constexpr StackID() noexcept = default;

  [[nodiscard]] static constexpr StackID defaultStack() noexcept {
    return StackID(DefaultStackValue);
  }

  [[nodiscard]] static constexpr StackID safeStack() noexcept {
    return StackID(SafeStackValue);
  }

  [[nodiscard]] static constexpr StackID shadowCallStack() noexcept {
    return StackID(ShadowCallStackValue);
  }

  [[nodiscard]] static constexpr StackID scalableVectorStack() noexcept {
    return StackID(ScalableVectorStackValue);
  }

  [[nodiscard]] static Expected<StackID> targetStack(std::uint32_t Value);

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr bool isTargetStack() const noexcept {
    return Value >= FirstTargetStackValue;
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value <= ScalableVectorStackValue || isTargetStack();
  }

  friend constexpr bool operator==(StackID Left, StackID Right) noexcept = default;

private:
  static constexpr std::uint32_t DefaultStackValue = 0;
  static constexpr std::uint32_t SafeStackValue = 1;
  static constexpr std::uint32_t ShadowCallStackValue = 2;
  static constexpr std::uint32_t ScalableVectorStackValue = 3;
  static constexpr std::uint32_t FirstTargetStackValue = 256;

  explicit constexpr StackID(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = DefaultStackValue;
};

struct StackObjectLifetimeRange final {
  MachineInstrStableID Begin;
  MachineInstrStableID End;

  friend constexpr bool operator==(StackObjectLifetimeRange Left,
                                   StackObjectLifetimeRange Right) noexcept = default;
};

struct StackObjectDesc final {
  StackObjectKind Kind = StackObjectKind::Default;
  std::optional<std::uint64_t> Size;
  Alignment Align;
  std::optional<std::int64_t> Offset;
  StackID Stack;
  bool IsDead = false;
  bool IsAliased = false;
  bool AddressTaken = false;
  std::vector<StackObjectLifetimeRange> Lifetime;
  std::optional<MachineVariableDebugInfo> DebugInfo;
  std::uint64_t TargetFlags = 0;

  friend bool operator==(const StackObjectDesc &Left, const StackObjectDesc &Right) = default;
};

class StackObject final {
public:
  [[nodiscard]] FrameIndex index() const noexcept {
    return Index;
  }

  [[nodiscard]] bool isTombstone() const noexcept {
    return std::holds_alternative<std::monostate>(Description);
  }

  [[nodiscard]] const StackObjectDesc *description() const noexcept {
    return std::get_if<StackObjectDesc>(&Description);
  }

  [[nodiscard]] bool isSpill() const noexcept;
  [[nodiscard]] bool isFixed() const noexcept;
  [[nodiscard]] bool isVariableSized() const noexcept;

private:
  StackObject(FrameIndex Index, StackObjectDesc Description)
      : Index(Index), Description(std::move(Description)) {}

  FrameIndex Index;
  std::variant<std::monostate, StackObjectDesc> Description;

  friend class MachineFrameInfo;
};

class MachineFrameInfo final {
public:
  MachineFrameInfo() = default;
  MachineFrameInfo(const MachineFrameInfo &) = delete;
  MachineFrameInfo &operator=(const MachineFrameInfo &) = delete;
  MachineFrameInfo(MachineFrameInfo &&) noexcept = default;
  MachineFrameInfo &operator=(MachineFrameInfo &&) noexcept = default;
  ~MachineFrameInfo() = default;

  [[nodiscard]] Expected<FrameIndex> createObject(StackObjectDesc Description);
  [[nodiscard]] Expected<void> updateObject(FrameIndex Index, StackObjectDesc Description);
  [[nodiscard]] Expected<void> setObjectOffset(FrameIndex Index, std::int64_t Offset);
  [[nodiscard]] Expected<void> markObjectDead(FrameIndex Index);
  [[nodiscard]] Expected<void> eraseObject(FrameIndex Index);

  [[nodiscard]] const StackObject *object(FrameIndex Index) const noexcept;
  [[nodiscard]] std::span<const StackObject> objects() const noexcept {
    return Objects;
  }

  [[nodiscard]] std::size_t liveObjectCount() const noexcept;

  [[nodiscard]] bool isFrozen() const noexcept {
    return Frozen;
  }

  [[nodiscard]] std::optional<std::uint64_t> staticStackSize() const noexcept {
    return StaticStackSize;
  }

  [[nodiscard]] Alignment stackAlignment() const noexcept {
    return StackAlign;
  }

  [[nodiscard]] std::uint64_t maxCallFrameSize() const noexcept {
    return MaxCallFrameSize;
  }

  [[nodiscard]] Expected<void> setMaxCallFrameSize(std::uint64_t Size);
  [[nodiscard]] Expected<void> freeze(std::uint64_t Size, Alignment Align);
  [[nodiscard]] Expected<void> verify() const;

private:
  [[nodiscard]] static Expected<void> verifyDescription(const StackObjectDesc &Description);
  [[nodiscard]] Expected<StackObject *> mutableObject(FrameIndex Index);

  std::vector<StackObject> Objects;
  bool Frozen = false;
  std::optional<std::uint64_t> StaticStackSize;
  Alignment StackAlign;
  std::uint64_t MaxCallFrameSize = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFRAMEINFO_H
