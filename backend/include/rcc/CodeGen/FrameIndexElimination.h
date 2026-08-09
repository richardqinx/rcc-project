// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_FRAMEINDEXELIMINATION_H
#define RCC_CODEGEN_FRAMEINDEXELIMINATION_H

#include "rcc/Support/Error.h"

namespace rcc {

class MachineFunction;
class RegisterScavenger;
class TargetDescription;
class TargetFrameLowering;

[[nodiscard]] Expected<bool> eliminateFrameIndices(MachineFunction &Function,
                                                   const TargetDescription &Target,
                                                   const TargetFrameLowering &FrameLowering,
                                                   RegisterScavenger &Scavenger);

} // namespace rcc

#endif // RCC_CODEGEN_FRAMEINDEXELIMINATION_H
