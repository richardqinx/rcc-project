// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINETAILMERGE_H
#define RCC_CODEGEN_MACHINETAILMERGE_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

class TargetInstrInfo;

struct MachineTailMergeOptions final {
  std::size_t MaximumMergedBlocks = 4'096;
};

class MachineTailMergePass final : public MachineFunctionPass {
public:
  explicit MachineTailMergePass(const TargetInstrInfo &Info,
                                MachineTailMergeOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target) const;

  const TargetInstrInfo *Info;
  MachineTailMergeOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINETAILMERGE_H
