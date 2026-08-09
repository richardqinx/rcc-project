// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVOBJECTFILE_H
#define RCC_TARGET_RISCV_RISCVOBJECTFILE_H

#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <vector>

namespace rcc {
class CompilationUnit;
class MachineFunction;
} // namespace rcc

namespace rcc::riscv {

struct ObjectEmissionOptions final {
  bool EmitFunctionDirectives = true;
  bool EmitCFI = true;
  bool EmitBlockLabels = true;
  bool VerifyInput = true;
};

[[nodiscard]] Expected<std::vector<std::uint8_t>> emitObject(const CompilationUnit &Unit,
                                                             const TargetDescription &Target,
                                                             ObjectEmissionOptions Options = {});
[[nodiscard]] Expected<std::vector<std::uint8_t>> emitObject(const MachineFunction &Function,
                                                             const TargetDescription &Target,
                                                             ObjectEmissionOptions Options = {});

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVOBJECTFILE_H
