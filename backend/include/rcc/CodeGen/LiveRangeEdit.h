// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_LIVERANGEEDIT_H
#define RCC_CODEGEN_LIVERANGEEDIT_H

#include "rcc/CodeGen/VirtRegMap.h"

#include <optional>

namespace rcc {

class TargetDescription;

class LiveRangeEdit final {
public:
  LiveRangeEdit(MachineFunction &Function, const TargetDescription &Target, VirtRegMap &Mapping)
      : Function(&Function), Target(&Target), Mapping(&Mapping) {}

  [[nodiscard]] Expected<Register> createFrom(Register Parent, bool Spillable,
                                              std::optional<Register> Origin = std::nullopt);
  [[nodiscard]] Expected<FrameIndex> getOrCreateSpillSlot(Register VirtualReg);
  [[nodiscard]] Expected<void> eraseVirtualRegister(Register VirtualReg);

private:
  MachineFunction *Function;
  const TargetDescription *Target;
  VirtRegMap *Mapping;
};

} // namespace rcc

#endif // RCC_CODEGEN_LIVERANGEEDIT_H
