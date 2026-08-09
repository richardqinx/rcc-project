// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCCFI_H
#define RCC_MC_MCCFI_H

#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <vector>

namespace rcc::mc {

enum class MCCFIOperation : std::uint8_t {
  StartProc,
  EndProc,
  Sections,
  Personality,
  LSDA,
  SignalFrame,
  ReturnColumn,
  SameValue,
  RememberState,
  RestoreState,
  Offset,
  DefCFARegister,
  DefCFAOffset,
  DefCFA,
  RelativeOffset,
  AdjustCFAOffset,
  Escape,
  Restore,
  Undefined,
  Register,
  WindowSave,
  NegateReturnAddressState,
  NegateReturnAddressStateWithPC,
  GNUArgsSize,
  Label,
  ValueOffset,
};

struct MCCFIInstruction final {
  MCCFIOperation Operation = MCCFIOperation::StartProc;
  std::vector<std::int64_t> Operands;
  std::vector<std::uint8_t> Bytes;
  MCSymbolRef Symbol;

  [[nodiscard]] Expected<void> verify() const;
};

} // namespace rcc::mc

#endif // RCC_MC_MCCFI_H
