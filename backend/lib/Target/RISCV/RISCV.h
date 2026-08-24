// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_RISCV_H
#define RCC_LIB_TARGET_RISCV_RISCV_H

#include "rcc/CodeGen/GlobalISel/GIMatchTableExecutor.h"
#include "rcc/CodeGen/MachineCombinerPattern.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/TargetFrameLowering.h"
#include "rcc/CodeGen/TargetMemoryFolding.h"
#include "rcc/CodeGen/TargetPseudoExpansion.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"

namespace rcc {

class InstructionSelector;
class MachineFunctionPass;

namespace riscv {

class RISCVRegisterBankInfo;
class RISCVSubtarget;

[[nodiscard]] InstructionSelector *
createRISCVInstructionSelector(const RISCVSubtarget &Subtarget,
                               const RISCVRegisterBankInfo &RegisterBanks);
[[nodiscard]] const TargetRegisterAllocationInfo &registerAllocationInfo() noexcept;
[[nodiscard]] std::unique_ptr<TargetFrameLowering>
createFrameLowering(const RISCVSubtarget &Subtarget);
[[nodiscard]] std::unique_ptr<TargetSchedulingHooks>
createSchedulingHooks(const RISCVSubtargetFeatures &Features);
[[nodiscard]] Expected<TargetSchedulingModel>
createSchedulingModel(const TargetSchedulingHooks &Hooks);
[[nodiscard]] const TargetMachineCombinerInfo &machineCombinerInfo() noexcept;
[[nodiscard]] const GIMatchTable &giselCombinerTable() noexcept;
[[nodiscard]] const GIMatchTableExecutor &giselCombinerExecutor() noexcept;
[[nodiscard]] const TargetMemoryFoldingInfo &memoryFoldingInfo() noexcept;
[[nodiscard]] const TargetPseudoExpansionInfo &pseudoExpansionInfo() noexcept;
[[nodiscard]] std::unique_ptr<MachineFunctionPass>
createMoveMergePass(const RISCVSubtarget &Subtarget);
[[nodiscard]] MachineOptimizationPipelineTargetInfo
optimizationPipelineInfo(const RISCVSubtarget &Subtarget, bool EmitSchedulerDebugOutput = false,
                         bool EmitFrameDebugOutput = false) noexcept;

} // namespace riscv
} // namespace rcc

#endif // RCC_LIB_TARGET_RISCV_RISCV_H
