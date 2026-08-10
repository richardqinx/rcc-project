// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGISTERPRESSURE_H
#define RCC_CODEGEN_REGISTERPRESSURE_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/ScheduleDAG.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineFunction;
class TargetDescription;
class TargetSchedulingModel;
struct MachineBlockLiveness;

namespace detail {
struct RegisterPressureBuilder;
} // namespace detail

struct RegisterPressureValue final {
  RegisterPressureSetID Set;
  std::uint32_t Current = 0;
  std::uint32_t Maximum = 0;
  std::uint32_t Limit = 0;

  friend constexpr bool operator==(RegisterPressureValue, RegisterPressureValue) noexcept = default;
};

struct RegisterPressureCost final {
  std::uint64_t CurrentPressure = 0;
  std::uint64_t CurrentExcess = 0;
  std::uint64_t PeakExcess = 0;
  std::uint64_t PeakIncrease = 0;

  friend constexpr bool operator==(RegisterPressureCost, RegisterPressureCost) noexcept = default;
};

enum class RegisterPressureDirection : std::uint8_t {
  TopDown,
  BottomUp,
};

class RegisterPressureTracker final {
public:
  [[nodiscard]] static Expected<RegisterPressureTracker>
  create(const ScheduleDAG &DAG, const MachineFunction &Function, const TargetDescription &Target,
         const TargetSchedulingModel &Model, ScheduleDAGMode Mode,
         const MachineBlockLiveness &Liveness,
         RegisterPressureDirection Direction = RegisterPressureDirection::TopDown);

  [[nodiscard]] std::span<const RegisterPressureValue> pressure() const noexcept {
    return Pressure;
  }

  [[nodiscard]] bool wasScheduled(std::size_t UnitID) const noexcept {
    return UnitID < Scheduled.size() && Scheduled[UnitID];
  }

  [[nodiscard]] Expected<RegisterPressureCost> evaluate(std::size_t UnitID) const;
  [[nodiscard]] Expected<void> schedule(std::size_t UnitID);
  [[nodiscard]] RegisterPressureCost cost() const noexcept;

private:
  using EntityID = std::size_t;

  struct Entity final {
    std::vector<RegisterPressureSetID> Sets;
    std::size_t RemainingUses = 0;
    bool HasDefinition = false;
    bool DefinitionPending = false;
    bool LiveOut = false;
    bool Live = false;
  };

  struct Event final {
    std::vector<EntityID> Uses;
    std::vector<EntityID> Definitions;
  };

  struct UnitEffects final {
    std::vector<Event> Events;
  };

  [[nodiscard]] Expected<void> applyEvent(const Event &Value);
  [[nodiscard]] Expected<void> makeLive(EntityID Entity);
  [[nodiscard]] Expected<void> makeDead(EntityID Entity);

  std::vector<RegisterPressureValue> Pressure;
  std::unordered_map<RegisterPressureSetID, std::size_t> PressureIndexes;
  std::vector<Entity> Entities;
  std::vector<UnitEffects> Effects;
  std::vector<bool> Scheduled;
  RegisterPressureDirection Direction = RegisterPressureDirection::TopDown;

  friend struct detail::RegisterPressureBuilder;
};

} // namespace rcc

#endif // RCC_CODEGEN_REGISTERPRESSURE_H
