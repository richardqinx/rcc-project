// SPDX-License-Identifier: GPL-3.0-or-later

#include "RegAllocBase.h"

#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/Spiller.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace rcc {

std::vector<const LiveInterval *>
orderedAllocationIntervals(const LiveIntervalsAnalysis::Result &Intervals,
                           const MachineFunction &Function) {
  std::vector<const LiveInterval *> Result;
  for (std::size_t Index = 0; Index < Function.registerInfo().virtualRegisters().size(); ++Index) {
    auto Reg = Register::makeVirtual(Index);
    if (!Reg)
      continue;
    const LiveInterval *Interval = Intervals.interval(*Reg);
    if (Interval != nullptr && !Interval->MainRange.empty())
      Result.push_back(Interval);
  }
  std::ranges::sort(Result, [](const LiveInterval *Left, const LiveInterval *Right) {
    const LiveSegment &LeftSegment = Left->MainRange.segments().front();
    const LiveSegment &RightSegment = Right->MainRange.segments().front();
    if (LeftSegment.Start != RightSegment.Start)
      return LeftSegment.Start < RightSegment.Start;
    if (LeftSegment.End != RightSegment.End)
      return LeftSegment.End < RightSegment.End;
    return Left->Reg.opaqueValue() < Right->Reg.opaqueValue();
  });
  return Result;
}

Expected<regalloc_detail::AllocationRunResult> RegAllocBase::run() {
  const std::uint64_t InitialFingerprint = regalloc_detail::allocationFingerprint(Function);
  const std::size_t InitialFrameObjects = Function.frameInfo().liveObjectCount();
  VirtRegMap Mapping(Function);
  auto Initialized = regalloc_detail::initializeMapping(Function, Mapping);
  if (!Initialized)
    return std::unexpected(Initialized.error());

  std::unordered_set<std::uint64_t> SeenFingerprints;
  while (true) {
    const std::uint64_t Fingerprint = regalloc_detail::allocationFingerprint(Function);
    if (!SeenFingerprints.insert(Fingerprint).second) {
      return makeUnexpected(ErrorCode::InternalError,
                            "register allocation spill/rebuild cycle made no progress");
    }
    auto Intervals = Analyses.getResult<LiveIntervalsAnalysis>(Function, Target);
    if (!Intervals)
      return std::unexpected(Intervals.error());
    auto Round = allocateRound(**Intervals, Mapping);
    if (!Round)
      return std::unexpected(Round.error());
    if (Round->Spilled.empty())
      break;

    std::ranges::sort(Round->Spilled, [](Register Left, Register Right) {
      return Left.opaqueValue() < Right.opaqueValue();
    });
    Spiller ValueSpiller(Function, Target, TargetInfo, Mapping, Options.EnableRematerialization,
                         Actions);
    for (const Register VirtualReg : Round->Spilled) {
      VirtRegMapEntry *Entry = Mapping.entry(VirtualReg);
      if (Entry != nullptr) {
        Entry->PhysicalReg.reset();
        Entry->State = VirtRegAllocationState::Unassigned;
      }
      auto NewRegisters = ValueSpiller.spill(VirtualReg);
      if (!NewRegisters)
        return std::unexpected(NewRegisters.error());
    }
    Analyses.clear(Function);
  }

  auto Rewritten = rewriteVirtualRegisters(Function, Target, Mapping);
  if (!Rewritten)
    return std::unexpected(Rewritten.error());
  const std::uint64_t FinalFingerprint = regalloc_detail::allocationFingerprint(Function);
  regalloc_detail::AllocationRunResult Result;
  Result.Changed = InitialFingerprint != FinalFingerprint || *Rewritten;
  Result.ChangedInstructions = InitialFingerprint != FinalFingerprint;
  Result.ChangedFrame = InitialFrameObjects != Function.frameInfo().liveObjectCount();
  return Result;
}

} // namespace rcc
