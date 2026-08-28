// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/TargetMachine.h"

#include "rcc/MC/TargetRegistry.h"

#include <utility>

namespace rcc {

TargetMachine::TargetMachine(const Target &RegisteredTarget, TargetMachineOptions Options,
                             TargetDescription Description)
    : RegisteredTarget(RegisteredTarget), Options(std::move(Options)),
      Description(std::move(Description)) {}

TargetMachine::~TargetMachine() = default;

} // namespace rcc
