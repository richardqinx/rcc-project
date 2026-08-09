// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/ScheduleHazardRecognizer.h"

#include "rcc/CodeGen/ScheduleDAG.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace rcc {
namespace {

[[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t Left, std::uint64_t Right) noexcept {
  return std::numeric_limits<std::uint64_t>::max() - Left < Right
             ? std::numeric_limits<std::uint64_t>::max()
             : Left + Right;
}

} // namespace

Expected<ScheduleHazardRecognizer>
ScheduleHazardRecognizer::create(const TargetSchedulingModel &Model, ScheduleDirection Direction) {
  ScheduleHazardRecognizer Result(Model, Direction);
  Result.TargetHazards = Model.hooks().createHazardRecognizer(Direction);
  auto Reset = Result.reset();
  if (!Reset)
    return std::unexpected(Reset.error());
  return Result;
}

ScheduleHazardRecognizer::ScheduleHazardRecognizer(const ScheduleHazardRecognizer &Other)
    : Model(Other.Model), Direction(Other.Direction),
      TargetHazards(Other.TargetHazards ? Other.TargetHazards->clone() : nullptr),
      Resources(Other.Resources), ResourceIndexes(Other.ResourceIndexes),
      CurrentCycle(Other.CurrentCycle), DispatchAvailableAt(Other.DispatchAvailableAt),
      IssuedMicroOps(Other.IssuedMicroOps), DispatchedUnits(Other.DispatchedUnits) {}

ScheduleHazardRecognizer &
ScheduleHazardRecognizer::operator=(const ScheduleHazardRecognizer &Other) {
  if (this == &Other)
    return *this;
  Model = Other.Model;
  Direction = Other.Direction;
  TargetHazards = Other.TargetHazards ? Other.TargetHazards->clone() : nullptr;
  Resources = Other.Resources;
  ResourceIndexes = Other.ResourceIndexes;
  CurrentCycle = Other.CurrentCycle;
  DispatchAvailableAt = Other.DispatchAvailableAt;
  IssuedMicroOps = Other.IssuedMicroOps;
  DispatchedUnits = Other.DispatchedUnits;
  return *this;
}

Expected<void> ScheduleHazardRecognizer::reset() {
  Resources.clear();
  ResourceIndexes.clear();
  Resources.reserve(Model->processorResources().size());
  for (const ProcessorResourceDesc &Resource : Model->processorResources()) {
    ResourceIndexes.emplace(Resource.ID, Resources.size());
    Resources.push_back({Resource.ID, std::vector<std::uint64_t>(Resource.Units, 0)});
  }
  CurrentCycle = 0;
  DispatchAvailableAt = 0;
  IssuedMicroOps = 0;
  DispatchedUnits = 0;
  if (TargetHazards != nullptr) {
    auto Reset = TargetHazards->reset();
    if (!Reset)
      return std::unexpected(Reset.error());
  }
  return {};
}

Expected<bool> ScheduleHazardRecognizer::canIssue(const ScheduleUnit &Unit,
                                                  std::uint64_t Cycle) const {
  ScheduleHazardRecognizer Trial = *this;
  auto Issued = Trial.issueImplementation(Unit, Cycle);
  if (!Issued) {
    if (Issued.error().code() == ErrorCode::SchedulingFailed)
      return false;
    return std::unexpected(Issued.error());
  }
  return true;
}

Expected<std::uint64_t> ScheduleHazardRecognizer::earliestIssueCycle(const ScheduleUnit &Unit,
                                                                     std::uint64_t Earliest) const {
  std::uint64_t Candidate = std::max(Earliest, CurrentCycle);
  for (;;) {
    auto Available = canIssue(Unit, Candidate);
    if (!Available)
      return std::unexpected(Available.error());
    if (*Available)
      return Candidate;
    if (Candidate == std::numeric_limits<std::uint64_t>::max()) {
      return makeUnexpected(ErrorCode::SchedulingFailed,
                            "scheduler resource timeline is exhausted");
    }
    ++Candidate;
  }
}

Expected<void> ScheduleHazardRecognizer::issue(const ScheduleUnit &Unit, std::uint64_t Cycle) {
  ScheduleHazardRecognizer Trial = *this;
  auto Issued = Trial.issueImplementation(Unit, Cycle);
  if (!Issued)
    return std::unexpected(Issued.error());
  *this = std::move(Trial);
  return {};
}

Expected<void> ScheduleHazardRecognizer::issueImplementation(const ScheduleUnit &Unit,
                                                             std::uint64_t Cycle) {
  if (Cycle < CurrentCycle) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "scheduler cannot issue into an earlier cycle");
  }
  auto Moved = moveToCycle(Cycle);
  if (!Moved)
    return std::unexpected(Moved.error());
  if (Cycle < DispatchAvailableAt) {
    return makeUnexpected(ErrorCode::SchedulingFailed, "scheduler dispatch pipeline is occupied");
  }
  if (TargetHazards != nullptr) {
    auto Hazard = TargetHazards->getHazardType(Unit);
    if (!Hazard)
      return std::unexpected(Hazard.error());
    if (*Hazard != ScheduleHazardType::NoHazard) {
      return makeUnexpected(ErrorCode::SchedulingFailed,
                            *Hazard == ScheduleHazardType::NoopHazard
                                ? "target hazard requires an unavailable scheduling noop"
                                : "target dynamic scheduling hazard prevents issue");
    }
  }

  const std::uint32_t Width = Model->issueWidth();
  if (Unit.MicroOps <= Width) {
    if (Unit.MicroOps > Width - IssuedMicroOps || DispatchedUnits == Model->dispatchLimit()) {
      return makeUnexpected(ErrorCode::SchedulingFailed,
                            "scheduler issue or dispatch width is exhausted");
    }
    IssuedMicroOps += Unit.MicroOps;
    ++DispatchedUnits;
  } else {
    if (IssuedMicroOps != 0 || DispatchedUnits != 0) {
      return makeUnexpected(ErrorCode::SchedulingFailed,
                            "multi-cycle dispatch requires an empty issue cycle");
    }
    const std::uint64_t DispatchCycles =
        (static_cast<std::uint64_t>(Unit.MicroOps) + Width - 1U) / Width;
    DispatchAvailableAt = saturatingAdd(Cycle, DispatchCycles);
    IssuedMicroOps = Width;
    DispatchedUnits = 1;
  }

  for (const SchedulingResourceUse &Use : Unit.Resources) {
    auto Reserved = reserveResource(Use.Resource, Use.Cycles, Cycle);
    if (!Reserved)
      return std::unexpected(Reserved.error());
  }
  if (TargetHazards != nullptr) {
    auto Emitted = TargetHazards->emitInstruction(Unit);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }
  return {};
}

