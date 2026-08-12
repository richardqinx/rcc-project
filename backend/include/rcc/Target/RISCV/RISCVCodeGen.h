// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVCODEGEN_H
#define RCC_TARGET_RISCV_RISCVCODEGEN_H

#include "rcc/CodeGen/MachineCombinerPattern.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/TargetCodeGen.h"
#include "rcc/CodeGen/TargetFrameLowering.h"
#include "rcc/CodeGen/TargetInstrInfo.h"
#include "rcc/CodeGen/TargetMemoryFolding.h"
#include "rcc/CodeGen/TargetPseudoExpansion.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"

#include <memory>

namespace rcc::riscv {

[[nodiscard]] const TargetLegalizerInfo &legalizerInfo() noexcept;
[[nodiscard]] std::unique_ptr<TargetLegalizerInfo>
createLegalizerInfo(RISCVSubtargetFeatures Features);
[[nodiscard]] const TargetRegisterBankInfo &registerBankInfo() noexcept;
[[nodiscard]] const TargetInstructionSelector &instructionSelector() noexcept;
[[nodiscard]] const TargetSSAInfo &ssaInfo() noexcept;
[[nodiscard]] const TargetRegisterAllocationInfo &registerAllocationInfo() noexcept;
[[nodiscard]] const TargetFrameLowering &frameLowering() noexcept;
[[nodiscard]] const TargetSchedulingModel &schedulingModel() noexcept;
[[nodiscard]] const TargetMachineCombinerInfo &machineCombinerInfo() noexcept;
[[nodiscard]] const TargetMemoryFoldingInfo &memoryFoldingInfo() noexcept;
[[nodiscard]] const TargetInstrInfo &branchInfo() noexcept;
[[nodiscard]] const TargetPseudoExpansionInfo &pseudoExpansionInfo() noexcept;
[[nodiscard]] MachineOptimizationPipelineTargetInfo optimizationPipelineInfo() noexcept;

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVCODEGEN_H
