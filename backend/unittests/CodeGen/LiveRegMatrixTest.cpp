// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveRegMatrix.h"

#include "CodeGen/RegisterAllocationTestSupport.h"

#include <algorithm>
#include <iostream>

namespace {

using namespace rcc;
using namespace rcc::test;
using namespace rcc::test::ra;

void testLaneAndAssignment(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("matrix-lanes", 1);
  if (!Fixture)
    return;
  auto Low = createVReg(*Fixture->Function);
  auto High = createVReg(*Fixture->Function);
  if (!Low || !High ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *Low, 1, TestSubReg::Low32) ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *High, 2, TestSubReg::High32) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *High, TestSubReg::High32) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Low, TestSubReg::Low32))
    return;

  MachineFunctionAnalysisManager Analyses;
  if (!registerStage11MachineAnalyses(Analyses))
    return;
  auto Intervals = Analyses.getResult<LiveIntervalsAnalysis>(*Fixture->Function, Target);
  if (!Intervals)
    return;
  const PhysicalRegister G0 = physical(TestRegister::G0);
  LiveRegMatrix Matrix(*Fixture->Function, Target, **Intervals);
  Context.expect(Matrix.assign(*Low, G0).has_value() &&
                     Matrix.checkInterference(*High, G0).Kind == RegisterInterferenceKind::Free &&
                     Matrix.assign(*High, G0).has_value() && Matrix.assignment(*Low) == G0,
                 "LiveRegMatrix must allow disjoint lanes and publish assignments");
  Context.expect(Matrix.unassign(*Low).has_value() && !Matrix.assignment(*Low),
                 "LiveRegMatrix must remove an assignment without affecting other lanes");
}

void testFixedAndMaskInterference(TestContext &Context, const TargetDescription &Target) {
  auto Fixture = createFunction("matrix-fixed", 2);
  if (!Fixture)
    return;
  auto Value = createVReg(*Fixture->Function);
  const PhysicalRegister G0 = physical(TestRegister::G0);
  const PhysicalRegister G1 = physical(TestRegister::G1);
  if (!Value || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 7) ||
      !buildMask(*Fixture->Function, Target, *Fixture->Block, createTestCallPreservedMask()) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, Register::fromPhysical(G0)) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value))
    return;

  MachineFunctionAnalysisManager Analyses;
  if (!registerStage11MachineAnalyses(Analyses))
    return;
  auto Intervals = Analyses.getResult<LiveIntervalsAnalysis>(*Fixture->Function, Target);
  if (!Intervals)
    return;
  LiveRegMatrix Matrix(*Fixture->Function, Target, **Intervals);
  const auto G0Interferences = Matrix.collectInterferences(*Value, G0);
  const auto G1Interferences = Matrix.collectInterferences(*Value, G1);
  const auto HasKind = [](std::span<const RegisterInterference> Values,
                          RegisterInterferenceKind Kind) {
    return std::ranges::any_of(
        Values, [Kind](const RegisterInterference &Value) { return Value.Kind == Kind; });
  };
  Context.expect(HasKind(G0Interferences, RegisterInterferenceKind::FixedRegister) &&
                     HasKind(G0Interferences, RegisterInterferenceKind::RegisterMask) &&
                     !HasKind(G1Interferences, RegisterInterferenceKind::FixedRegister) &&
                     Matrix.assign(*Value, G1).has_value(),
                 "LiveRegMatrix must distinguish fixed-register and RegMask interference");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "LiveRegMatrix test target must be constructible");
  if (!Target) {
    std::cerr << Target.error().message() << '\n';
    return Context.result();
  }
  testLaneAndAssignment(Context, *Target);
  testFixedAndMaskInterference(Context, *Target);
  return Context.result();
}