Expected<void> ScheduleHazardRecognizer::moveToCycle(std::uint64_t Cycle) {
  while (CurrentCycle < Cycle) {
    if (TargetHazards != nullptr) {
      Expected<void> Advanced = Direction == ScheduleDirection::TopDown
                                    ? TargetHazards->advanceCycle()
                                    : TargetHazards->recedeCycle();
      if (!Advanced)
        return std::unexpected(Advanced.error());
    }
    ++CurrentCycle;
    IssuedMicroOps = 0;
    DispatchedUnits = 0;
  }
  return {};
}

Expected<void> ScheduleHazardRecognizer::reserveResource(ProcessorResourceID Resource,
                                                         std::uint32_t Cycles,
                                                         std::uint64_t Cycle) {
  const auto Position = ResourceIndexes.find(Resource);
  const ProcessorResourceDesc *Description = Model->processorResource(Resource);
  if (Position == ResourceIndexes.end() || Description == nullptr || Cycles == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "schedule unit references an invalid processor resource");
  }
  ResourceState &State = Resources[Position->second];
  std::optional<std::size_t> Selected;
  for (std::size_t Index = 0; Index < State.AvailableAt.size(); ++Index) {
    if (State.AvailableAt[Index] <= Cycle &&
        (!Selected || State.AvailableAt[Index] < State.AvailableAt[*Selected])) {
      Selected = Index;
    }
  }
  if (!Selected) {
    return makeUnexpected(ErrorCode::SchedulingFailed, "processor resource has no available unit");
  }
  State.AvailableAt[*Selected] = saturatingAdd(Cycle, Cycles);
  if (Description->Parent) {
    auto Parent = reserveResource(*Description->Parent, Cycles, Cycle);
    if (!Parent)
      return std::unexpected(Parent.error());
  }
  return {};
}

} // namespace rcc
