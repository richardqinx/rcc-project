// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LiveVariables.h"

#include "LiveTestSupport.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

namespace {

using namespace rcc::test::liveness;

void testLivenessRebuildAndVerifier(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("rebuild", 6, 2);
  if (!Fixture) {
    Context.expect(false, "liveness-rebuild fixture creation must succeed");
    return;
  }
  auto &Function = *Fixture->Function;
  auto &Entry = *Fixture->Blocks[0];
  auto &Exit = *Fixture->Blocks[1];
  auto Edge = Function.cfgUpdater().addSuccessor(Entry, Exit, rcc::BranchProbability::one());
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  const rcc::Register G1 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G1));
  auto LiveDef = buildDef(Function, Target, Entry, G0);
  auto FirstUse = buildUse(Function, Target, Exit, G0);
  auto LastUse = buildUse(Function, Target, Exit, G0);
  auto DeadDef = buildDef(Function, Target, Exit, G1);
  if (!Edge || !LiveDef || !FirstUse || !LastUse || !DeadDef) {
    Context.expect(false, "liveness-rebuild MachineIR must be constructed");
    return;
  }
  auto StaleLiveIn = Entry.addLiveIn(physical(rcc::test::TestRegister::F0));
  Context.expect(StaleLiveIn.has_value(), "stale live-in setup must succeed");
  setRebuildProperties(Function);

  rcc::MachineFunctionPassManager Pipeline;
  auto Added = Pipeline.addPass(std::make_unique<rcc::RebuildMachineLivenessPass>());
  rcc::MachineFunctionAnalysisManager Analyses;
  auto Run = Added ? Pipeline.run(Function, Target, Analyses)
                   : rcc::Expected<void>{std::unexpected(Added.error())};
  if (!Run)
    std::cerr << "liveness rebuild pipeline error: " << Run.error().message() << '\n';
  Context.expect(Run.has_value() &&
                     Function.properties().has(rcc::MachineFunctionProperty::TracksLiveness),
                 "RebuildMachineLiveness must publish TracksLiveness through the pass manager");
  if (!Run)
    return;
  Context.expect(
      Entry.liveIns().empty() && Exit.liveIns().size() == 1 &&
          Exit.liveIns().front().PhysReg == physical(rcc::test::TestRegister::G0) &&
          Exit.liveIns().front().Lanes == rcc::LaneMask::all(),
      "liveness rebuild must replace stale live-ins with canonical whole-register input");
  Context.expect(!hasFlag(**LiveDef, 0, rcc::RegisterOperandFlag::Dead) &&
                     !hasFlag(**FirstUse, 0, rcc::RegisterOperandFlag::Kill) &&
                     hasFlag(**LastUse, 0, rcc::RegisterOperandFlag::Kill) &&
                     hasFlag(**DeadDef, 0, rcc::RegisterOperandFlag::Dead),
                 "reverse reconstruction must derive conservative kill and dead flags");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "rebuilt tracked liveness must pass the MachineVerifier");

  auto ConservativeLiveIn = Entry.addLiveIn(physical(rcc::test::TestRegister::F0));
  Context.expect(ConservativeLiveIn.has_value() &&
                     rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "MachineVerifier must accept conservative physical live-ins");
  Entry.clearLiveIns();

  rcc::MachineOperand *FirstUseOperand = (*FirstUse)->operandAt(0);
  if (FirstUseOperand == nullptr) {
    Context.expect(false, "first use operand must exist");
    return;
  }
  rcc::RegisterOperandFlags StaleFlags = FirstUseOperand->registerFlags();
  StaleFlags.set(rcc::RegisterOperandFlag::Kill);
  auto StaleKill = FirstUseOperand->setRegisterFlags(StaleFlags);
  Context.expect(StaleKill.has_value() && !rcc::MachineVerifier::verify(Function, Target),
                 "MachineVerifier must reject an overstrong tracked kill flag");
  StaleFlags.clear(rcc::RegisterOperandFlag::Kill);
  auto ClearedKill = FirstUseOperand->setRegisterFlags(StaleFlags);
  Context.expect(ClearedKill.has_value(), "stale kill flag cleanup must succeed");
  Exit.clearLiveIns();
  Context.expect(!rcc::MachineVerifier::verify(Function, Target),
                 "MachineVerifier must reject stale tracked physical live-ins");

  rcc::RebuildMachineLivenessPass Rebuild;
  auto Repaired = Rebuild.run(Function, Target, Analyses);
  Context.expect(Repaired && rcc::MachineVerifier::verify(Function, Target),
                 "a second liveness rebuild must repair stale derived state");
}

