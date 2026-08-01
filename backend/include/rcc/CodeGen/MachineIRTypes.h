// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEIRTYPES_H
#define RCC_CODEGEN_MACHINEIRTYPES_H

#include "rcc/Support/Error.h"

#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <type_traits>

namespace rcc {

template <typename TagT, typename ValueT = std::uint32_t> class MachineIndex final {
public:
  constexpr MachineIndex() noexcept = default;

  [[nodiscard]] static constexpr MachineIndex fromValue(ValueT Value) noexcept {
    return MachineIndex(Value);
  }

  [[nodiscard]] constexpr ValueT value() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(MachineIndex Left, MachineIndex Right) noexcept = default;
  friend constexpr auto operator<=>(MachineIndex Left, MachineIndex Right) noexcept = default;

private:
  explicit constexpr MachineIndex(ValueT Value) noexcept : Value(Value) {}

  ValueT Value = 0;
};

namespace detail {

struct FrameIndexTag;
struct ConstantPoolIndexTag;
struct JumpTableIndexTag;
struct TargetIndexTag;
struct SubRegIndexTag;
struct RegisterUnitIDTag;
struct RegisterBankIDTag;
struct RegisterClassIDTag;
struct SchedulingClassIDTag;
struct ImmediatePredicateIDTag;
struct NamedOperandIDTag;
struct TargetOperandTypeIDTag;
struct EncodingCategoryIDTag;
struct AssemblyFormatIDTag;
struct SyncScopeIDTag;
struct MemoryAliasSourceIDTag;
struct OriginIDTag;
struct TargetConstantTypeIDTag;
struct TargetFunctionStateTypeIDTag;
struct TargetRelocationKindIDTag;

} // namespace detail

using FrameIndex = MachineIndex<detail::FrameIndexTag>;
using ConstantPoolIndex = MachineIndex<detail::ConstantPoolIndexTag>;
using JumpTableIndex = MachineIndex<detail::JumpTableIndexTag>;
using TargetIndex = MachineIndex<detail::TargetIndexTag>;
using SubRegIndex = MachineIndex<detail::SubRegIndexTag>;
using RegisterUnitID = MachineIndex<detail::RegisterUnitIDTag>;
using RegisterBankID = MachineIndex<detail::RegisterBankIDTag>;
using RegisterClassID = MachineIndex<detail::RegisterClassIDTag>;
using SchedulingClassID = MachineIndex<detail::SchedulingClassIDTag>;
using ImmediatePredicateID = MachineIndex<detail::ImmediatePredicateIDTag>;
using NamedOperandID = MachineIndex<detail::NamedOperandIDTag>;
using TargetOperandTypeID = MachineIndex<detail::TargetOperandTypeIDTag>;
using EncodingCategoryID = MachineIndex<detail::EncodingCategoryIDTag>;
using AssemblyFormatID = MachineIndex<detail::AssemblyFormatIDTag>;
using SyncScopeID = MachineIndex<detail::SyncScopeIDTag>;
using MemoryAliasSourceID = MachineIndex<detail::MemoryAliasSourceIDTag, std::uint64_t>;
using OriginID = MachineIndex<detail::OriginIDTag, std::uint64_t>;
using TargetConstantTypeID = MachineIndex<detail::TargetConstantTypeIDTag, std::uint64_t>;
using TargetFunctionStateTypeID = MachineIndex<detail::TargetFunctionStateTypeIDTag, std::uint64_t>;
using TargetRelocationKindID = MachineIndex<detail::TargetRelocationKindIDTag>;

class Opcode final {
public:
  constexpr Opcode() noexcept = default;

