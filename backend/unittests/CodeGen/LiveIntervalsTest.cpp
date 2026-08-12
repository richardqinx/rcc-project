// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveIntervals.h"

#include "LiveTestSupport.h"
#include "rcc/CodeGen/LiveRegUnits.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

namespace {

using namespace rcc::test::liveness;

void testCFGAndValueNumbers(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("cfg-liveness", 2, 8);
  if (!Fixture) {
    Context.expect(false, "CFG liveness fixture creation must succeed");
    return;
  }
  auto &Function = *Fixture->Function;
  auto &Entry = *Fixture->Blocks[0];
  auto &Left = *Fixture->Blocks[1];
  auto &Right = *Fixture->Blocks[2];
  auto &Join = *Fixture->Blocks[3];
  auto &Header = *Fixture->Blocks[4];
  auto &Body = *Fixture->Blocks[5];
  auto &Exit = *Fixture->Blocks[6];
  auto &Dead = *Fixture->Blocks[7];
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto Updater = Function.cfgUpdater();
  const bool CFGValid = Half && Updater.addSuccessor(Entry, Left, *Half) &&
                        Updater.addSuccessor(Entry, Right, *Half) &&
                        Updater.addSuccessor(Left, Join, rcc::BranchProbability::one()) &&
                        Updater.addSuccessor(Right, Join, rcc::BranchProbability::one()) &&
                        Updater.addSuccessor(Join, Header, rcc::BranchProbability::one()) &&
                        Updater.addSuccessor(Header, Body, *Half) &&
                        Updater.addSuccessor(Header, Exit, *Half) &&
                        Updater.addSuccessor(Body, Header, rcc::BranchProbability::one());
  Context.expect(CFGValid, "diamond and loop CFG construction must succeed");
  if (!CFGValid)
    return;

  auto Across = createVReg(Function);
  auto Merged = createVReg(Function);
  auto Unreachable = createVReg(Function);
  auto Redefined = createVReg(Function);
  if (!Across || !Merged || !Unreachable || !Redefined) {
    Context.expect(false, "CFG liveness virtual registers must be created");
    return;
  }
  auto AcrossDef = buildDef(Function, Target, Entry, *Across);
  auto LeftDef = buildDef(Function, Target, Left, *Merged);
  auto RightDef = buildDef(Function, Target, Right, *Merged);
  auto MergedUse = buildUse(Function, Target, Join, *Merged);
  auto LoopUse = buildUse(Function, Target, Body, *Across);
  auto DeadUse = buildUse(Function, Target, Dead, *Unreachable);
  auto FirstRedef = buildDef(Function, Target, Exit, *Redefined);
  auto SecondRedef = buildDef(Function, Target, Exit, *Redefined);
  if (!AcrossDef || !LeftDef || !RightDef || !MergedUse || !LoopUse || !DeadUse || !FirstRedef ||
      !SecondRedef) {
    Context.expect(false, "CFG liveness instructions must be built");
    return;
  }

  rcc::MachineFunctionAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Variables = Registered ? Analyses.getResult<rcc::LiveVariablesAnalysis>(Function, Target)
                              : rcc::Expected<rcc::LiveVariablesAnalysis::Result *>{
                                    std::unexpected(Registered.error())};
  auto Intervals = Registered ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(Function, Target)
                              : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                                    std::unexpected(Registered.error())};
  if (!Variables || !Intervals) {
    Context.expect(false, "CFG liveness analyses must materialize");
    return;
  }
  Context.expect((*Variables)->isLiveOut(Entry, *Across) && (*Variables)->isLiveIn(Left, *Across) &&
                     (*Variables)->isLiveIn(Right, *Across) &&
                     (*Variables)->isLiveIn(Header, *Across) &&
                     (*Variables)->isLiveOut(Body, *Across),
                 "CFG fixed point must propagate a value through a diamond and loop backedge");
  Context.expect(
      (*Variables)->isLiveOut(Left, *Merged) && (*Variables)->isLiveOut(Right, *Merged) &&
          (*Variables)->isLiveIn(Join, *Merged) && (*Variables)->isLiveIn(Dead, *Unreachable),
      "liveness must merge predecessor definitions and retain unreachable uses");

