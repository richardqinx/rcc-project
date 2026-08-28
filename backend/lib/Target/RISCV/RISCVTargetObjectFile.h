// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVTARGETOBJECTFILE_H
#define RCC_TARGET_RISCV_RISCVTARGETOBJECTFILE_H

#include "rcc/CodeGen/TargetLoweringObjectFile.h"

namespace rcc::riscv {

class RISCVELFTargetObjectFile final : public TargetLoweringObjectFile {
public:
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
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVTARGETOBJECTFILE_H
