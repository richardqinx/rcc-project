// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCOBJECTWRITER_H
#define RCC_MC_MCOBJECTWRITER_H

#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rcc::mc {

enum class MCObjectSymbolBinding : std::uint8_t {
  Local,
  Global,
  Weak,
};

enum class MCObjectSymbolType : std::uint8_t {
  None,
  Function,
  Object,
  TLSObject,
  Section,
};

enum class MCObjectSymbolVisibility : std::uint8_t {
  Default,
  Hidden,
  Protected,
};

struct MCObjectRelocation final {
  std::uint64_t Offset = 0;
  std::uint32_t Type = 0;
  MCSymbolRef Symbol;
  std::int64_t Addend = 0;
};

struct MCObjectSection final {
  MCSectionRef Section;
  std::vector<std::uint8_t> Contents;
  std::uint64_t AddressSize = 0;
  std::uint64_t Alignment = 1;
  std::vector<MCObjectRelocation> Relocations;
};

struct MCObjectSymbol final {
  MCSymbolRef Symbol;
  MCObjectSymbolBinding Binding = MCObjectSymbolBinding::Local;
  MCObjectSymbolType Type = MCObjectSymbolType::None;
  MCObjectSymbolVisibility Visibility = MCObjectSymbolVisibility::Default;
  std::optional<std::size_t> SectionIndex;
  std::uint64_t Value = 0;
  std::uint64_t Size = 0;
  std::uint64_t CommonAlignment = 0;
};

struct MCAssembledObject final {
  std::vector<MCObjectSection> Sections;
  std::vector<MCObjectSymbol> Symbols;
  std::uint32_t Flags = 0;
};

class MCObjectWriter {
public:
  virtual ~MCObjectWriter() = default;

  [[nodiscard]] virtual Expected<std::vector<std::uint8_t>>
  writeObject(const MCAssembledObject &Object) const = 0;
};

} // namespace rcc::mc

#endif // RCC_MC_MCOBJECTWRITER_H
