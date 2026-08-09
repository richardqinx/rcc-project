// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_CODEGEN_MACHINESCHEDULERINTERNAL_H
#define RCC_LIB_CODEGEN_MACHINESCHEDULERINTERNAL_H

#include "rcc/CodeGen/MachineAnalyses.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineScheduler.h"
#include "rcc/CodeGen/ScheduleDAG.h"
#include "rcc/Support/Error.h"

#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class TargetDescription;
class TargetSchedulingModel;
struct MachineSchedulingRunResult final {
  bool Changed = false;
  std::vector<MachineBasicBlock *> ChangedBlocks;
};

[[nodiscard]] Expected<MachineSchedulingRunResult> runMachineScheduler(
    MachineFunction &Function, const TargetDescription &Target, const TargetSchedulingModel &Model,
    ScheduleDAGMode Mode, const LiveVariablesAnalysis::Result &Liveness,
    const MachineBlockFrequencyAnalysis::Result &Frequencies, MachineSchedulerOptions Options);

} // namespace rcc

#endif // RCC_LIB_CODEGEN_MACHINESCHEDULERINTERNAL_H
