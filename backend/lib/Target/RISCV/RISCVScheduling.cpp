// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

enum class Resource : std::uint32_t {
  Core = 1,
  IntegerALU = 2,
  Memory = 3,
  Branch = 4,
  MultiplyDivide = 5,
  FloatingPoint = 6,
  System = 7,
};

enum class PressureSet : std::uint32_t {
  GPR = 1,
  FPR = 2,
};

[[nodiscard]] constexpr ProcessorResourceID resource(Resource Value) noexcept {
  return ProcessorResourceID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] constexpr RegisterPressureSetID pressureSet(PressureSet Value) noexcept {
  return RegisterPressureSetID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] constexpr RegisterClassID registerClass(RegisterClass Value) noexcept {
  return RegisterClassID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] std::vector<SchedulingResourceUse> resourceUses(SchedulingClass Class) {
  switch (Class) {
  case SchedulingClass::ALU:
  case SchedulingClass::COMPRESSED:
    return {{resource(Resource::IntegerALU), 1}};
  case SchedulingClass::LOAD:
  case SchedulingClass::STORE:
    return {{resource(Resource::Memory), 1}};
  case SchedulingClass::BRANCH:
    return {{resource(Resource::Branch), 1}};
  case SchedulingClass::SYSTEM:
  case SchedulingClass::FENCE:
    return {{resource(Resource::System), 1}};
  case SchedulingClass::MUL:
    return {{resource(Resource::MultiplyDivide), 3}};
  case SchedulingClass::DIV:
    return {{resource(Resource::MultiplyDivide), 20}};
  case SchedulingClass::ATOMIC:
    return {{resource(Resource::Memory), 4}, {resource(Resource::System), 1}};
  case SchedulingClass::FP_ADD:
  case SchedulingClass::FP_MUL:
  case SchedulingClass::FP_CONVERT:
    return {{resource(Resource::FloatingPoint), 4}};
  case SchedulingClass::FP_DIV:
  case SchedulingClass::FP_SQRT:
    return {{resource(Resource::FloatingPoint), 20}};
  case SchedulingClass::None:
    return {};
  }
  return {};
}

[[nodiscard]] TargetSchedulingModelDesc createDescription() {
  TargetSchedulingModelDesc Description;
  Description.IssueWidth = 2;
  Description.DispatchLimit = 2;
  Description.ProcessorResources = {
      {resource(Resource::Core), "core", 2, std::nullopt},
      {resource(Resource::IntegerALU), "integer-alu", 2, resource(Resource::Core)},
      {resource(Resource::Memory), "memory", 1, resource(Resource::Core)},
      {resource(Resource::Branch), "branch", 1, resource(Resource::Core)},
      {resource(Resource::MultiplyDivide), "multiply-divide", 1, resource(Resource::Core)},
      {resource(Resource::FloatingPoint), "floating-point", 1, resource(Resource::Core)},
      {resource(Resource::System), "system", 1, resource(Resource::Core)},
  };
  for (const SchedulingClassInfo &Class : schedulingClasses()) {
    SchedulingClassModelDesc Model{
        .ID = Class.ID,
        .Name = std::string(Class.Name),
        .MicroOps = Class.MicroOps,
        .Latency = Class.Latency,
        .Resources = resourceUses(static_cast<SchedulingClass>(Class.ID.value())),
        .WriteLatencies = {},
        .ReadAdvances = {},
    };
    Model.WriteLatencies.push_back({0, Class.Latency});
    switch (static_cast<SchedulingClass>(Class.ID.value())) {
    case SchedulingClass::ALU:
      Model.ReadAdvances = {
          {1, SchedulingClassID::fromValue(static_cast<std::uint32_t>(SchedulingClass::LOAD)), 1},
          {2, SchedulingClassID::fromValue(static_cast<std::uint32_t>(SchedulingClass::LOAD)), 1},
      };
      break;
    default:
      break;
    }
    Description.SchedulingClasses.push_back(std::move(Model));
  }
  Description.PressureSets = {
      {pressureSet(PressureSet::GPR),
       "gpr",
       {registerClass(RegisterClass::GPR), registerClass(RegisterClass::GPRC),
        registerClass(RegisterClass::GPR_NX0), registerClass(RegisterClass::GPR_NX0_NX2),
        registerClass(RegisterClass::GPRJALRNonX7)},
       27},
      {pressureSet(PressureSet::FPR),
       "fpr",
       {registerClass(RegisterClass::FPR64), registerClass(RegisterClass::FPR64C),
        registerClass(RegisterClass::FPR32), registerClass(RegisterClass::FPR32C)},
       32},
  };
  return Description;
}

} // namespace

const TargetSchedulingModel &schedulingModel() noexcept {
  static const Expected<TargetSchedulingModel> Model =
      TargetSchedulingModel::create(createDescription());
  if (!Model)
    std::abort();
  return *Model;
}

} // namespace rcc::riscv
