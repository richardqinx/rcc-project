// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINELIVENESSREPAIR_H
#define RCC_CODEGEN_MACHINELIVENESSREPAIR_H

#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/Support/Error.h"

#include <span>

namespace rcc {

class MachineBasicBlock;
class TargetDescription;

[[nodiscard]] Expected<bool>
repairPostRAMachineBlockLiveness(std::span<MachineBasicBlock *const> Blocks,
                                 const TargetDescription &Target,
                                 const LiveVariablesAnalysis::Result &Liveness);

} // namespace rcc

#endif // RCC_CODEGEN_MACHINELIVENESSREPAIR_H
