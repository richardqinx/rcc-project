// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGALLOCPBQP_H
#define RCC_CODEGEN_REGALLOCPBQP_H

#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"

namespace rcc {

class PBQPAllocatorPass final : public MachineFunctionPass {
public:
  explicit PBQPAllocatorPass(const TargetRegisterAllocationInfo &Info,
                             RegisterAllocationOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineAnalysisManager &Analyses) override;

private:
  const TargetRegisterAllocationInfo *Info;
  RegisterAllocationOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_REGALLOCPBQP_H
