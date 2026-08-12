// SPDX-License-Identifier: GPL-3.0-or-later

// The private header has no stable include-root-relative guard name.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_LIB_CODEGEN_REGALLOCBASIC_H
#define RCC_LIB_CODEGEN_REGALLOCBASIC_H

#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"

namespace rcc {

class BasicAllocatorPass final : public MachineFunctionPass {
public:
  explicit BasicAllocatorPass(const TargetRegisterAllocationInfo &Info,
                              RegisterAllocationOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const TargetRegisterAllocationInfo *Info;
  RegisterAllocationOptions Options;
};

} // namespace rcc

#endif // RCC_LIB_CODEGEN_REGALLOCBASIC_H