void testTrackedLiveThroughPhysicalRegister(TestContext &Context,
                                            const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("tracked-live-through", 11, 2);
  if (!Fixture) {
    Context.expect(false, "tracked live-through fixture creation must succeed");
    return;
  }
  auto &Function = *Fixture->Function;
  auto &Predecessor = *Fixture->Blocks[0];
  auto &Successor = *Fixture->Blocks[1];
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  auto Edge =
      Function.cfgUpdater().addSuccessor(Predecessor, Successor, rcc::BranchProbability::one());
  auto Use = buildUse(Function, Target, Successor, G0);
  auto LiveIn = Successor.addLiveIn(physical(rcc::test::TestRegister::G0));
  if (!Edge || !Use || !LiveIn) {
    Context.expect(false, "tracked live-through MachineIR must be constructed");
    return;
  }

  Function.properties().set(rcc::MachineFunctionProperty::TracksLiveness);
  Context.expect(rcc::verifyTrackedMachineLiveness(Function, Target).has_value() &&
                     Predecessor.liveIns().empty(),
                 "a predecessor must not declare a physical register used only by its successor");
  Successor.clearLiveIns();
  Context.expect(!rcc::verifyTrackedMachineLiveness(Function, Target),
                 "a block must declare a physical register that it reads before defining");
}

void testFailedRebuildIsAtomic(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("failed-rebuild", 9, 2);
  if (!Fixture) {
    Context.expect(false, "failed-rebuild fixture creation must succeed");
    return;
  }
  auto Destination = createVReg(*Fixture->Function);
  auto Source = createVReg(*Fixture->Function);
  if (!Destination || !Source) {
    Context.expect(false, "failed-rebuild virtual registers must be created");
    return;
  }

  auto &Predecessor = *Fixture->Blocks[0];
  auto &PhiBlock = *Fixture->Blocks[1];
  const auto LiveIn =
      PhiBlock.addLiveIn(physical(rcc::test::TestRegister::G0), rcc::LaneMask::fromRaw(0x1));
  rcc::RegisterOperandFlags KillFlags;
  KillFlags.set(rcc::RegisterOperandFlag::Kill);
  rcc::MachineIRBuilder Builder(*Fixture->Function, Target);
  auto Point = Builder.setInsertPoint(PhiBlock);
  auto Pending = Point ? Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI))
                       : rcc::Expected<rcc::MachineInstrBuilder>{std::unexpected(Point.error())};
  auto Def = Pending ? Pending->addDef(*Destination)
                     : rcc::Expected<void>{std::unexpected(Pending.error())};
  auto Use =
      Def ? Pending->addUse(*Source, KillFlags) : rcc::Expected<void>{std::unexpected(Def.error())};
  auto Incoming =
      Use ? Pending->addBasicBlock(Predecessor) : rcc::Expected<void>{std::unexpected(Use.error())};
  auto Phi = Incoming ? Pending->build()
                      : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Incoming.error())};
  if (!LiveIn || !Phi) {
    Context.expect(false, "malformed-edge PHI setup must succeed");
    return;
  }

  const std::vector<rcc::MachineBasicBlockLiveIn> OriginalLiveIns(PhiBlock.liveIns().begin(),
                                                                  PhiBlock.liveIns().end());
  const rcc::RegisterOperandFlags OriginalFlags = (*Phi)->operandAt(1)->registerFlags();
  auto Rebuilt = rcc::rebuildMachineLiveness(*Fixture->Function, Target);
  Context.expect(!Rebuilt && Rebuilt.error().code() == rcc::ErrorCode::VerificationError &&
                     std::ranges::equal(PhiBlock.liveIns(), OriginalLiveIns) &&
                     (*Phi)->operandAt(1)->registerFlags() == OriginalFlags,
                 "malformed PHI edges must fail without changing live-ins or operand flags");
}

