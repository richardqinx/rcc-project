// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCSYMBOL_H
#define RCC_MC_MCSYMBOL_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rcc::mc {

class MCContext;

enum class MCSymbolKind : std::uint8_t {
  Generic,
  Function,
  Object,
  BasicBlock,
  ConstantPool,
  JumpTable,
  LiteralIsland,
  Section,
  Temporary,
};

class MCSymbol final {
public:
  [[nodiscard]] std::string_view name() const noexcept {
    return Name;
  }

  [[nodiscard]] MCSymbolKind kind() const noexcept {
    return Kind;
  }

  [[nodiscard]] bool isTemporary() const noexcept {
    return Kind == MCSymbolKind::Temporary || Kind == MCSymbolKind::BasicBlock ||
           Kind == MCSymbolKind::ConstantPool || Kind == MCSymbolKind::JumpTable ||
           Kind == MCSymbolKind::LiteralIsland;
  }

private:
  MCSymbol(std::string Name, MCSymbolKind Kind) : Name(std::move(Name)), Kind(Kind) {}

  std::string Name;
  MCSymbolKind Kind;

  friend class MCContext;
};

using MCSymbolRef = std::shared_ptr<const MCSymbol>;

} // namespace rcc::mc

#endif // RCC_MC_MCSYMBOL_H