  const rcc::LiveInterval *MergedInterval = (*Intervals)->interval(*Merged);
  const rcc::LiveInterval *AcrossInterval = (*Intervals)->interval(*Across);
  const rcc::LiveInterval *RedefinedInterval = (*Intervals)->interval(*Redefined);
  const bool HasPhi = MergedInterval != nullptr &&
                      std::ranges::any_of(MergedInterval->MainRange.values(),
                                          [](const rcc::LiveValueNumber &Value) {
                                            return Value.State == rcc::LiveValueState::Phi;
                                          });
  const bool HasRedefined =
      RedefinedInterval != nullptr &&
      std::ranges::any_of(RedefinedInterval->MainRange.values(),
                          [](const rcc::LiveValueNumber &Value) {
                            return Value.State == rcc::LiveValueState::Redefined;
                          });
  Context.expect(MergedInterval != nullptr && MergedInterval->MainRange.values().size() >= 3 &&
                     HasPhi && HasRedefined,
                 "non-SSA intervals must retain multiple definitions, PHI, and redefined values");
  Context.expect(AcrossInterval != nullptr && (*Intervals)->interferes(*Across, *Merged),
                 "live intervals must expose CFG-derived interference");
}

void testPhysicalUnitsAndRegMasks(TestContext &Context, const rcc::TargetDescription &Target) {
  const rcc::RegisterUnitID MultiLaneUnit = rcc::RegisterUnitID::fromValue(9);
  rcc::LiveRegUnits PartialLanes;
  PartialLanes.add(MultiLaneUnit, rcc::LaneMask::fromRaw(0x3));
  rcc::LiveRegUnits RemovedLanes;
  RemovedLanes.add(MultiLaneUnit, rcc::LaneMask::fromRaw(0x2));
  PartialLanes.subtract(RemovedLanes);
  Context.expect(PartialLanes.lanes(MultiLaneUnit) == rcc::LaneMask::fromRaw(0x1) &&
                     PartialLanes.contains(MultiLaneUnit, rcc::LaneMask::fromRaw(0x1)) &&
                     !PartialLanes.intersects(MultiLaneUnit, rcc::LaneMask::fromRaw(0x2)),
                 "RegisterUnit lane sets must preserve partial union and subtraction");

  auto AliasFixture = createFunction("physical-alias", 4, 1);
  if (!AliasFixture) {
    Context.expect(false, "physical-alias fixture creation must succeed");
    return;
  }
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  const rcc::Register G0High =
      rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0High32));
  auto G0Def = buildDef(*AliasFixture->Function, Target, *AliasFixture->Blocks[0], G0);
  auto G0HighUse = buildUse(*AliasFixture->Function, Target, *AliasFixture->Blocks[0], G0High);
  if (!G0Def || !G0HighUse) {
    Context.expect(false, "physical-alias instructions must be built");
    return;
  }
  rcc::MachineFunctionAnalysisManager AliasAnalyses;
  auto AliasRegistered = rcc::registerStage11MachineAnalyses(AliasAnalyses);
  auto AliasIntervals =
      AliasRegistered
          ? AliasAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*AliasFixture->Function, Target)
          : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                std::unexpected(AliasRegistered.error())};
  const rcc::RegisterUnitID HighUnit = rcc::RegisterUnitID::fromValue(3);
  Context.expect(AliasIntervals && (*AliasIntervals)->regUnitRange(HighUnit) != nullptr &&
                     (*AliasIntervals)->regUnitRange(HighUnit)->segments().size() == 1,
                 "shared RegisterUnits must connect super-register defs to subregister uses");

  auto MaskFixture = createFunction("regmask", 5, 1);
  if (!MaskFixture) {
    Context.expect(false, "RegMask fixture creation must succeed");
    return;
  }
  auto Across = createVReg(*MaskFixture->Function);
  auto Output = createVReg(*MaskFixture->Function);
  if (!Across || !Output) {
    Context.expect(false, "RegMask virtual registers must be created");
    return;
  }
  const auto Mask = rcc::test::createTestCallPreservedMask();
  auto AcrossDef = buildDef(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Across);
  auto FirstMask = buildMask(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], Mask);
  auto AcrossUse = buildUse(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Across);
  auto OutputDef =
      buildMaskedDef(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Output, Mask);
  auto OutputUse = buildUse(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Output);
  if (!AcrossDef || !FirstMask || !AcrossUse || !OutputDef || !OutputUse) {
    Context.expect(false, "RegMask instructions must be built");
    return;
  }
  rcc::MachineFunctionAnalysisManager MaskAnalyses;
  auto MaskRegistered = rcc::registerStage11MachineAnalyses(MaskAnalyses);
  auto MaskIntervals =
      MaskRegistered
          ? MaskAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*MaskFixture->Function, Target)
          : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                std::unexpected(MaskRegistered.error())};
  if (!MaskIntervals) {
    Context.expect(false, "RegMask intervals must materialize");
    return;
  }
  const rcc::PhysicalRegister Candidate = physical(rcc::test::TestRegister::G0);
  const rcc::LiveInterval *AcrossInterval = (*MaskIntervals)->interval(*Across);
  const rcc::LiveInterval *OutputInterval = (*MaskIntervals)->interval(*Output);
  Context.expect((*MaskIntervals)->regMaskPositions().size() == 2 &&
                     std::ranges::all_of((*MaskIntervals)->regMaskPositions(),
                                         [](const rcc::RegMaskPosition &Position) {
                                           return Position.Position.kind() ==
                                                  rcc::SlotIndexKind::EarlyClobber;
                                         }),
                 "RegMask interference positions must use early-clobber slots");
  Context.expect(AcrossInterval != nullptr && OutputInterval != nullptr &&
                     (*MaskIntervals)->regMaskClobbers(*AcrossInterval, Candidate) &&
                     !(*MaskIntervals)->regMaskClobbers(*OutputInterval, Candidate),
                 "RegMask queries must reject live-through values but accept same-call outputs");
  Context.expect((*MaskIntervals)->regUnitRange(rcc::RegisterUnitID::fromValue(1)) == nullptr,
                 "unused RegMask clobbers must not expand into physical live ranges");
}

void testRISCVLiveness(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFunction("riscv-liveness", 8, 1);
  if (!Target || !Fixture) {
    Context.expect(false, "RV64GC liveness fixture creation must succeed");
    return;
  }
  auto X5 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X5);
  auto X6 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X6);
  auto X7 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X7);
  if (!X5 || !X6 || !X7) {
    Context.expect(false, "RV64GC physical registers must resolve");
    return;
  }
  auto BuildADDI = [&](rcc::PhysicalRegister Destination, rcc::PhysicalRegister Source,
                       std::int64_t Immediate) -> rcc::Expected<rcc::MachineInstr *> {
    rcc::MachineIRBuilder Builder(*Fixture->Function, *Target);
    auto Point = Builder.setInsertPoint(*Fixture->Blocks[0]);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(
        rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADDI)));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto Def = Pending->addDef(rcc::Register::fromPhysical(Destination));
    if (!Def)
      return std::unexpected(Def.error());
    auto Use = Pending->addUse(rcc::Register::fromPhysical(Source));
    if (!Use)
      return std::unexpected(Use.error());
    auto Imm = Pending->addImmediate(Immediate);
    if (!Imm)
      return std::unexpected(Imm.error());
    return Pending->build();
  };
  auto First = BuildADDI(*X6, *X5, 1);
  auto Second = BuildADDI(*X7, *X6, 2);
  if (!First || !Second) {
    Context.expect(false, "RV64GC ADDI instructions must be built");
    return;
  }
  rcc::MachineFunctionAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Intervals = Registered
                       ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, *Target)
                       : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                             std::unexpected(Registered.error())};
  const auto Unit =
      rcc::RegisterUnitID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterUnit::U_X6));
  Context.expect(Intervals && (*Intervals)->regUnitRange(Unit) != nullptr &&
                     (*Intervals)->regUnitRange(Unit)->segments().size() == 1,
                 "production RV64GC register units must participate in LiveIntervals");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();
  testCFGAndValueNumbers(Context, *Target);
  testPhysicalUnitsAndRegMasks(Context, *Target);
  testRISCVLiveness(Context);
  return Context.result();
}
