// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVPIPELINE_H
#define RCC_TARGET_RISCV_RISCVPIPELINE_H

#include "rcc/CodeGen/CommandFlags.h"
#include "rcc/CodeGen/MachinePassManager.h"
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

struct RISCVPipelineOptions final {
  OptimizationLevel Optimization = OptimizationLevel::O2;
  bool UseTargetCopyInstructions = false;
  GlobalISelAbortMode GlobalISelAbort = GlobalISelAbortMode::Enable;
  bool DebugMachineScheduler = false;
};

[[nodiscard]] Expected<void> addCanonicalPipeline(MachineFunctionPassManager &Pipeline,
                                                  const MachineFunction &Function,
                                                  const RISCVSubtarget &Subtarget,
                                                  RISCVPipelineOptions Options = {});
[[nodiscard]] Expected<std::unique_ptr<MachineFunctionPass>>
createCanonicalPipelinePass(std::string_view Name, const RISCVSubtarget &Subtarget,
                            RISCVPipelineOptions Options = {});
[[nodiscard]] Expected<std::vector<std::string>>
canonicalPipelinePassNames(const MachineFunction &Function, const RISCVSubtarget &Subtarget);
} // namespace riscv
} // namespace rcc

#endif // RCC_TARGET_RISCV_RISCVPIPELINE_H
