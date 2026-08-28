// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETMCINSTLOWERING_H
#define RCC_CODEGEN_TARGETMCINSTLOWERING_H

#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCInstPrinter.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/Support/Error.h"

namespace rcc {

class MachineInstr;
class MachineMCContext;

class TargetMCInstLowering {
public:
  virtual ~TargetMCInstLowering() = default;

  [[nodiscard]] virtual const mc::MCInstPrinter &instructionPrinter() const noexcept = 0;

  [[nodiscard]] virtual Expected<mc::MCInst> lowerInstruction(const MachineInstr &Instruction,
                                                              MachineMCContext &Context) const = 0;

  [[nodiscard]] virtual Expected<mc::MCExprRef>
  lowerSymbolExpression(mc::MCExprRef Expression, std::uint32_t TargetFlags) const = 0;

  [[nodiscard]] virtual Expected<void> emitTerminalInstruction(const MachineInstr &Instruction,
                                                               MachineMCContext &Context,
                                                               mc::MCStreamer &Streamer) const;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETMCINSTLOWERING_H
