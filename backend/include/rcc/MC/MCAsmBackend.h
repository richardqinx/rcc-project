// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCASMBACKEND_H
#define RCC_MC_MCASMBACKEND_H

#include "rcc/MC/MCFixup.h"
#include "rcc/MC/MCInst.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rcc::mc {

struct MCFixupKindInfo final {
  std::uint8_t StorageSize = 0;
  bool PCRelative = false;
};

struct MCFixupRelocation final {
  std::uint32_t Type = 0;
  MCSymbolRef Symbol;
  std::int64_t Addend = 0;
};

class MCAsmBackend {
public:
  virtual ~MCAsmBackend() = default;

  [[nodiscard]] virtual Expected<MCFixupKindInfo> fixupKindInfo(MCFixupKind Kind) const = 0;
  [[nodiscard]] virtual Expected<void> applyFixup(MCFixupKind Kind, std::span<std::uint8_t> Data,
                                                  std::uint64_t Offset,
                                                  std::int64_t Value) const = 0;
  [[nodiscard]] virtual Expected<std::uint32_t> relocationType(const MCFixup &Fixup,
                                                               const MCValue &Value) const = 0;
  [[nodiscard]] virtual Expected<std::vector<MCFixupRelocation>>
  relocationsForFixup(const MCFixup &Fixup, const MCValue &Value) const;

  [[nodiscard]] virtual bool mayNeedRelaxation(const MCInst &) const noexcept {
    return false;
  }
  [[nodiscard]] virtual Expected<bool> fixupNeedsRelaxation(const MCFixup &, std::int64_t) const {
    return false;
  }
  [[nodiscard]] virtual Expected<MCInst> relaxInstruction(const MCInst &) const;

  [[nodiscard]] virtual std::uint32_t objectFlags() const noexcept {
    return 0;
  }
};

} // namespace rcc::mc

#endif // RCC_MC_MCASMBACKEND_H
