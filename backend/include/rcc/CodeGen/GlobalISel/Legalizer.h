// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_LEGALIZER_H
#define RCC_CODEGEN_GLOBALISEL_LEGALIZER_H

#include "rcc/CodeGen/CommandFlags.h"
#include "rcc/CodeGen/GlobalISel/LegalizerInfo.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/TargetCallLowering.h"

#include <memory>

namespace rcc {

class Legalizer final : public MachineFunctionPass {
public:
  Legalizer(const LegalizerInfo *Info, OptimizationLevel Optimization,
            LibcallLoweringInfo Libcalls = {},
            GlobalISelAbortMode AbortMode = GlobalISelAbortMode::Enable)
      : Info(Info), Optimization(Optimization), Libcalls(Libcalls), AbortMode(AbortMode) {}
  Legalizer(const LegalizerInfo &Info, OptimizationLevel Optimization,
            LibcallLoweringInfo Libcalls = {},
            GlobalISelAbortMode AbortMode = GlobalISelAbortMode::Enable)
      : Info(&Info), Optimization(Optimization), Libcalls(Libcalls), AbortMode(AbortMode) {}
  Legalizer(std::unique_ptr<LegalizerInfo> Info, OptimizationLevel Optimization,
            LibcallLoweringInfo Libcalls = {},
            GlobalISelAbortMode AbortMode = GlobalISelAbortMode::Enable)
      : OwnedInfo(std::move(Info)), Info(OwnedInfo.get()), Optimization(Optimization),
        Libcalls(Libcalls), AbortMode(AbortMode) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  [[nodiscard]] Expected<MachinePassResult> runImplementation(MachineFunction &Function,
                                                              const TargetDescription &Target,
                                                              MachineFunctionAnalysisManager
                                                                  &Analyses);

  std::unique_ptr<LegalizerInfo> OwnedInfo;
  const LegalizerInfo *Info = nullptr;
  OptimizationLevel Optimization;
  LibcallLoweringInfo Libcalls;
  GlobalISelAbortMode AbortMode;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_LEGALIZER_H
