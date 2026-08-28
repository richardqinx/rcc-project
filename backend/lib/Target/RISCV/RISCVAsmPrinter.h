// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVASMPRINTER_H
#define RCC_TARGET_RISCV_RISCVASMPRINTER_H

#include "rcc/CodeGen/AsmPrinter.h"

namespace rcc::riscv {

class RISCVAsmPrinter final : public AsmPrinter {
public:
  RISCVAsmPrinter(const TargetDescription &Target, const TargetMCInstLowering &Lowering,
                  const TargetLoweringObjectFile &ObjectFile, DataLayout Layout,
                  MachineMCContext &Context, mc::MCStreamer &Streamer,
                  AsmPrinterOptions Options = {}) noexcept
      : AsmPrinter(Target, Lowering, ObjectFile, std::move(Layout), Context, Streamer, Options) {}

private:
  [[nodiscard]] Expected<void> emitStartOfAsmFile() override;
  [[nodiscard]] Expected<void> emitEndOfAsmFile() override;
  [[nodiscard]] Expected<void> emitTargetConstant(const TargetConstant &Constant) override;
  [[nodiscard]] Expected<void> emitTargetJumpTable(const MachineFunction &Function,
                                                   const MachineJumpTableEntry &Entry) override;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVASMPRINTER_H
