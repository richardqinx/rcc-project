// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGALLOCFAST_H
#define RCC_CODEGEN_REGALLOCFAST_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"

namespace rcc {

class FastAllocatorPass final : public MachineFunctionPass {
public:
  explicit FastAllocatorPass(const TargetRegisterAllocationInfo &Info) noexcept : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetRegisterAllocationInfo *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_REGALLOCFAST_H
