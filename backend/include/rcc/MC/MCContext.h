// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCCONTEXT_H
#define RCC_MC_MCCONTEXT_H

#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rcc::mc {

class MCContext final {
public:
  [[nodiscard]] Expected<MCSymbolRef> getOrCreateSymbol(std::string Name, MCSymbolKind Kind);
  [[nodiscard]] Expected<MCSymbolRef> createTemporarySymbol(std::string_view Prefix);
  [[nodiscard]] MCSymbolRef symbol(std::string_view Name) const noexcept;

  [[nodiscard]] Expected<MCSectionRef> getOrCreateSection(MCSectionDesc Description);
  [[nodiscard]] MCSectionRef section(std::string_view Name) const noexcept;

private:
  struct StringHash final {
    using is_transparent = void; // NOLINT(readability-identifier-naming)

    [[nodiscard]] std::size_t operator()(std::string_view Value) const noexcept {
      return std::hash<std::string_view>{}(Value);
    }
  };

  std::unordered_map<std::string, MCSymbolRef, StringHash, std::equal_to<>> Symbols;
  std::unordered_map<std::string, MCSectionRef, StringHash, std::equal_to<>> Sections;
  std::uint64_t NextTemporarySymbol = 0;
};

} // namespace rcc::mc

#endif // RCC_MC_MCCONTEXT_H
