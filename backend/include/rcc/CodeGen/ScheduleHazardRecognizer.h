// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_SCHEDULEHAZARDRECOGNIZER_H
#define RCC_CODEGEN_SCHEDULEHAZARDRECOGNIZER_H

#include "rcc/CodeGen/TargetSchedulingModel.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rcc {

struct ScheduleUnit;

class ScheduleHazardRecognizer final {
public:
  [[nodiscard]] static Expected<ScheduleHazardRecognizer>
  create(const TargetSchedulingModel &Model,
         ScheduleDirection Direction = ScheduleDirection::TopDown);

  ScheduleHazardRecognizer(const ScheduleHazardRecognizer &Other);
  ScheduleHazardRecognizer &operator=(const ScheduleHazardRecognizer &Other);
  ScheduleHazardRecognizer(ScheduleHazardRecognizer &&) noexcept = default;
  ScheduleHazardRecognizer &operator=(ScheduleHazardRecognizer &&) noexcept = default;
  ~ScheduleHazardRecognizer() = default;

  [[nodiscard]] std::uint64_t currentCycle() const noexcept {
    return CurrentCycle;
  }

  [[nodiscard]] Expected<bool> canIssue(const ScheduleUnit &Unit, std::uint64_t Cycle) const;
  [[nodiscard]] Expected<std::uint64_t> earliestIssueCycle(const ScheduleUnit &Unit,
                                                           std::uint64_t Earliest) const;
  [[nodiscard]] Expected<void> issue(const ScheduleUnit &Unit, std::uint64_t Cycle);

private:
  struct ResourceState final {
    ProcessorResourceID ID;
    std::vector<std::uint64_t> AvailableAt;
  };

  ScheduleHazardRecognizer(const TargetSchedulingModel &Model, ScheduleDirection Direction) noexcept
      : Model(&Model), Direction(Direction) {}

  [[nodiscard]] Expected<void> reset();
  [[nodiscard]] Expected<void> moveToCycle(std::uint64_t Cycle);
  [[nodiscard]] Expected<void> issueImplementation(const ScheduleUnit &Unit, std::uint64_t Cycle);
  [[nodiscard]] Expected<void> reserveResource(ProcessorResourceID Resource, std::uint32_t Cycles,
                                               std::uint64_t Cycle);

  const TargetSchedulingModel *Model;
  ScheduleDirection Direction = ScheduleDirection::TopDown;
  std::unique_ptr<TargetScheduleHazardRecognizer> TargetHazards;
  std::vector<ResourceState> Resources;
  std::unordered_map<ProcessorResourceID, std::size_t> ResourceIndexes;
  std::uint64_t CurrentCycle = 0;
  std::uint64_t DispatchAvailableAt = 0;
  std::uint32_t IssuedMicroOps = 0;
  std::uint32_t DispatchedUnits = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_SCHEDULEHAZARDRECOGNIZER_H