class DummyAnalysis final {
public:
  struct Result final {};

  [[nodiscard]] static rcc::Expected<Result> run(rcc::MachineFunction &,
                                                 const rcc::TargetDescription &,
                                                 rcc::MachineFunctionAnalysisManager &) {
    return Result{};
  }
};

void testAnalysisRegistrationAndInvalidation(TestContext &Context,
                                             const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("analysis-invalidation", 7, 1);
  if (!Fixture) {
    Context.expect(false, "analysis-invalidation fixture creation must succeed");
    return;
  }
  auto Reg = createVReg(*Fixture->Function);
  auto Def = Reg ? buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg)
                 : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Reg.error())};
  auto Use = Reg ? buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg)
                 : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Reg.error())};
  if (!Reg || !Def || !Use) {
    Context.expect(false, "analysis-invalidation instructions must be built");
    return;
  }

  rcc::MachineFunctionAnalysisManager Analyses;
  auto FirstRegistration = rcc::registerStage11MachineAnalyses(Analyses);
  auto SecondRegistration = rcc::registerStage11MachineAnalyses(Analyses);
  auto Intervals = FirstRegistration
                       ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target)
                       : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                             std::unexpected(FirstRegistration.error())};
  Context.expect(FirstRegistration && SecondRegistration && Intervals &&
                     Analyses.cachedResultCount(*Fixture->Function) == 3,
                 "Stage 11 analyses must register idempotently and materialize dependencies");
  Analyses.invalidate(*Fixture->Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::Frame});
  Context.expect(Analyses.cachedResultCount(*Fixture->Function) == 3,
                 "debug-only changes must preserve all liveness analyses");
  Analyses.invalidate(*Fixture->Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::Operands});
  Context.expect(
      Analyses.cachedResultCount(*Fixture->Function) == 1 &&
          Analyses.hasCached(*Fixture->Function,
                             rcc::MachineFunctionAnalysisManager::id<rcc::SlotIndexesAnalysis>()),
      "operand changes must retain SlotIndexes while invalidating liveness dependents");
  auto Recomputed = Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target);
  Context.expect(Recomputed && Analyses.cachedResultCount(*Fixture->Function) == 3,
                 "invalidated liveness dependencies must recompute on demand");
  Analyses.invalidate(*Fixture->Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::BlockLayout});
  Context.expect(
      Analyses.cachedResultCount(*Fixture->Function) == 1 &&
          Analyses.hasCached(*Fixture->Function,
                             rcc::MachineFunctionAnalysisManager::id<rcc::LiveVariablesAnalysis>()),
      "layout changes must invalidate SlotIndexes and LiveIntervals only");

  rcc::MachineFunctionAnalysisManager Atomic;
  rcc::MachineAnalysisRegistration DummyRegistration;
  DummyRegistration.Name = "SlotIndexes";
  auto Dummy = Atomic.registerAnalysis<DummyAnalysis>(std::move(DummyRegistration));
  auto Rejected = rcc::registerStage11MachineAnalyses(Atomic);
  Context.expect(
      Dummy && !Rejected &&
          Atomic.isRegistered(rcc::MachineFunctionAnalysisManager::id<DummyAnalysis>()) &&
          !Atomic.isRegistered(
              rcc::MachineFunctionAnalysisManager::id<rcc::SlotIndexesAnalysis>()) &&
          !Atomic.isRegistered(
              rcc::MachineFunctionAnalysisManager::id<rcc::LiveVariablesAnalysis>()),
      "failed standard-analysis registration must leave the manager unchanged");
}

