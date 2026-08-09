// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCSECTION_H
#define RCC_MC_MCSECTION_H

#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace rcc::mc {

enum class MCSectionKind : std::uint8_t {
  Text,
  ReadOnly,
  Data,
  BSS,
  TLSData,
  TLSBSS,
  EHFrame,
  Custom,
};

enum class MCSectionFlag : std::uint8_t {
  Alloc,
  Executable,
  Writable,
  Merge,
  Strings,
  TLS,
  Group,
  Retain,
  Count,
};

class MCSectionFlags final {
public:
  constexpr MCSectionFlags() noexcept = default;

  constexpr MCSectionFlags(std::initializer_list<MCSectionFlag> Flags) noexcept {
    for (const MCSectionFlag Flag : Flags)
      set(Flag);
  }

  constexpr void set(MCSectionFlag Flag) noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    if (Index < static_cast<std::uint8_t>(MCSectionFlag::Count))
      Mask |= std::uint16_t{1} << Index;
  }

  [[nodiscard]] constexpr bool has(MCSectionFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint8_t>(Flag);
    return Index < static_cast<std::uint8_t>(MCSectionFlag::Count) &&
           (Mask & (std::uint16_t{1} << Index)) != 0;
  }

  [[nodiscard]] constexpr std::uint16_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(MCSectionFlags Left, MCSectionFlags Right) noexcept = default;

private:
  std::uint16_t Mask = 0;
};

struct MCSectionDesc final {
  std::string Name;
  MCSectionKind Kind = MCSectionKind::Custom;
  MCSectionFlags Flags;
  std::uint64_t Alignment = 1;
  std::uint64_t EntrySize = 0;
  MCSymbolRef Group;
  std::optional<std::uint64_t> UniqueID;

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const MCSectionDesc &Left, const MCSectionDesc &Right) = default;
};

class MCSection final {
public:
  [[nodiscard]] std::string_view name() const noexcept {
    return Description.Name;
  }

  [[nodiscard]] const MCSectionDesc &description() const noexcept {
    return Description;
  }

private:
  explicit MCSection(MCSectionDesc Description) : Description(std::move(Description)) {}

  MCSectionDesc Description;

  friend class MCContext;
};

using MCSectionRef = std::shared_ptr<const MCSection>;

} // namespace rcc::mc

#endif // RCC_MC_MCSECTION_H
