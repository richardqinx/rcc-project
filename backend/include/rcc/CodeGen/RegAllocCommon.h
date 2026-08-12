// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGALLOCCOMMON_H
#define RCC_CODEGEN_REGALLOCCOMMON_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <memory>

namespace rcc {

class TargetRegisterAllocationInfo;

struct RegisterAllocationOptions final {
  bool EnableRematerialization = true;
  bool EnableCopyCoalescing = true;
};

[[nodiscard]] std::unique_ptr<MachineFunctionPass>
createBasicAllocatorPass(const TargetRegisterAllocationInfo &Info,
                         RegisterAllocationOptions Options = {});

} // namespace rcc

#endif // RCC_CODEGEN_REGALLOCCOMMON_H
