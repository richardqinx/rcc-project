// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVMCTARGETLOWERING_H
#define RCC_TARGET_RISCV_RISCVMCTARGETLOWERING_H

#include "rcc/CodeGen/TargetMCInstLowering.h"
#include "rcc/Target/RISCV/RISCVInstPrinter.h"

namespace rcc::riscv {

class RISCVMCTargetLowering final : public TargetMCInstLowering {
public:
  explicit RISCVMCTargetLowering(const TargetDescription &Target) noexcept
      : Target(&Target), Printer(Target) {}

  [[nodiscard]] const mc::MCInstPrinter &instructionPrinter() const noexcept override {
    return Printer;
  }

  [[nodiscard]] Expected<mc::MCInst> lowerInstruction(const MachineInstr &Instruction,
                                                      MachineMCContext &Context) const override;
  [[nodiscard]] Expected<void> emitTerminalInstruction(const MachineInstr &Instruction,
                                                       MachineMCContext &Context,
                                                       mc::MCStreamer &Streamer) const override;

  [[nodiscard]] Expected<mc::MCSectionDesc>
  textSection(const MachineFunction &Function) const override;
  [[nodiscard]] Expected<mc::MCSectionDesc> globalSection(const GlobalObjectDesc &Object,
                                                          MachineMCContext &Context) const override;
  [[nodiscard]] Expected<mc::MCSectionDesc>
  constantPoolSection(const MachineFunction &Function,
                      const MachineConstantPoolEntry &Entry) const override;
  [[nodiscard]] Expected<mc::MCSectionDesc>
  jumpTableSection(const MachineFunction &Function,
                   const MachineJumpTableEntry &Entry) const override;

  [[nodiscard]] Expected<void> emitFileHeader(MachineMCContext &Context,
                                              mc::MCStreamer &Streamer) const override;
  [[nodiscard]] Expected<void> emitFileTrailer(MachineMCContext &Context,
                                               mc::MCStreamer &Streamer) const override;
  [[nodiscard]] Expected<void> emitGenericConstant(const GenericConstantData &Constant,
                                                   std::uint64_t Size, MachineMCContext &Context,
                                                   mc::MCStreamer &Streamer) const override;
  [[nodiscard]] Expected<void> emitGlobalInitializer(const GlobalInitializer &Initializer,
                                                     MachineMCContext &Context,
                                                     mc::MCStreamer &Streamer) const override;
  [[nodiscard]] Expected<void> emitTargetConstant(const TargetConstant &Constant,
                                                  MachineMCContext &Context,
                                                  mc::MCStreamer &Streamer) const override;
  [[nodiscard]] Expected<void> emitTargetJumpTable(const MachineFunction &Function,
                                                   const MachineJumpTableEntry &Entry,
                                                   MachineMCContext &Context,
                                                   mc::MCStreamer &Streamer) const override;

private:
  const TargetDescription *Target;
  RISCVInstPrinter Printer;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVMCTARGETLOWERING_H
