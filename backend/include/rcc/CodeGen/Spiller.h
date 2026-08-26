// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_SPILLER_H
#define RCC_CODEGEN_SPILLER_H

#include "rcc/CodeGen/LiveRangeEdit.h"
#include "rcc/CodeGen/MachineFunctionAnalysisManager.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"

#include <optional>
#include <vector>

namespace rcc {

class Spiller final {
public:
  Spiller(MachineFunction &Function, const TargetDescription &Target,
          const TargetRegisterAllocationInfo &TargetInfo, VirtRegMap &Mapping,
          bool EnableRematerialization = true,
          MachineFunctionAnalysisManager *ActionAnalyses = nullptr)
      : Function(&Function), Target(&Target), TargetInfo(&TargetInfo), Mapping(&Mapping),
        Edit(Function, Target, Mapping), EnableRematerialization(EnableRematerialization),
        ActionAnalyses(ActionAnalyses) {}

  [[nodiscard]] Expected<std::vector<Register>> spill(Register VirtualReg);

private:
  [[nodiscard]] Expected<std::vector<Register>> spillAtInstructions(Register VirtualReg);
  [[nodiscard]] Expected<void> spillLiveIn(Register VirtualReg, RegisterClassID Class,
                                           FrameIndex StackSlot);
  [[nodiscard]] Expected<std::optional<std::vector<Register>>>
  tryRematerialize(Register VirtualReg);

  MachineFunction *Function;
  const TargetDescription *Target;
  const TargetRegisterAllocationInfo *TargetInfo;
  VirtRegMap *Mapping;
  LiveRangeEdit Edit;
  bool EnableRematerialization;
  MachineFunctionAnalysisManager *ActionAnalyses;
};

} // namespace rcc

#endif // RCC_CODEGEN_SPILLER_H
