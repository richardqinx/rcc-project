// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveInterval.h"

#include "LiveTestSupport.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/LiveVariables.h"

namespace {

using namespace rcc::test::liveness;

void testLiveRange(TestContext &Context) {
  const rcc::SlotIndex S16 = rcc::SlotIndex::fromValue(16).registerSlot();
  const rcc::SlotIndex S24 = rcc::SlotIndex::fromValue(24).registerSlot();
  const rcc::SlotIndex S32 = rcc::SlotIndex::fromValue(32).registerSlot();
  const rcc::SlotIndex S40 = rcc::SlotIndex::fromValue(40).registerSlot();
  const rcc::SlotIndex S48 = rcc::SlotIndex::fromValue(48).registerSlot();

  rcc::LiveRange Range;
  auto First = Range.createValue(S16);
  Context.expect(First.has_value(), "live value creation must succeed");
  if (!First)
    return;
  Context.expect(Range.addSegment({S16, S32, *First}).has_value() &&
                     Range.addSegment({S32, S48, *First}).has_value() &&
                     Range.segments().size() == 1,
                 "adjacent same-value live segments must merge");
  auto Second = Range.createValue(S40, rcc::LiveValueState::Redefined);
  Context.expect(Second.has_value() && !Range.addSegment({S24, S40, *Second}),
                 "overlapping different live values must be rejected");
  Context.expect(Range.valueAt(S24) != nullptr && Range.liveAt(S40) &&
                     Range.remove(S24, S40).has_value() && Range.segments().size() == 2 &&
                     Range.verify().has_value(),
                 "live-range lookup and removal must preserve ordered half-open segments");

  rcc::LiveRange Shrunk;
  auto Value = Shrunk.createValue(S16);
  if (!Value) {
    Context.expect(false, "shrink test value creation must succeed");
    return;
  }
  const std::array Uses{S32};
  Context.expect(Shrunk.addSegment({S16, S48.deadSlot(), *Value}).has_value() &&
                     Shrunk.shrinkToUses(Uses).has_value() && Shrunk.segments().size() == 1 &&
                     Shrunk.segments().front().End == S32.deadSlot(),
                 "shrinkToUses must retain exactly through the final use dead slot");
}

void testLaneSubrangesAndEarlyClobber(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("lane-subranges", 3, 1);
  if (!Fixture) {
    Context.expect(false, "lane-subrange fixture creation must succeed");
    return;
  }
  auto Reg = createVReg(*Fixture->Function);
  auto Early = createVReg(*Fixture->Function);
  if (!Reg || !Early) {
    Context.expect(false, "lane-subrange registers must be created");
    return;
  }
  auto LowDef =
      buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, rcc::test::TestSubReg::Low32);
  auto LowUse =
      buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, rcc::test::TestSubReg::Low32);
  auto HighDef = buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg,
                          rcc::test::TestSubReg::High32);
  auto HighUse = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg,
                          rcc::test::TestSubReg::High32);
  auto EarlyDef = buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Early, {}, true);
  auto EarlyUse = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Early);
  if (!LowDef || !LowUse || !HighDef || !HighUse || !EarlyDef || !EarlyUse) {
    Context.expect(false, "lane-subrange instructions must be built");
    return;
  }

  rcc::MachineFunctionAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Intervals = Registered
                       ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target)
                       : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                             std::unexpected(Registered.error())};
  if (!Intervals) {
    Context.expect(false, "lane-subrange intervals must materialize");
    return;
  }
  const rcc::LiveInterval *Interval = (*Intervals)->interval(*Reg);
  const rcc::LiveInterval *EarlyInterval = (*Intervals)->interval(*Early);
  const bool HasLow = Interval != nullptr &&
                      std::ranges::any_of(Interval->SubRanges, [](const rcc::LiveSubRange &Range) {
                        return Range.Lanes == rcc::LaneMask::fromRaw(0x3);
                      });
  const bool HasHigh = Interval != nullptr &&
                       std::ranges::any_of(Interval->SubRanges, [](const rcc::LiveSubRange &Range) {
                         return Range.Lanes == rcc::LaneMask::fromRaw(0xC);
                       });
  Context.expect(Interval != nullptr && Interval->SubRanges.size() == 2 && HasLow && HasHigh,
                 "partial virtual-register accesses must form disjoint lane subranges");
  Context.expect(EarlyInterval != nullptr && !EarlyInterval->MainRange.values().empty() &&
                     EarlyInterval->MainRange.values().front().Definition.kind() ==
                         rcc::SlotIndexKind::EarlyClobber,
                 "early-clobber definitions must use the early-clobber slot");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();
  testLiveRange(Context);
  testLaneSubrangesAndEarlyClobber(Context, *Target);
  return Context.result();
}
