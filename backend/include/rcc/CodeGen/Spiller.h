// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_SPILLER_H
#define RCC_CODEGEN_SPILLER_H

#include "rcc/CodeGen/LiveRangeEdit.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"

#include <optional>
#include <vector>

namespace rcc {

class MachineActionReplay;

class Spiller final {
public:
  Spiller(MachineFunction &Function, const TargetDescription &Target,
          const TargetRegisterAllocationInfo &TargetInfo, VirtRegMap &Mapping,
          bool EnableRematerialization = true, MachineActionReplay *Actions = nullptr)
      : Function(&Function), Target(&Target), TargetInfo(&TargetInfo), Mapping(&Mapping),
        Edit(Function, Target, Mapping), EnableRematerialization(EnableRematerialization),
        Actions(Actions) {}

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
  MachineActionReplay *Actions;
};

} // namespace rcc

#endif // RCC_CODEGEN_SPILLER_H
