// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVTARGET_H
#define RCC_TARGET_RISCV_RISCVTARGET_H

#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/Support/Error.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <cstdint>
#include <memory>

namespace rcc::riscv {

[[nodiscard]] Expected<PhysicalRegister> physicalRegister(PhysicalRegisterID ID);
[[nodiscard]] Expected<TargetRegisterInfo> createRegisterInfo();
[[nodiscard]] Expected<TargetMIRTextInfo> createMIRTextInfo();
[[nodiscard]] std::shared_ptr<const RegisterMask> createCallPreservedMask();

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVTARGET_H
