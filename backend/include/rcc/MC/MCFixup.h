// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCFIXUP_H
#define RCC_MC_MCFIXUP_H

#include "rcc/MC/MCExpr.h"
#include "rcc/Support/Error.h"

#include <cstdint>

namespace rcc::mc {

using MCFixupKind = std::uint16_t;

namespace fixup_kind {

inline constexpr MCFixupKind None = 0;
inline constexpr MCFixupKind Data1 = 1;
inline constexpr MCFixupKind Data2 = 2;
inline constexpr MCFixupKind Data4 = 3;
inline constexpr MCFixupKind Data8 = 4;
inline constexpr MCFixupKind PCRelative4 = 5;
inline constexpr MCFixupKind FirstTarget = 256;

} // namespace fixup_kind

struct MCFixup final {
  std::uint64_t Offset = 0;
  MCExprRef Value;
  MCFixupKind Kind = fixup_kind::None;
  bool PCRelative = false;
  bool LinkerRelaxable = false;

  [[nodiscard]] Expected<void> verify() const;
};

struct MCValue final {
  MCSymbolRef AddSymbol;
  MCSymbolRef SubtractSymbol;
  std::int64_t Addend = 0;
  std::uint32_t TargetKind = 0;

  [[nodiscard]] bool isAbsolute() const noexcept {
    return !AddSymbol && !SubtractSymbol;
  }
};

[[nodiscard]] Expected<MCValue> evaluateAsRelocatable(const MCExpr &Expression);

} // namespace rcc::mc

#endif // RCC_MC_MCFIXUP_H
