// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVASSEMBLYPRINTER_H
#define RCC_TARGET_RISCV_RISCVASSEMBLYPRINTER_H

#include "rcc/CodeGen/AsmPrinter.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Support/Error.h"

#include <string>

namespace rcc {
class CompilationUnit;
class MachineFunction;
} // namespace rcc

namespace rcc::riscv {

struct AssemblyEmissionOptions final {
  bool EmitFunctionDirectives = true;
  bool EmitCFI = true;
  bool EmitBlockLabels = true;
  bool VerifyInput = true;
};

[[nodiscard]] Expected<std::string> emitAssembly(const CompilationUnit &Unit,
                                                 const TargetDescription &Target,
                                                 AssemblyEmissionOptions Options = {});
[[nodiscard]] Expected<std::string> emitAssembly(const MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 AssemblyEmissionOptions Options = {});

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVASSEMBLYPRINTER_H
