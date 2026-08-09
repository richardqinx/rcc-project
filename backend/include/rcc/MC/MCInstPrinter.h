// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCINSTPRINTER_H
#define RCC_MC_MCINSTPRINTER_H

#include "rcc/MC/MCInst.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace rcc::mc {

class MCInstPrinter {
public:
  virtual ~MCInstPrinter() = default;

  [[nodiscard]] virtual Expected<std::string> printInstruction(const MCInst &Instruction) const = 0;

  [[nodiscard]] virtual Expected<std::string>
  printTargetExpression(std::uint32_t Kind, std::string_view Operand) const = 0;
};

[[nodiscard]] Expected<std::string> printMCExpression(const MCExpr &Expression,
                                                      const MCInstPrinter &Printer);
[[nodiscard]] Expected<std::string> printMCSymbolName(std::string_view Name);

} // namespace rcc::mc

#endif // RCC_MC_MCINSTPRINTER_H
