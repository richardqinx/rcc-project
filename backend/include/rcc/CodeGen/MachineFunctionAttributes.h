// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEFUNCTIONATTRIBUTES_H
#define RCC_CODEGEN_MACHINEFUNCTIONATTRIBUTES_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace rcc {

enum class MachineFunctionAttribute : std::uint8_t {
  ExposesReturnsTwice,
  HasWinCFI,
  HasFakeUses,
  CallsEHReturn,
  CallsUnwindInit,
  HasEHContTarget,
  HasEHScopes,
  HasEHFunclets,
  IsOutlined,
  DebugInstrRef,
  TracksDebugUserValues,
  Count,
};

[[nodiscard]] std::string_view
getMachineFunctionAttributeName(MachineFunctionAttribute Attribute) noexcept;

class MachineFunctionAttributes final {
public:
  constexpr MachineFunctionAttributes() noexcept = default;

  [[nodiscard]] static Expected<MachineFunctionAttributes> fromRawFlags(std::uint16_t RawFlags);

  [[nodiscard]] constexpr bool has(MachineFunctionAttribute Attribute) const noexcept {
    if (!isValidAttribute(Attribute))
      return false;
    return (Flags & flagFor(Attribute)) != 0;
  }

  constexpr void set(MachineFunctionAttribute Attribute) noexcept {
    if (isValidAttribute(Attribute))
      Flags |= flagFor(Attribute);
  }

  constexpr void clear(MachineFunctionAttribute Attribute) noexcept {
    if (isValidAttribute(Attribute))
      Flags &= static_cast<std::uint16_t>(~flagFor(Attribute));
  }

  [[nodiscard]] constexpr std::uint16_t rawFlags() const noexcept {
    return Flags;
  }

  [[nodiscard]] std::optional<Alignment> alignment() const noexcept {
    return FunctionAlignment;
  }

  void setAlignment(Alignment NewAlignment) noexcept {
    FunctionAlignment = NewAlignment;
  }

  void clearAlignment() noexcept {
    FunctionAlignment.reset();
  }

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const MachineFunctionAttributes &Left,
                         const MachineFunctionAttributes &Right) = default;

private:
  static constexpr std::uint16_t AttributeCount =
      static_cast<std::uint16_t>(MachineFunctionAttribute::Count);
  static_assert(AttributeCount < 16);

  static constexpr std::uint16_t ValidFlags =
      static_cast<std::uint16_t>((std::uint16_t{1} << AttributeCount) - 1);

  [[nodiscard]] static constexpr bool
  isValidAttribute(MachineFunctionAttribute Attribute) noexcept {
    return static_cast<std::uint16_t>(Attribute) < AttributeCount;
  }

  [[nodiscard]] static constexpr std::uint16_t
  flagFor(MachineFunctionAttribute Attribute) noexcept {
    return static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint16_t>(Attribute));
  }

  std::uint16_t Flags = 0;
  std::optional<Alignment> FunctionAlignment;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEFUNCTIONATTRIBUTES_H
