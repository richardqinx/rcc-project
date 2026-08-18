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

namespace riscv {

class RISCVRegisterBankInfo;
class RISCVSubtarget;

[[nodiscard]] InstructionSelector *
createRISCVInstructionSelector(const RISCVSubtarget &Subtarget,
                               const RISCVRegisterBankInfo &RegisterBanks);
[[nodiscard]] const TargetRegisterAllocationInfo &registerAllocationInfo() noexcept;
[[nodiscard]] const TargetFrameLowering &frameLowering() noexcept;
[[nodiscard]] const TargetSchedulingModel &schedulingModel() noexcept;
[[nodiscard]] const TargetMachineCombinerInfo &machineCombinerInfo() noexcept;
[[nodiscard]] const GIMatchTable &giselCombinerTable() noexcept;
[[nodiscard]] const GIMatchTableExecutor &giselCombinerExecutor() noexcept;
[[nodiscard]] const TargetMemoryFoldingInfo &memoryFoldingInfo() noexcept;
[[nodiscard]] const TargetPseudoExpansionInfo &pseudoExpansionInfo() noexcept;
[[nodiscard]] MachineOptimizationPipelineTargetInfo optimizationPipelineInfo() noexcept;

} // namespace riscv
} // namespace rcc

#endif // RCC_LIB_TARGET_RISCV_RISCV_H
