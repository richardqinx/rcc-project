// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_REACHINGDEFPRINTER_H
#define RCC_MIR_REACHINGDEFPRINTER_H

#include "rcc/CodeGen/MachinePassManager.h"

#include <string_view>

namespace rcc::mir {

class ReachingDefPrinterPass final : public MachineFunctionPass {
public:
  [[nodiscard]] static constexpr std::string_view name() noexcept {
    return "print<reaching-def>";
  }

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;
};

} // namespace rcc::mir

#endif // RCC_MIR_REACHINGDEFPRINTER_H
