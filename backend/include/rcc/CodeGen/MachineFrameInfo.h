// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFRAMEINFO_H
#define RCC_CODEGEN_MACHINEFRAMEINFO_H

#include "rcc/CodeGen/MachineDebugInfo.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

enum class StackObjectKind : std::uint8_t {
  Default,
  FixedABI,
  FixedSpillSlot,
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

  [[nodiscard]] static constexpr StackID sgprSpillStack() noexcept {
    return StackID(SGPRSpillStackValue);
  }

  [[nodiscard]] static constexpr StackID scalableVectorStack() noexcept {
    return StackID(ScalableVectorStackValue);
  }

  [[nodiscard]] static constexpr StackID wasmLocalStack() noexcept {
    return StackID(WasmLocalStackValue);
  }

  [[nodiscard]] static constexpr StackID scalablePredicateVectorStack() noexcept {
    return StackID(ScalablePredicateVectorStackValue);
  }

  [[nodiscard]] static constexpr StackID noAllocStack() noexcept {
    return StackID(NoAllocStackValue);
  }

  [[nodiscard]] static Expected<StackID> fromValue(std::uint32_t Value);

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr bool isSpecialStack() const noexcept {
    return Value != DefaultStackValue;
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value <= ScalablePredicateVectorStackValue || Value == NoAllocStackValue;
  }

  friend constexpr bool operator==(StackID Left, StackID Right) noexcept = default;

private:
  static constexpr std::uint32_t DefaultStackValue = 0;
  static constexpr std::uint32_t SGPRSpillStackValue = 1;
  static constexpr std::uint32_t ScalableVectorStackValue = 2;
  static constexpr std::uint32_t WasmLocalStackValue = 3;
  static constexpr std::uint32_t ScalablePredicateVectorStackValue = 4;
  static constexpr std::uint32_t NoAllocStackValue = 255;

  explicit constexpr StackID(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = DefaultStackValue;
};

[[nodiscard]] std::optional<StackID> parseStackIDName(std::string_view Name) noexcept;
[[nodiscard]] std::string_view getStackIDName(StackID ID) noexcept;

struct StackObjectLifetimeRange final {
  MachineInstrStableID Begin;
  MachineInstrStableID End;

  friend constexpr bool operator==(StackObjectLifetimeRange Left,
                                   StackObjectLifetimeRange Right) noexcept = default;
};

struct StackObjectDesc final {
  StackObjectKind Kind = StackObjectKind::Default;
  std::optional<std::string> AllocaName;
  std::optional<std::uint64_t> Size;
  Alignment Align;
  std::optional<std::int64_t> Offset;
  StackID Stack;
  bool IsDead = false;
  bool IsImmutable = false;
  bool IsAliased = false;
  bool AddressTaken = false;
  std::optional<PhysicalRegister> CalleeSavedRegister;
  bool CalleeSavedRestored = true;
  std::optional<std::int64_t> LocalOffset;
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

enum class MachineFramePointerPolicy : std::uint8_t {
  None,
  NonLeaf,
  All,
  Reserved,
  NonLeafNoReserve,
  Count,
};

[[nodiscard]] std::optional<MachineFramePointerPolicy>
parseMachineFramePointerPolicy(std::string_view Name) noexcept;
[[nodiscard]] std::string_view
getMachineFramePointerPolicyName(MachineFramePointerPolicy Policy) noexcept;

struct MachineFrameSaveRestorePoint final {
  MachineBlockStableID Block;
  std::vector<PhysicalRegister> Registers;

  friend bool operator==(const MachineFrameSaveRestorePoint &Left,
                         const MachineFrameSaveRestorePoint &Right) = default;
};

struct MachineFrameState final {
  bool IsFrameAddressTaken = false;
  bool IsReturnAddressTaken = false;
  bool HasStackMap = false;
  bool HasPatchPoint = false;
  std::uint64_t StackSize = 0;
  std::int32_t OffsetAdjustment = 0;
  std::optional<Alignment> MaxAlignment;
  bool AdjustsStack = false;
  bool HasCalls = false;
  MachineFramePointerPolicy FramePointerPolicy = MachineFramePointerPolicy::None;
  std::optional<FrameIndex> StackProtector;
  std::optional<FrameIndex> FunctionContext;
  std::optional<std::uint64_t> MaxCallFrameSize;
  std::uint32_t CVBytesOfCalleeSavedRegisters = 0;
  bool HasOpaqueSPAdjustment = false;
  bool HasVAStart = false;
  bool HasMustTailInVarArgFunc = false;
  bool HasTailCall = false;
  bool IsCalleeSavedInfoValid = false;
  std::uint32_t LocalFrameSize = 0;
  std::vector<MachineFrameSaveRestorePoint> SavePoints;
  std::vector<MachineFrameSaveRestorePoint> RestorePoints;

  friend bool operator==(const MachineFrameState &Left,
                         const MachineFrameState &Right) = default;
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

  [[nodiscard]] const MachineFrameState &state() const noexcept {
    return State;
  }

  [[nodiscard]] std::optional<std::uint64_t> staticStackSize() const noexcept {
    if (!Frozen)
      return std::nullopt;
    return State.StackSize;
  }

  [[nodiscard]] Alignment stackAlignment() const noexcept {
    return State.MaxAlignment.value_or(Alignment{});
  }

  [[nodiscard]] std::optional<std::uint64_t> maxCallFrameSize() const noexcept {
    return State.MaxCallFrameSize;
  }

  [[nodiscard]] Expected<void> setState(MachineFrameState NewState);
  [[nodiscard]] Expected<void> setMaxCallFrameSize(std::uint64_t Size);
  [[nodiscard]] Expected<void> clearMaxCallFrameSize();
  [[nodiscard]] Expected<void> freeze(std::uint64_t Size, Alignment Align);
  [[nodiscard]] Expected<void> verify() const;

private:
  [[nodiscard]] static Expected<void> verifyDescription(const StackObjectDesc &Description);
  [[nodiscard]] static Expected<void> verifyStateStructure(const MachineFrameState &State);
  [[nodiscard]] Expected<StackObject *> mutableObject(FrameIndex Index);

  std::vector<StackObject> Objects;
  MachineFrameState State;
  bool Frozen = false;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFRAMEINFO_H
