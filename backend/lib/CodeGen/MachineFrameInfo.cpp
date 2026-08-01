// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFrameInfo.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>

namespace rcc {

Expected<StackID> StackID::targetStack(std::uint32_t Value) {
  if (Value < FirstTargetStackValue) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target stack ID must not overlap a built-in stack ID");
  }
  return StackID(Value);
}

bool StackObject::isSpill() const noexcept {
  const StackObjectDesc *Desc = description();
  if (Desc == nullptr)
    return false;
  return Desc->Kind == StackObjectKind::SpillSlot || Desc->Kind == StackObjectKind::CalleeSaved ||
         Desc->Kind == StackObjectKind::EmergencySpill;
}

bool StackObject::isFixed() const noexcept {
  const StackObjectDesc *Desc = description();
  return Desc != nullptr && Desc->Kind == StackObjectKind::FixedABI;
}

bool StackObject::isVariableSized() const noexcept {
  const StackObjectDesc *Desc = description();
  return Desc != nullptr && (Desc->Kind == StackObjectKind::VariableSized ||
                             Desc->Kind == StackObjectKind::DynamicAlloca);
}

Expected<void> MachineFrameInfo::verifyDescription(const StackObjectDesc &Description) {
  if (Description.Kind >= StackObjectKind::Count)
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object kind is invalid");
  if (!Description.Stack.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object has an invalid stack ID");

  const bool IsVariable = Description.Kind == StackObjectKind::VariableSized ||
                          Description.Kind == StackObjectKind::DynamicAlloca;
  if (IsVariable) {
    if (Description.Size || Description.Offset) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variable-sized stack objects cannot have a fixed size or offset");
    }
  } else if (!Description.Size || *Description.Size == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "fixed-size stack objects require a nonzero size");
  }

  if (Description.Kind == StackObjectKind::FixedABI && !Description.Offset) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "fixed ABI stack objects require a fixed offset");
  }
  if (Description.Kind == StackObjectKind::Target && !Description.Stack.isTargetStack() &&
      Description.TargetFlags == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target stack objects require a target stack ID or nonzero target flags");
  }

  std::unordered_set<MachineInstrStableID> LifetimeBoundaries;
  for (const StackObjectLifetimeRange &Range : Description.Lifetime) {
    if (!Range.Begin.isValid() || !Range.End.isValid() || Range.Begin == Range.End) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "stack object lifetime boundaries must be valid and distinct");
    }
    if (!LifetimeBoundaries.insert(Range.Begin).second ||
        !LifetimeBoundaries.insert(Range.End).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "stack object lifetime boundaries must not be reused");
    }
  }
  if (Description.DebugInfo) {
    auto DebugInfoResult = Description.DebugInfo->verifyStructure();
    if (!DebugInfoResult)
      return DebugInfoResult;
  }
  return {};
}

Expected<FrameIndex> MachineFrameInfo::createObject(StackObjectDesc Description) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen frame cannot create stack objects");
  auto DescriptionResult = verifyDescription(Description);
  if (!DescriptionResult)
    return std::unexpected(DescriptionResult.error());
  if (Objects.size() > std::numeric_limits<std::uint32_t>::max()) {
    return makeUnexpected(ErrorCode::Unsupported, "stack object index space is exhausted");
  }

  const FrameIndex Index = FrameIndex::fromValue(static_cast<std::uint32_t>(Objects.size()));
  Objects.push_back(StackObject(Index, std::move(Description)));
  return Index;
}

Expected<StackObject *> MachineFrameInfo::mutableObject(FrameIndex Index) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frozen frame cannot modify stack objects");
  if (Index.value() >= Objects.size())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object index is out of range");
  StackObject &Object = Objects[Index.value()];
  if (Object.isTombstone())
    return makeUnexpected(ErrorCode::InvalidArgument, "stack object index is a tombstone");
  return &Object;
}

Expected<void> MachineFrameInfo::updateObject(FrameIndex Index, StackObjectDesc Description) {
  auto DescriptionResult = verifyDescription(Description);
  if (!DescriptionResult)
    return std::unexpected(DescriptionResult.error());
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  (*Object)->Description.emplace<StackObjectDesc>(std::move(Description));
  return {};
}

Expected<void> MachineFrameInfo::setObjectOffset(FrameIndex Index, std::int64_t Offset) {
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  StackObjectDesc *CurrentDescription = std::get_if<StackObjectDesc>(&(*Object)->Description);
  if (CurrentDescription == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "live stack object unexpectedly lacks a description");
  }
  StackObjectDesc Description = *CurrentDescription;
  if ((*Object)->isVariableSized()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "variable-sized stack object cannot have a fixed offset");
  }
  Description.Offset = Offset;
  (*Object)->Description.emplace<StackObjectDesc>(std::move(Description));
  return {};
}

Expected<void> MachineFrameInfo::markObjectDead(FrameIndex Index) {
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  StackObjectDesc *Description = std::get_if<StackObjectDesc>(&(*Object)->Description);
  if (Description == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "live stack object unexpectedly lacks a description");
  }
  Description->IsDead = true;
  return {};
}

Expected<void> MachineFrameInfo::eraseObject(FrameIndex Index) {
  auto Object = mutableObject(Index);
  if (!Object)
    return std::unexpected(Object.error());
  (*Object)->Description.emplace<std::monostate>();
  return {};
}

const StackObject *MachineFrameInfo::object(FrameIndex Index) const noexcept {
  if (Index.value() >= Objects.size() || Objects[Index.value()].isTombstone())
    return nullptr;
  return &Objects[Index.value()];
}

std::size_t MachineFrameInfo::liveObjectCount() const noexcept {
  std::size_t Count = 0;
  for (const StackObject &Object : Objects) {
    if (!Object.isTombstone())
      ++Count;
  }
  return Count;
}

Expected<void> MachineFrameInfo::setMaxCallFrameSize(std::uint64_t Size) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "frozen frame cannot change its maximum call-frame size");
  MaxCallFrameSize = Size;
  return {};
}

Expected<void> MachineFrameInfo::freeze(std::uint64_t Size, Alignment Align) {
  if (Frozen)
    return makeUnexpected(ErrorCode::InvalidArgument, "frame layout is already frozen");
  for (const StackObject &Object : Objects) {
    if (Object.isTombstone() || Object.isVariableSized())
      continue;
    if (!Object.description()->Offset) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "fixed-size stack object is missing its final offset");
    }
  }
  if (Size % Align.bytes() != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "static stack size must be aligned to the stack alignment");
  }

  StaticStackSize = Size;
  StackAlign = Align;
  Frozen = true;
  return {};
}

Expected<void> MachineFrameInfo::verify() const {
  for (std::size_t Position = 0; Position < Objects.size(); ++Position) {
    const StackObject &Object = Objects[Position];
    if (Object.index() != FrameIndex::fromValue(static_cast<std::uint32_t>(Position))) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object index does not match its stable table position");
    }
    if (Object.isTombstone())
      continue;
    auto DescriptionResult = verifyDescription(*Object.description());
    if (!DescriptionResult)
      return std::unexpected(DescriptionResult.error());
    if (Frozen && !Object.isVariableSized() && !Object.description()->Offset) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "frozen fixed-size stack object has no final offset");
    }
  }

  if (Frozen != StaticStackSize.has_value()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "frame frozen state and static stack size are inconsistent");
  }
  if (Frozen && *StaticStackSize % StackAlign.bytes() != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "frozen static stack size violates stack alignment");
  }
  return {};
}

} // namespace rcc
