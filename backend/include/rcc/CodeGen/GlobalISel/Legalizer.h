// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_LEGALIZER_H
#define RCC_CODEGEN_GLOBALISEL_LEGALIZER_H

#include "rcc/CodeGen/CommandFlags.h"
#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"
#include "rcc/CodeGen/MachinePassManager.h"

#include <memory>

namespace rcc {

class Legalizer final : public MachineFunctionPass {
public:
  Legalizer(const LegalizerInfo *Info, OptimizationLevel Optimization)
      : Info(Info), Optimization(Optimization) {}
  Legalizer(const LegalizerInfo &Info, OptimizationLevel Optimization)
      : Info(&Info), Optimization(Optimization) {}
  Legalizer(std::unique_ptr<LegalizerInfo> Info, OptimizationLevel Optimization)
      : OwnedInfo(std::move(Info)), Info(OwnedInfo.get()), Optimization(Optimization) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineActionReplay &Actions);

  std::unique_ptr<LegalizerInfo> OwnedInfo;
  const LegalizerInfo *Info = nullptr;
  OptimizationLevel Optimization;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZER_H
