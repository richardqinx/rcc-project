// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_SCHEDULEDAG_H
#define RCC_CODEGEN_SCHEDULEDAG_H

#include "rcc/CodeGen/StableID.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class TargetDescription;

enum class ScheduleDAGMode : std::uint8_t {
  PreRegisterAllocation,
  PostRegisterAllocation,
};

enum class ScheduleDependencyKind : std::uint8_t {
  Data,
  Anti,
  Output,
  Order,
  Memory,
  Barrier,
  Artificial,
  Cluster,
  RegisterUnit,
  RegisterMask,
};

struct ScheduleDependency final {
  std::size_t Source = 0;
  std::size_t Destination = 0;
  ScheduleDependencyKind Kind = ScheduleDependencyKind::Artificial;
  std::uint32_t Latency = 0;

  friend constexpr bool operator==(const ScheduleDependency &,
                                   const ScheduleDependency &) noexcept = default;
};

struct ScheduleUnit final {
  std::size_t ID = 0;
  std::vector<MachineInstr *> Instructions;
  std::vector<ScheduleDependency> Predecessors;
  std::vector<ScheduleDependency> Successors;
  std::vector<SchedulingResourceUse> Resources;
  std::uint32_t MicroOps = 0;
  std::uint32_t Latency = 0;
  std::uint64_t Depth = 0;
  std::uint64_t Height = 0;
  MachineInstrStableID StableID;
  std::optional<std::size_t> ClusterPredecessor;
  std::optional<std::size_t> ClusterSuccessor;
};

class ScheduleDAG final {
public:
  [[nodiscard]] static Expected<ScheduleDAG>
  build(MachineBasicBlock &Block, std::span<MachineInstr *const> Instructions,
        const MachineFunction &Function, const TargetDescription &Target,
        const TargetSchedulingModel &Model, ScheduleDAGMode Mode, bool EmitDebugOutput = false);

  [[nodiscard]] MachineBasicBlock &block() const noexcept {
    return *Block;
  }

  [[nodiscard]] std::span<const ScheduleUnit> units() const noexcept {
    return Units;
  }

  [[nodiscard]] std::span<ScheduleUnit> units() noexcept {
    return Units;
  }

  [[nodiscard]] const ScheduleUnit *unit(std::size_t ID) const noexcept {
    return ID < Units.size() ? &Units[ID] : nullptr;
  }

  [[nodiscard]] Expected<void> addArtificialDependency(std::size_t Source, std::size_t Destination,
                                                       std::uint32_t Latency);
  [[nodiscard]] Expected<bool> fuseInstructionPair(std::size_t First, std::size_t Second);

private:
  explicit ScheduleDAG(MachineBasicBlock &Block) : Block(&Block) {}

  [[nodiscard]] Expected<void> buildUnits(std::span<MachineInstr *const> Instructions,
                                          const TargetDescription &Target,
                                          const TargetSchedulingModel &Model);
  [[nodiscard]] Expected<void> buildRegisterDependencies(const MachineFunction &Function,
                                                         const TargetDescription &Target,
                                                         const TargetSchedulingModel &Model,
                                                         ScheduleDAGMode Mode);
  [[nodiscard]] Expected<void> buildMemoryDependencies(const TargetDescription &Target);
  [[nodiscard]] Expected<void> buildOrderDependencies(const TargetDescription &Target);
  [[nodiscard]] bool hasPath(std::size_t Source, std::size_t Destination) const;
  [[nodiscard]] bool hasDirectDependency(std::size_t Source, std::size_t Destination) const;
  void addDependency(std::size_t Source, std::size_t Destination, ScheduleDependencyKind Kind,
                     std::uint32_t Latency);
  [[nodiscard]] Expected<void> computeCriticalPaths();

  MachineBasicBlock *Block;
  std::vector<ScheduleUnit> Units;
};

} // namespace rcc

#endif // RCC_CODEGEN_SCHEDULEDAG_H
