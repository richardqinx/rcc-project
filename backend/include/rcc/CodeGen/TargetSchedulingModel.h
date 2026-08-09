// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETSCHEDULINGMODEL_H
#define RCC_CODEGEN_TARGETSCHEDULINGMODEL_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class ScheduleDAG;
class TargetDescription;
struct ScheduleUnit;

enum class ScheduleDAGMode : std::uint8_t;

enum class ScheduleDirection : std::uint8_t {
  TopDown,
  BottomUp,
};

enum class ScheduleHazardType : std::uint8_t {
  NoHazard,
  Hazard,
  NoopHazard,
};

struct ProcessorResourceDesc final {
  ProcessorResourceID ID;
  std::string Name;
  std::uint32_t Units = 1;
  std::optional<ProcessorResourceID> Parent;
};

struct SchedulingResourceUse final {
  ProcessorResourceID Resource;
  std::uint32_t Cycles = 1;

  friend constexpr bool operator==(SchedulingResourceUse, SchedulingResourceUse) noexcept = default;
};

struct SchedulingWriteLatency final {
  std::uint32_t OperandIndex = 0;
  std::uint32_t Cycles = 0;

  friend constexpr bool operator==(SchedulingWriteLatency,
                                   SchedulingWriteLatency) noexcept = default;
};

struct SchedulingReadAdvance final {
  std::uint32_t OperandIndex = 0;
  std::optional<SchedulingClassID> ProducerClass;
  std::uint32_t Cycles = 0;

  friend constexpr bool operator==(const SchedulingReadAdvance &,
                                   const SchedulingReadAdvance &) noexcept = default;
};

struct SchedulingClassModelDesc final {
  SchedulingClassID ID;
  std::string Name;
  std::uint32_t MicroOps = 1;
  std::uint32_t Latency = 1;
  std::vector<SchedulingResourceUse> Resources;
  std::vector<SchedulingWriteLatency> WriteLatencies;
  std::vector<SchedulingReadAdvance> ReadAdvances;
};

struct RegisterPressureSetDesc final {
  RegisterPressureSetID ID;
  std::string Name;
  std::vector<RegisterClassID> RegisterClasses;
  std::uint32_t Limit = 1;
};

class TargetScheduleHazardRecognizer {
public:
  virtual ~TargetScheduleHazardRecognizer() = default;

  [[nodiscard]] virtual std::unique_ptr<TargetScheduleHazardRecognizer> clone() const = 0;
  [[nodiscard]] virtual Expected<ScheduleHazardType> getHazardType(const ScheduleUnit &Unit) const;
  [[nodiscard]] virtual Expected<void> emitInstruction(const ScheduleUnit &Unit);
  [[nodiscard]] virtual Expected<void> advanceCycle();
  [[nodiscard]] virtual Expected<void> recedeCycle();
  [[nodiscard]] virtual Expected<void> reset();
};

class TargetSchedulingHooks {
public:
  virtual ~TargetSchedulingHooks() = default;

  [[nodiscard]] virtual bool isSchedulingBoundary(const MachineInstr &Instruction,
                                                  const TargetDescription &Target) const noexcept;
  [[nodiscard]] virtual Expected<void> mutateScheduleDAG(ScheduleDAG &DAG,
                                                         const MachineFunction &Function,
                                                         const TargetDescription &Target,
                                                         ScheduleDAGMode Mode) const;
  [[nodiscard]] virtual std::int64_t
  candidatePreference(const ScheduleUnit &Unit, ScheduleDirection Direction,
                      const MachineBasicBlock &Block, std::uint64_t BlockFrequency) const noexcept;
  [[nodiscard]] virtual std::unique_ptr<TargetScheduleHazardRecognizer>
  createHazardRecognizer(ScheduleDirection Direction) const;
};

struct TargetSchedulingModelDesc final {
  std::uint32_t IssueWidth = 1;
  std::uint32_t DispatchLimit = 1;
  std::vector<ProcessorResourceDesc> ProcessorResources;
  std::vector<SchedulingClassModelDesc> SchedulingClasses;
  std::vector<RegisterPressureSetDesc> PressureSets;
  const TargetSchedulingHooks *Hooks = nullptr;
};

class TargetSchedulingModel final {
public:
  [[nodiscard]] static Expected<TargetSchedulingModel>
  create(TargetSchedulingModelDesc Description);

  [[nodiscard]] std::uint32_t issueWidth() const noexcept {
    return Description.IssueWidth;
  }

  [[nodiscard]] std::uint32_t dispatchLimit() const noexcept {
    return Description.DispatchLimit;
  }

  [[nodiscard]] std::span<const ProcessorResourceDesc> processorResources() const noexcept {
    return Description.ProcessorResources;
  }

  [[nodiscard]] std::span<const SchedulingClassModelDesc> schedulingClasses() const noexcept {
    return Description.SchedulingClasses;
  }

  [[nodiscard]] std::span<const RegisterPressureSetDesc> pressureSets() const noexcept {
    return Description.PressureSets;
  }

  [[nodiscard]] const ProcessorResourceDesc *
  processorResource(ProcessorResourceID ID) const noexcept;
  [[nodiscard]] const SchedulingClassModelDesc *
  schedulingClass(SchedulingClassID ID) const noexcept;
  [[nodiscard]] const RegisterPressureSetDesc *pressureSet(RegisterPressureSetID ID) const noexcept;
  [[nodiscard]] std::span<const RegisterPressureSetID>
  pressureSetsForClass(RegisterClassID Class) const noexcept;
  [[nodiscard]] Expected<std::uint32_t> operandLatency(SchedulingClassID ProducerClass,
                                                       std::size_t DefinitionOperand,
                                                       SchedulingClassID ConsumerClass,
                                                       std::size_t UseOperand) const;
  [[nodiscard]] const TargetSchedulingHooks &hooks() const noexcept;

private:
  explicit TargetSchedulingModel(TargetSchedulingModelDesc Description)
      : Description(std::move(Description)) {}

  TargetSchedulingModelDesc Description;
  std::unordered_map<ProcessorResourceID, std::size_t> ResourceToIndex;
  std::unordered_map<SchedulingClassID, std::size_t> ClassToIndex;
  std::unordered_map<RegisterPressureSetID, std::size_t> PressureSetToIndex;
  std::unordered_map<RegisterClassID, std::vector<RegisterPressureSetID>> ClassToPressureSets;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETSCHEDULINGMODEL_H
