// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetSchedulingModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace rcc {
namespace {

template <typename IDT> [[nodiscard]] bool isValidID(IDT ID) noexcept {
  return ID.value() != 0;
}

} // namespace

Expected<ScheduleHazardType>
TargetScheduleHazardRecognizer::getHazardType(const ScheduleUnit &) const {
  return ScheduleHazardType::NoHazard;
}

Expected<void> TargetScheduleHazardRecognizer::emitInstruction(const ScheduleUnit &) {
  return {};
}

Expected<void> TargetScheduleHazardRecognizer::advanceCycle() {
  return {};
}

Expected<void> TargetScheduleHazardRecognizer::recedeCycle() {
  return {};
}

Expected<void> TargetScheduleHazardRecognizer::reset() {
  return {};
}

bool TargetSchedulingHooks::isSchedulingBoundary(const MachineInstr &,
                                                 const TargetDescription &) const noexcept {
  return false;
}

Expected<void> TargetSchedulingHooks::mutateScheduleDAG(ScheduleDAG &, const MachineFunction &,
                                                        const TargetDescription &, ScheduleDAGMode,
                                                        bool) const {
  return {};
}

std::int64_t TargetSchedulingHooks::candidatePreference(const ScheduleUnit &, ScheduleDirection,
                                                        const MachineBasicBlock &,
                                                        std::uint64_t) const noexcept {
  return 0;
}

std::unique_ptr<TargetScheduleHazardRecognizer>
TargetSchedulingHooks::createHazardRecognizer(ScheduleDirection) const {
  return nullptr;
}

Expected<TargetSchedulingModel>
TargetSchedulingModel::create(TargetSchedulingModelDesc Description) {
  if (Description.IssueWidth == 0 || Description.DispatchLimit == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target scheduling model requires nonzero issue and dispatch widths");
  }

  TargetSchedulingModel Result(std::move(Description));
  std::unordered_set<std::string> Names;
  for (std::size_t Index = 0; Index < Result.Description.ProcessorResources.size(); ++Index) {
    const ProcessorResourceDesc &Resource = Result.Description.ProcessorResources[Index];
    if (!isValidID(Resource.ID) || Resource.Name.empty() || Resource.Units == 0 ||
        !Result.ResourceToIndex.emplace(Resource.ID, Index).second ||
        !Names.insert(Resource.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target scheduling model has an invalid processor resource");
    }
  }
  for (const ProcessorResourceDesc &Resource : Result.Description.ProcessorResources) {
    if (Resource.Parent &&
        (Resource.Parent == Resource.ID || !Result.ResourceToIndex.contains(*Resource.Parent))) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "processor resource has an invalid parent resource");
    }
    std::unordered_set<ProcessorResourceID> Path;
    const ProcessorResourceDesc *Current = &Resource;
    while (Current->Parent) {
      if (!Path.insert(Current->ID).second) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "processor resource hierarchy contains a cycle");
      }
      Current = &Result.Description.ProcessorResources[Result.ResourceToIndex.at(*Current->Parent)];
    }
  }

  Names.clear();
  for (std::size_t Index = 0; Index < Result.Description.SchedulingClasses.size(); ++Index) {
    SchedulingClassModelDesc &Class = Result.Description.SchedulingClasses[Index];
    if (!isValidID(Class.ID) || Class.Name.empty() || Class.MicroOps == 0 || Class.Latency == 0 ||
        !Result.ClassToIndex.emplace(Class.ID, Index).second || !Names.insert(Class.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target scheduling model has an invalid scheduling class");
    }
  }
  for (SchedulingClassModelDesc &Class : Result.Description.SchedulingClasses) {
    std::unordered_set<ProcessorResourceID> UsedResources;
    for (const SchedulingResourceUse &Use : Class.Resources) {
      if (!isValidID(Use.Resource) || Use.Cycles == 0 ||
          !Result.ResourceToIndex.contains(Use.Resource) ||
          !UsedResources.insert(Use.Resource).second) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "scheduling class has an invalid resource use");
      }
    }
    std::ranges::sort(Class.Resources, {},
                      [](const SchedulingResourceUse &Use) { return Use.Resource.value(); });

    std::unordered_set<std::uint32_t> WriteOperands;
    for (const SchedulingWriteLatency &Latency : Class.WriteLatencies) {
      if (!WriteOperands.insert(Latency.OperandIndex).second) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "scheduling class has duplicate write latencies");
      }
    }
    std::ranges::sort(Class.WriteLatencies, {}, &SchedulingWriteLatency::OperandIndex);

    std::unordered_set<std::uint64_t> ReadAdvanceKeys;
    for (const SchedulingReadAdvance &Advance : Class.ReadAdvances) {
      if (Advance.ProducerClass && !Result.ClassToIndex.contains(*Advance.ProducerClass)) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "read advance references an unknown producer class");
      }
      const std::uint64_t Producer = Advance.ProducerClass ? Advance.ProducerClass->value() : 0;
      const std::uint64_t Key = (Producer << 32U) | Advance.OperandIndex;
      if (!ReadAdvanceKeys.insert(Key).second) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "scheduling class has duplicate read advances");
      }
    }
    std::ranges::sort(Class.ReadAdvances,
                      [](const SchedulingReadAdvance &Left, const SchedulingReadAdvance &Right) {
                        if (Left.OperandIndex != Right.OperandIndex)
                          return Left.OperandIndex < Right.OperandIndex;
                        return Left.ProducerClass.value_or(SchedulingClassID{}).value() <
                               Right.ProducerClass.value_or(SchedulingClassID{}).value();
                      });
  }

  Names.clear();
  for (std::size_t Index = 0; Index < Result.Description.PressureSets.size(); ++Index) {
    RegisterPressureSetDesc &Set = Result.Description.PressureSets[Index];
    if (!isValidID(Set.ID) || Set.Name.empty() || Set.Limit == 0 || Set.RegisterClasses.empty() ||
        !Result.PressureSetToIndex.emplace(Set.ID, Index).second ||
        !Names.insert(Set.Name).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target scheduling model has an invalid register pressure set");
    }
    std::ranges::sort(Set.RegisterClasses, [](RegisterClassID Left, RegisterClassID Right) {
      return Left.value() < Right.value();
    });
    if (std::ranges::adjacent_find(Set.RegisterClasses) != Set.RegisterClasses.end() ||
        std::ranges::any_of(Set.RegisterClasses,
                            [](RegisterClassID Class) { return !isValidID(Class); })) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "register pressure set has duplicate or invalid register classes");
    }
    for (const RegisterClassID Class : Set.RegisterClasses)
      Result.ClassToPressureSets[Class].push_back(Set.ID);
  }
  for (auto &[Class, Sets] : Result.ClassToPressureSets) {
    (void)Class;
    std::ranges::sort(Sets, [](RegisterPressureSetID Left, RegisterPressureSetID Right) {
      return Left.value() < Right.value();
    });
  }
  return Result;
}

const ProcessorResourceDesc *
TargetSchedulingModel::processorResource(ProcessorResourceID ID) const noexcept {
  const auto Position = ResourceToIndex.find(ID);
  return Position == ResourceToIndex.end() ? nullptr
                                           : &Description.ProcessorResources[Position->second];
}

const SchedulingClassModelDesc *
TargetSchedulingModel::schedulingClass(SchedulingClassID ID) const noexcept {
  const auto Position = ClassToIndex.find(ID);
  return Position == ClassToIndex.end() ? nullptr
                                        : &Description.SchedulingClasses[Position->second];
}

const RegisterPressureSetDesc *
TargetSchedulingModel::pressureSet(RegisterPressureSetID ID) const noexcept {
  const auto Position = PressureSetToIndex.find(ID);
  return Position == PressureSetToIndex.end() ? nullptr
                                              : &Description.PressureSets[Position->second];
}

std::span<const RegisterPressureSetID>
TargetSchedulingModel::pressureSetsForClass(RegisterClassID Class) const noexcept {
  const auto Position = ClassToPressureSets.find(Class);
  return Position == ClassToPressureSets.end()
             ? std::span<const RegisterPressureSetID>{}
             : std::span<const RegisterPressureSetID>(Position->second);
}

Expected<std::uint32_t> TargetSchedulingModel::operandLatency(SchedulingClassID ProducerClass,
                                                              std::size_t DefinitionOperand,
                                                              SchedulingClassID ConsumerClass,
                                                              std::size_t UseOperand) const {
  if (DefinitionOperand > std::numeric_limits<std::uint32_t>::max() ||
      UseOperand > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::SchedulingFailed,
                          "scheduling operand index exceeds the target model range");
  }
  const SchedulingClassModelDesc *Producer = schedulingClass(ProducerClass);
  const SchedulingClassModelDesc *Consumer = schedulingClass(ConsumerClass);
  if (Producer == nullptr || Consumer == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "operand latency references an unknown scheduling class");
  }

  const std::uint32_t DefinitionIndex = static_cast<std::uint32_t>(DefinitionOperand);
  const std::uint32_t UseIndex = static_cast<std::uint32_t>(UseOperand);
  std::uint32_t Latency = Producer->Latency;
  if (const auto Position = std::ranges::find(Producer->WriteLatencies, DefinitionIndex,
                                              &SchedulingWriteLatency::OperandIndex);
      Position != Producer->WriteLatencies.end()) {
    Latency = Position->Cycles;
  }

  const SchedulingReadAdvance *Wildcard = nullptr;
  const SchedulingReadAdvance *Exact = nullptr;
  for (const SchedulingReadAdvance &Advance : Consumer->ReadAdvances) {
    if (Advance.OperandIndex != UseIndex)
      continue;
    if (!Advance.ProducerClass)
      Wildcard = &Advance;
    else if (*Advance.ProducerClass == ProducerClass)
      Exact = &Advance;
  }
  const SchedulingReadAdvance *Advance = Exact != nullptr ? Exact : Wildcard;
  return Advance == nullptr || Advance->Cycles >= Latency ? (Advance == nullptr ? Latency : 0U)
                                                          : Latency - Advance->Cycles;
}

const TargetSchedulingHooks &TargetSchedulingModel::hooks() const noexcept {
  static const TargetSchedulingHooks DefaultHooks;
  return Description.Hooks == nullptr ? DefaultHooks : *Description.Hooks;
}

} // namespace rcc
