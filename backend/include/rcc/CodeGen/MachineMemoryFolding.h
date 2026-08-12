// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEMEMORYFOLDING_H
#define RCC_CODEGEN_MACHINEMEMORYFOLDING_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

class TargetMemoryFoldingInfo;

struct MachineMemoryFoldingOptions final {
  std::size_t MaximumFolds = 4'096;
};

class MachineMemoryFoldingPass final : public MachineFunctionPass {
public:
  explicit MachineMemoryFoldingPass(const TargetMemoryFoldingInfo &Info,
                                    MachineMemoryFoldingOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target) const;

  const TargetMemoryFoldingInfo *Info;
  MachineMemoryFoldingOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEMEMORYFOLDING_H
