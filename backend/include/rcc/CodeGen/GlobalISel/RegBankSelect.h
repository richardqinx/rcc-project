// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_REGBANKSELECT_H
#define RCC_CODEGEN_GLOBALISEL_REGBANKSELECT_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/RegisterBankInfo.h"

namespace rcc {

class RegBankSelect final : public MachineFunctionPass {
public:
  explicit RegBankSelect(const RegisterBankInfo *Info) : Info(Info) {}
  explicit RegBankSelect(const RegisterBankInfo &Info) : Info(&Info) {}

  [[nodiscard]] const MachinePassContract &contract() const noexcept override;
  [[nodiscard]] Expected<MachinePassResult> run(MachineFunction &Function,
                                                const TargetDescription &Target,
                                                MachineFunctionAnalysisManager &Analyses) override;

private:
  const RegisterBankInfo *Info;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_REGBANKSELECT_H
