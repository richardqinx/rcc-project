// SPDX-License-Identifier: GPL-3.0-or-later

// The private header has no stable include-root-relative guard name.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_LIB_CODEGEN_REGALLOCBASE_H
#define RCC_LIB_CODEGEN_REGALLOCBASE_H

#include "RegAllocInternal.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/VirtRegMap.h"

#include <vector>

namespace rcc {

struct AllocationRoundResult final {
  std::vector<Register> Spilled;
};

class RegAllocBase {
public:
  virtual ~RegAllocBase() = default;

  [[nodiscard]] Expected<regalloc_detail::AllocationRunResult> run();

protected:
  RegAllocBase(MachineFunction &Function, const TargetDescription &Target,
               const TargetRegisterAllocationInfo &TargetInfo, MachineAnalysisManager &Analyses,
               RegisterAllocationOptions Options, MachineActionReplay *Actions = nullptr) noexcept
      : Function(Function), Target(Target), TargetInfo(TargetInfo), Analyses(Analyses),
        Options(Options), Actions(Actions) {}

  [[nodiscard]] virtual Expected<AllocationRoundResult>
  allocateRound(const LiveIntervalsAnalysis::Result &Intervals, VirtRegMap &Mapping) = 0;

  MachineFunction &Function;
  const TargetDescription &Target;
  const TargetRegisterAllocationInfo &TargetInfo;
  MachineAnalysisManager &Analyses;
  RegisterAllocationOptions Options;
  MachineActionReplay *Actions = nullptr;
};

[[nodiscard]] std::vector<const LiveInterval *>
orderedAllocationIntervals(const LiveIntervalsAnalysis::Result &Intervals,
                           const MachineFunction &Function);

} // namespace rcc

#endif // RCC_LIB_CODEGEN_REGALLOCBASE_H
