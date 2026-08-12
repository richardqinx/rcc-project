// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVPIPELINE_H
#define RCC_TARGET_RISCV_RISCVPIPELINE_H

#include "rcc/CodeGen/MachinePass.h"
#include "rcc/Support/Error.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class MachineFunction;
class TargetDescription;

namespace riscv {

[[nodiscard]] Expected<void> addCanonicalPipeline(MachinePassManager &Pipeline,
                                                  const MachineFunction &Function);
[[nodiscard]] Expected<void> addCanonicalPipeline(MachinePassManager &Pipeline,
                                                  const MachineFunction &Function,
                                                  RISCVSubtargetFeatures Features);
[[nodiscard]] Expected<std::unique_ptr<MachineFunctionPass>>
createCanonicalPipelinePass(std::string_view Name);
[[nodiscard]] Expected<std::unique_ptr<MachineFunctionPass>>
createCanonicalPipelinePass(std::string_view Name, RISCVSubtargetFeatures Features);
[[nodiscard]] Expected<std::vector<std::string>>
canonicalPipelinePassNames(const MachineFunction &Function);
} // namespace riscv
} // namespace rcc

#endif // RCC_TARGET_RISCV_RISCVPIPELINE_H
