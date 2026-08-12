// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINETAILDUPLICATION_H
#define RCC_CODEGEN_MACHINETAILDUPLICATION_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <cstddef>

namespace rcc {

class TargetInstrInfo;

struct MachineTailDuplicationOptions final {
  std::size_t MaximumTailInstructions = 8;
  std::size_t MaximumDuplicatedInstructions = 4'096;
};

class MachineTailDuplicationPass final : public MachineFunctionPass {
public:
  explicit MachineTailDuplicationPass(const TargetInstrInfo &Info,
                                      MachineTailDuplicationOptions Options = {}) noexcept
      : Info(&Info), Options(Options) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult>
  runImplementation(MachineFunction &Function, const TargetDescription &Target) const;

  const TargetInstrInfo *Info;
  MachineTailDuplicationOptions Options;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINETAILDUPLICATION_H
