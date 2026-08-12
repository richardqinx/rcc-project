// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveInterval.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace rcc {

const LiveSegment *LiveRange::segmentAt(SlotIndex Position) const noexcept {
  const auto Candidate = std::ranges::upper_bound(
      Segments, Position, {}, [](const LiveSegment &Segment) { return Segment.Start; });
  if (Candidate == Segments.begin())
    return nullptr;
  const LiveSegment &Previous = *std::prev(Candidate);
  return Previous.contains(Position) ? &Previous : nullptr;
}

const LiveValueNumber *LiveRange::value(LiveValueNumberID ID) const noexcept {
  if (!ID.isValid() || ID.value() > Values.size())
    return nullptr;
  const LiveValueNumber &Result = Values[ID.value() - 1U];
  return Result.ID == ID ? &Result : nullptr;
}

const LiveValueNumber *LiveRange::valueAt(SlotIndex Position) const noexcept {
  const LiveSegment *Segment = segmentAt(Position);
  return Segment == nullptr ? nullptr : value(Segment->Value);
}

bool LiveRange::liveAt(SlotIndex Position) const noexcept {
  return segmentAt(Position) != nullptr;
}

bool LiveRange::overlaps(const LiveRange &Other) const noexcept {
  std::size_t Left = 0;
  std::size_t Right = 0;
  while (Left < Segments.size() && Right < Other.Segments.size()) {
    if (Segments[Left].overlaps(Other.Segments[Right]))
      return true;
    if (Segments[Left].End <= Other.Segments[Right].Start)
      ++Left;
    else
      ++Right;
  }
  return false;
}

Expected<LiveValueNumberID> LiveRange::createValue(SlotIndex Definition, LiveValueState State,
                                                   std::optional<MachineInstrStableID> Origin) {
  if (!Definition.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "live value requires a valid definition");
  if (Values.size() >= std::numeric_limits<std::uint32_t>::max())
    return makeUnexpected(ErrorCode::Unsupported, "live value-number space is exhausted");
  const LiveValueNumberID ID =
      LiveValueNumberID::fromValue(static_cast<std::uint32_t>(Values.size() + 1U));
  Values.push_back({ID, Definition, State, Origin});
  return ID;
}

Expected<void> LiveRange::addSegment(LiveSegment Segment) {
  if (!Segment.Start.isValid() || !Segment.End.isValid() || Segment.empty() ||
      value(Segment.Value) == nullptr) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "live segment requires ordered indexes and a known value number");
  }
  std::vector<LiveSegment> Candidate = Segments;
  Candidate.push_back(Segment);
  std::ranges::sort(Candidate, [](const LiveSegment &Left, const LiveSegment &Right) {
    if (Left.Start != Right.Start)
      return Left.Start < Right.Start;
    return Left.End < Right.End;
  });
  for (std::size_t Index = 1; Index < Candidate.size(); ++Index) {
    if (Candidate[Index].Start < Candidate[Index - 1U].End &&
        Candidate[Index].Value != Candidate[Index - 1U].Value) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live segments with different values overlap");
    }
  }
  Segments = std::move(Candidate);
  mergeAdjacent();
  return {};
}

Expected<void> LiveRange::extend(LiveValueNumberID Value, SlotIndex Start, SlotIndex End) {
  return addSegment({Start, End, Value});
}

Expected<void> LiveRange::remove(SlotIndex Start, SlotIndex End) {
  if (!Start.isValid() || !End.isValid() || !(Start < End)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "removed live range must be nonempty");
  }
  std::vector<LiveSegment> Result;
  for (const LiveSegment &Segment : Segments) {
    if (!(Segment.Start < End && Start < Segment.End)) {
      Result.push_back(Segment);
      continue;
    }
    if (Segment.Start < Start)
      Result.push_back({Segment.Start, Start, Segment.Value});
    if (End < Segment.End)
      Result.push_back({End, Segment.End, Segment.Value});
  }
  Segments = std::move(Result);
  mergeAdjacent();
  return {};
}

Expected<void> LiveRange::shrinkToUses(std::span<const SlotIndex> Uses) {
  std::vector<LiveSegment> Result;
  for (const LiveSegment &Segment : Segments) {
    const LiveValueNumber *Value = value(Segment.Value);
    if (Value == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "live segment references an unknown value number");
    std::optional<SlotIndex> LastUse;
    for (const SlotIndex Use : Uses) {
      if (Segment.contains(Use) && (!LastUse || *LastUse < Use))
        LastUse = Use;
    }
    if (!LastUse) {
      if (Value->State == LiveValueState::Unused)
        Result.push_back(Segment);
      continue;
    }
    SlotIndex NewEnd = LastUse->deadSlot();
    if (Segment.End < NewEnd)
      NewEnd = Segment.End;
    if (Segment.Start < NewEnd)
      Result.push_back({Segment.Start, NewEnd, Segment.Value});
  }
  Segments = std::move(Result);
  mergeAdjacent();
  return verify();
}

Expected<void> LiveRange::verify() const {
  std::unordered_set<LiveValueNumberID> IDs;
  for (std::size_t Index = 0; Index < Values.size(); ++Index) {
    const LiveValueNumber &Value = Values[Index];
    if (!Value.ID.isValid() || Value.ID.value() != Index + 1U || !IDs.insert(Value.ID).second ||
        !Value.Definition.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live range contains an invalid value number");
    }
  }
  for (std::size_t Index = 0; Index < Segments.size(); ++Index) {
    const LiveSegment &Segment = Segments[Index];
    if (!Segment.Start.isValid() || !Segment.End.isValid() || Segment.empty() ||
        value(Segment.Value) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError, "live range contains an invalid segment");
    }
    if (Index != 0 && Segments[Index - 1U].End > Segment.Start) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "live range segments are not ordered and disjoint");
    }
  }
  return {};
}

void LiveRange::mergeAdjacent() {
  if (Segments.empty())
    return;
  std::vector<LiveSegment> Result;
  Result.reserve(Segments.size());
  Result.push_back(Segments.front());
  for (std::size_t Index = 1; Index < Segments.size(); ++Index) {
    LiveSegment &Previous = Result.back();
    const LiveSegment &Current = Segments[Index];
    if (Current.Value == Previous.Value && Current.Start <= Previous.End) {
      if (Previous.End < Current.End)
        Previous.End = Current.End;
      continue;
    }
    Result.push_back(Current);
  }
  Segments = std::move(Result);
}

} // namespace rcc
