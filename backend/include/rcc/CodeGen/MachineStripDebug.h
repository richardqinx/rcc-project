// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINESTRIPDEBUG_H
#define RCC_CODEGEN_MACHINESTRIPDEBUG_H

#include "rcc/Support/Error.h"

namespace rcc {

class MachineFunction;

[[nodiscard]] Expected<bool> stripDebugMachineFunction(MachineFunction &Function);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINESTRIPDEBUG_H