  [[nodiscard]] static constexpr Opcode fromValue(std::uint32_t Value) noexcept {
    return Opcode(Value);
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(Opcode Left, Opcode Right) noexcept = default;

private:
  explicit constexpr Opcode(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = 0;
};

class LaneMask final {
public:
  constexpr LaneMask() noexcept = default;

  [[nodiscard]] static constexpr LaneMask fromRaw(std::uint64_t RawValue) noexcept {
    return LaneMask(RawValue);
  }

  [[nodiscard]] static constexpr LaneMask all() noexcept {
    return LaneMask(~std::uint64_t{0});
  }

  [[nodiscard]] constexpr bool empty() const noexcept {
    return RawValue == 0;
  }

  [[nodiscard]] constexpr bool intersects(LaneMask Other) const noexcept {
    return (RawValue & Other.RawValue) != 0;
  }

  [[nodiscard]] constexpr bool contains(LaneMask Other) const noexcept {
    return (RawValue & Other.RawValue) == Other.RawValue;
  }

  [[nodiscard]] constexpr std::uint64_t rawValue() const noexcept {
    return RawValue;
  }

  friend constexpr bool operator==(LaneMask Left, LaneMask Right) noexcept = default;

  friend constexpr LaneMask operator|(LaneMask Left, LaneMask Right) noexcept {
    return LaneMask(Left.RawValue | Right.RawValue);
  }

  friend constexpr LaneMask operator&(LaneMask Left, LaneMask Right) noexcept {
    return LaneMask(Left.RawValue & Right.RawValue);
  }

private:
  explicit constexpr LaneMask(std::uint64_t RawValue) noexcept : RawValue(RawValue) {}

  std::uint64_t RawValue = 0;
};

class BranchProbability final {
public:
  constexpr BranchProbability() noexcept = default;

  [[nodiscard]] static constexpr BranchProbability zero() noexcept {
    return BranchProbability(0);
  }

  [[nodiscard]] static constexpr BranchProbability one() noexcept {
    return BranchProbability(MaxRawValue);
  }

  [[nodiscard]] static constexpr BranchProbability fromRaw(std::uint32_t RawValue) noexcept {
    return BranchProbability(RawValue);
  }

  [[nodiscard]] static Expected<BranchProbability> fromFraction(std::uint32_t Numerator,
                                                                std::uint32_t Denominator);

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return RawValue;
  }

  [[nodiscard]] constexpr BranchProbability saturatingAdd(BranchProbability Other) const noexcept {
    if (MaxRawValue - RawValue < Other.RawValue)
      return one();
    return BranchProbability(RawValue + Other.RawValue);
  }

  [[nodiscard]] constexpr BranchProbability complement() const noexcept {
    return BranchProbability(MaxRawValue - RawValue);
  }

  friend constexpr bool operator==(BranchProbability Left,
                                   BranchProbability Right) noexcept = default;
  friend constexpr auto operator<=>(BranchProbability Left,
                                    BranchProbability Right) noexcept = default;

private:
  static constexpr std::uint32_t MaxRawValue = ~std::uint32_t{0};

  explicit constexpr BranchProbability(std::uint32_t RawValue) noexcept : RawValue(RawValue) {}

  std::uint32_t RawValue = 0;
};

enum class MachineInstrFlag : std::uint8_t {
  NoSignedWrap,
  NoUnsignedWrap,
  Exact,
  NoFPExcept,
  FastMathAllowReassoc,
  FastMathNoNaNs,
  FastMathNoInfs,
  FastMathNoSignedZeros,
  FastMathAllowReciprocal,
  FastMathAllowContract,
  FastMathApproxFunc,
  FrameSetup,
  FrameDestroy,
  BundledWithPred,
  BundledWithSucc,
  NoMerge,
  Unpredictable,
  Count,
};

class MachineInstrFlags final {
public:
  constexpr MachineInstrFlags() noexcept = default;

  MachineInstrFlags(std::initializer_list<MachineInstrFlag> Flags) noexcept;

  [[nodiscard]] constexpr bool has(MachineInstrFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index >= static_cast<std::uint32_t>(MachineInstrFlag::Count))
      return false;
    return (Mask & (std::uint32_t{1} << Index)) != 0;
  }

  constexpr void set(MachineInstrFlag Flag) noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index < static_cast<std::uint32_t>(MachineInstrFlag::Count))
      Mask |= std::uint32_t{1} << Index;
  }

  constexpr void clear(MachineInstrFlag Flag) noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index < static_cast<std::uint32_t>(MachineInstrFlag::Count))
      Mask &= ~(std::uint32_t{1} << Index);
  }

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(MachineInstrFlags Left,
                                   MachineInstrFlags Right) noexcept = default;

private:
  std::uint32_t Mask = 0;
};

struct SourceLocation final {
  std::string File;
  std::uint32_t Line = 0;
  std::uint32_t Column = 0;
  std::uint32_t Discriminator = 0;

  [[nodiscard]] bool isValid() const noexcept {
    return !File.empty() && Line != 0;
  }

  friend bool operator==(const SourceLocation &Left, const SourceLocation &Right) = default;
};

static_assert(sizeof(Opcode) == sizeof(std::uint32_t));
static_assert(sizeof(FrameIndex) == sizeof(std::uint32_t));
static_assert(sizeof(MemoryAliasSourceID) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<Opcode>);
static_assert(std::is_trivially_copyable_v<FrameIndex>);
static_assert(std::is_trivially_copyable_v<LaneMask>);
static_assert(std::is_trivially_copyable_v<BranchProbability>);

} // namespace rcc

template <typename TagT, typename ValueT> struct std::hash<rcc::MachineIndex<TagT, ValueT>> {
  [[nodiscard]] std::size_t operator()(rcc::MachineIndex<TagT, ValueT> Index) const noexcept {
    return std::hash<ValueT>{}(Index.value());
  }
};

template <> struct std::hash<rcc::Opcode> {
  [[nodiscard]] std::size_t operator()(rcc::Opcode Value) const noexcept {
    return std::hash<std::uint32_t>{}(Value.value());
  }
};

#endif // RCC_CODEGEN_MACHINEIRTYPES_H
