// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVPIPELINE_H
#define RCC_TARGET_RISCV_RISCVPIPELINE_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/Support/Error.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"
#include "rcc/Target/TargetMachine.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

class MachineFunction;
class TargetDescription;
class TargetMachineOutlinerInfo;

namespace riscv {

[[nodiscard]] Expected<void> addCanonicalPipeline(MachineFunctionPassManager &Pipeline,
                                                  const MachineFunction &Function,
                                                  const RISCVSubtarget &Subtarget,
                                                  TargetPipelineOptions Options = {});
[[nodiscard]] Expected<std::unique_ptr<MachineFunctionPass>>
createCanonicalPipelinePass(std::string_view Name, const RISCVSubtarget &Subtarget,
                            TargetPipelineOptions Options = {});
[[nodiscard]] Expected<std::vector<std::string>>
canonicalPipelinePassNames(const MachineFunction &Function, const RISCVSubtarget &Subtarget);
[[nodiscard]] Expected<std::unique_ptr<TargetMachineOutlinerInfo>>
createMachineOutlinerInfo(bool FunctionSections, bool EnableRegisterSave);
} // namespace riscv
} // namespace rcc

#endif // RCC_TARGET_RISCV_RISCVPIPELINE_H
