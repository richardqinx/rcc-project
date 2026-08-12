// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVCODEGEN_H
#define RCC_TARGET_RISCV_RISCVCODEGEN_H

#include "rcc/CodeGen/GlobalISel/InstructionSelector.h"
#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"
#include "rcc/CodeGen/MachineCombinerPattern.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/RegisterBankInfo.h"
#include "rcc/CodeGen/TargetFrameLowering.h"
#include "rcc/CodeGen/TargetMemoryFolding.h"
#include "rcc/CodeGen/TargetPseudoExpansion.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"

#include <memory>

namespace rcc::riscv {

[[nodiscard]] const LegalizerInfo &legalizerInfo() noexcept;
[[nodiscard]] std::unique_ptr<LegalizerInfo> createLegalizerInfo(RISCVSubtargetFeatures Features);
[[nodiscard]] const RegisterBankInfo &registerBankInfo() noexcept;
[[nodiscard]] const InstructionSelector &instructionSelector() noexcept;
[[nodiscard]] const TargetRegisterAllocationInfo &registerAllocationInfo() noexcept;
[[nodiscard]] const TargetFrameLowering &frameLowering() noexcept;
[[nodiscard]] const TargetSchedulingModel &schedulingModel() noexcept;
[[nodiscard]] const TargetMachineCombinerInfo &machineCombinerInfo() noexcept;
[[nodiscard]] const TargetMemoryFoldingInfo &memoryFoldingInfo() noexcept;
[[nodiscard]] const TargetPseudoExpansionInfo &pseudoExpansionInfo() noexcept;
[[nodiscard]] MachineOptimizationPipelineTargetInfo optimizationPipelineInfo() noexcept;

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVCODEGEN_H
