// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFUNCTIONPROPERTIES_H
#define RCC_CODEGEN_MACHINEFUNCTIONPROPERTIES_H

#include "rcc/Support/Error.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>

namespace rcc {

enum class MachineFunctionProperty : std::uint8_t {
  IsSSA,
  NoPHIs,
  TracksLiveness,
  Legalized,
  RegBankSelected,
  Selected,
  TiedOpsRewritten,
  NoVRegs,
  FrameFinalized,
  NoFrameIndices,
  NoExecutablePseudos,
  // These two properties mirror LLVM MIR pipeline state. They are deliberately
  // separate from RCC's MachineCompilationStatus, which records a concrete run.
  FailedISel,
  FailsVerification,
  Count,
};

[[nodiscard]] std::string_view
getMachineFunctionPropertyName(MachineFunctionProperty Property) noexcept;

class MachineFunctionProperties final {
public:
  constexpr MachineFunctionProperties() noexcept = default;

  MachineFunctionProperties(std::initializer_list<MachineFunctionProperty> Properties) noexcept;

  [[nodiscard]] static Expected<MachineFunctionProperties> fromRaw(std::uint64_t RawValue);

  [[nodiscard]] constexpr bool empty() const noexcept {
    return Mask == 0;
  }

  [[nodiscard]] constexpr std::size_t count() const noexcept {
    return static_cast<std::size_t>(std::popcount(Mask));
  }

  [[nodiscard]] constexpr bool has(MachineFunctionProperty Property) const noexcept {
    if (!isValidPropertyValue(Property))
      return false;
    return (Mask & maskFor(Property)) != 0;
  }

  [[nodiscard]] constexpr bool containsAll(MachineFunctionProperties Other) const noexcept {
    return (Mask & Other.Mask) == Other.Mask;
  }

  [[nodiscard]] constexpr bool intersects(MachineFunctionProperties Other) const noexcept {
    return (Mask & Other.Mask) != 0;
  }

  [[nodiscard]] constexpr MachineFunctionProperties
  missingFrom(MachineFunctionProperties Available) const noexcept {
    return MachineFunctionProperties(Mask & ~Available.Mask);
  }

  constexpr void set(MachineFunctionProperty Property) noexcept {
    if (isValidPropertyValue(Property))
      Mask |= maskFor(Property);
  }

  constexpr void clear(MachineFunctionProperty Property) noexcept {
    if (isValidPropertyValue(Property))
      Mask &= ~maskFor(Property);
  }

  [[nodiscard]] Expected<void> applyTransition(MachineFunctionProperties Required,
                                               MachineFunctionProperties Sets,
                                               MachineFunctionProperties Clears);

  [[nodiscard]] constexpr std::uint64_t rawValue() const noexcept {
    return Mask;
  }

  [[nodiscard]] std::string toString() const;

  friend constexpr bool operator==(MachineFunctionProperties Left,
                                   MachineFunctionProperties Right) noexcept = default;

  friend constexpr MachineFunctionProperties operator|(MachineFunctionProperties Left,
                                                       MachineFunctionProperties Right) noexcept {
    return MachineFunctionProperties(Left.Mask | Right.Mask);
  }

  friend constexpr MachineFunctionProperties operator&(MachineFunctionProperties Left,
                                                       MachineFunctionProperties Right) noexcept {
    return MachineFunctionProperties(Left.Mask & Right.Mask);
  }

private:
  static constexpr std::uint64_t PropertyCount =
      static_cast<std::uint64_t>(MachineFunctionProperty::Count);
  static_assert(PropertyCount < 64);

  static constexpr std::uint64_t ValidMask = (std::uint64_t{1} << PropertyCount) - 1;

  explicit constexpr MachineFunctionProperties(std::uint64_t Mask) noexcept : Mask(Mask) {}

  [[nodiscard]] static constexpr bool
  isValidPropertyValue(MachineFunctionProperty Property) noexcept {
    return static_cast<std::uint64_t>(Property) < PropertyCount;
  }

  [[nodiscard]] static constexpr std::uint64_t maskFor(MachineFunctionProperty Property) noexcept {
    return std::uint64_t{1} << static_cast<std::uint64_t>(Property);
  }

  std::uint64_t Mask = 0;
};

static_assert(sizeof(MachineFunctionProperties) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<MachineFunctionProperties>);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTIONPROPERTIES_H