void testRISCVReservedRegisterLiveness(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFunction("riscv-reserved-liveness", 10, 1);
  if (!Target || !Fixture) {
    Context.expect(false, "RV64GC reserved-register liveness fixture creation must succeed");
    return;
  }
  auto X0 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X0);
  auto X5 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X5);
  if (!X0 || !X5) {
    Context.expect(false, "RV64GC reserved-register fixture registers must resolve");
    return;
  }

  rcc::MachineIRBuilder Builder(*Fixture->Function, *Target);
  auto Point = Builder.setInsertPoint(*Fixture->Blocks[0]);
  auto Pending = Point ? Builder.buildInstr(rcc::Opcode::fromValue(
                             static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADDI)))
                       : rcc::Expected<rcc::MachineInstrBuilder>{std::unexpected(Point.error())};
  auto Def = Pending ? Pending->addDef(rcc::Register::fromPhysical(*X5))
                     : rcc::Expected<void>{std::unexpected(Pending.error())};
  auto Use = Def ? Pending->addUse(rcc::Register::fromPhysical(*X0))
                 : rcc::Expected<void>{std::unexpected(Def.error())};
  auto Immediate =
      Use ? Pending->addImmediate(1) : rcc::Expected<void>{std::unexpected(Use.error())};
  auto Instruction = Immediate
                         ? Pending->build()
                         : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Immediate.error())};
  auto DeadDef =
      Instruction ? Builder.buildInstr(rcc::Opcode::fromValue(
                        static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADDI)))
                  : rcc::Expected<rcc::MachineInstrBuilder>{std::unexpected(Instruction.error())};
  auto DeadDefDestination = DeadDef ? DeadDef->addDef(rcc::Register::fromPhysical(*X0))
                                    : rcc::Expected<void>{std::unexpected(DeadDef.error())};
  auto DeadDefSource = DeadDefDestination
                           ? DeadDef->addUse(rcc::Register::fromPhysical(*X0))
                           : rcc::Expected<void>{std::unexpected(DeadDefDestination.error())};
  auto DeadDefImmediate = DeadDefSource
                              ? DeadDef->addImmediate(0)
                              : rcc::Expected<void>{std::unexpected(DeadDefSource.error())};
  auto DeadDefInstruction =
      DeadDefImmediate
          ? DeadDef->build()
          : rcc::Expected<rcc::MachineInstr *>{std::unexpected(DeadDefImmediate.error())};
  if (!Instruction || !DeadDefInstruction) {
    Context.expect(false, "RV64GC reserved-register liveness instruction must be built");
    return;
  }

  auto Rebuilt = rcc::rebuildMachineLiveness(*Fixture->Function, *Target);
  const rcc::MachineOperand *ZeroUse = (*Instruction)->operandAt(1);
  const rcc::MachineOperand *ZeroDef = (*DeadDefInstruction)->operandAt(0);
  const rcc::MachineOperand *DeadDefZeroUse = (*DeadDefInstruction)->operandAt(1);
  Context.expect(Rebuilt && Fixture->Blocks[0]->liveIns().empty() && ZeroUse != nullptr &&
                     !ZeroUse->registerFlags().has(rcc::RegisterOperandFlag::Kill) &&
                     ZeroDef != nullptr &&
                     ZeroDef->registerFlags().has(rcc::RegisterOperandFlag::Dead) &&
                     DeadDefZeroUse != nullptr &&
                     !DeadDefZeroUse->registerFlags().has(rcc::RegisterOperandFlag::Kill),
                 "reserved uses must remain available while dead defs retain precise liveness");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();

  testLivenessRebuildAndVerifier(Context, *Target);
  testTrackedLiveThroughPhysicalRegister(Context, *Target);
  testFailedRebuildIsAtomic(Context, *Target);
  testAnalysisRegistrationAndInvalidation(Context, *Target);
  testRISCVReservedRegisterLiveness(Context);
  return Context.result();
}
