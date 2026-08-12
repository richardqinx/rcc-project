// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LivePhysRegs.h"

#include "LiveTestSupport.h"
#include "rcc/CodeGen/LiveVariables.h"

namespace {

using namespace rcc::test::liveness;

void testPostRALivenessRepair(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("post-ra-repair", 12, 1);
  if (!Fixture) {
    Context.expect(false, "post-RA repair fixture creation must succeed");
    return;
  }
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  auto Def = buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], G0);
  auto FirstUse = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], G0);
  auto LastUse = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], G0);
  if (!Def || !FirstUse || !LastUse) {
    Context.expect(false, "post-RA repair instructions must be built");
    return;
  }

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Liveness = Registered
                      ? Analyses.getResult<rcc::LiveVariablesAnalysis>(*Fixture->Function, Target)
                      : rcc::Expected<rcc::LiveVariablesAnalysis::Result *>{
                            std::unexpected(Registered.error())};
  if (!Liveness) {
    Context.expect(false, "post-RA repair liveness must materialize");
    return;
  }
  const std::array Blocks{Fixture->Blocks[0]};
  auto Repaired = rcc::repairPostRAMachineBlockLiveness(Blocks, Target, **Liveness);
  Context.expect(Repaired && *Repaired, "post-RA repair must update stale kill/dead flags");
  Context.expect(!hasFlag(**Def, 0, rcc::RegisterOperandFlag::Dead) &&
                     !hasFlag(**FirstUse, 0, rcc::RegisterOperandFlag::Kill) &&
                     hasFlag(**LastUse, 0, rcc::RegisterOperandFlag::Kill),
                 "post-RA repair must derive precise physical kill/dead flags");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();
  testPostRALivenessRepair(Context, *Target);
  return Context.result();
}
