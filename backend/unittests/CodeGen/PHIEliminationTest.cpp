// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/PHIElimination.h"

#include "SSAExitTestSupport.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachineVerifier.h"

#include <array>
#include <memory>
#include <utility>

namespace {

using namespace rcc::test::ssa;

[[nodiscard]] rcc::Expected<rcc::MachineInstr *> buildJump(rcc::MachineFunction &Function,
                                                           const rcc::TargetDescription &Target,
                                                           rcc::MachineBasicBlock &Block,
                                                           rcc::MachineBasicBlock &Destination) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(testOpcode(JumpOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto TargetOperand = Pending->addBasicBlock(Destination);
  if (!TargetOperand)
    return std::unexpected(TargetOperand.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildPhi(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Destination,
         std::span<const std::pair<rcc::Register, rcc::MachineBasicBlock *>> Incoming,
         rcc::RegisterOperandFlags DestinationFlags = {}, std::uint32_t DestinationTargetFlags = 0,
         rcc::RegisterOperandFlags IncomingFlags = {}, std::uint32_t IncomingTargetFlags = 0) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination, DestinationFlags, {}, DestinationTargetFlags);
  if (!Def)
    return std::unexpected(Def.error());
  for (const auto &[Value, Predecessor] : Incoming) {
    auto Use = Pending->addUse(Value, IncomingFlags, {}, IncomingTargetFlags);
    if (!Use)
      return std::unexpected(Use.error());
    auto IncomingBlock = Pending->addBasicBlock(*Predecessor);
    if (!IncomingBlock)
      return std::unexpected(IncomingBlock.error());
  }
  return Pending->build();
}

[[nodiscard]] rcc::Expected<void> runPHIElimination(rcc::MachineFunction &Function,
                                                    const rcc::TargetDescription &Target,
                                                    const rcc::TargetInstrInfo &Info) {
  rcc::MachinePassManager Pipeline;
  auto Added = Pipeline.addPass(std::make_unique<rcc::PHIEliminationPass>(Info));
  if (!Added)
    return std::unexpected(Added.error());
  rcc::MachineAnalysisManager Analyses;
  return Pipeline.run(Function, Target, Analyses);
}

[[nodiscard]] std::vector<rcc::MachineInstr *> linkedInstructions(rcc::MachineBasicBlock &Block) {
  std::vector<rcc::MachineInstr *> Result;
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    Result.push_back(Instruction);
  }
  return Result;
}

[[nodiscard]] bool hasLiveIn(const rcc::MachineBasicBlock &Block, rcc::PhysicalRegister Reg) {
  return std::ranges::any_of(Block.liveIns(), [Reg](const rcc::MachineBasicBlockLiveIn &LiveIn) {
    return LiveIn.PhysReg == Reg;
  });
}

void testParallelPHIsAndPlacement(TestContext &Context, const rcc::TargetDescription &Target,
                                  const TestInstrInfo &Info) {
  auto Fixture = createFunction(Context, "parallel_phis", 1, 3);
  if (!Fixture.Function || Fixture.Blocks.size() != 3)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  if (!I64)
    return;
  auto &Function = *Fixture.Function;
  rcc::MachineBasicBlock &Preheader = *Fixture.Blocks[0];
  rcc::MachineBasicBlock &Header = *Fixture.Blocks[1];
  rcc::MachineBasicBlock &Backedge = *Fixture.Blocks[2];
  const rcc::PhysicalRegister Hint = *rcc::PhysicalRegister::create(rcc::test::TestRegister::G1);

  auto InitA = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                          rcc::test::TestRegisterClass::GPR64);
  auto InitB = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                          rcc::test::TestRegisterClass::GPR64);
  auto DestinationA = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                                 rcc::test::TestRegisterClass::GPR64, 0x1, Hint);
  auto DestinationB = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                                 rcc::test::TestRegisterClass::GPR64);
  Context.expect(InitA && InitB && DestinationA && DestinationB,
                 "parallel PHI registers must be created");
  if (!InitA || !InitB || !DestinationA || !DestinationB)
    return;

  auto CFG = Function.cfgUpdater();
  Context.expect(
      CFG.addSuccessor(Preheader, Header, rcc::BranchProbability::one()).has_value() &&
          CFG.addSuccessor(Header, Backedge, rcc::BranchProbability::one()).has_value() &&
          CFG.addSuccessor(Backedge, Header, rcc::BranchProbability::one()).has_value(),
      "parallel PHI loop CFG must be connected");

  const std::array InitADef{*InitA};
  const std::array InitBDef{*InitB};
  Context.expect(buildRegisterInstruction(Function, Target, Preheader, testOpcode(DefOpcode),
                                          InitADef, {}, {rcc::RegisterOperandFlag::Dead}) &&
                     buildRegisterInstruction(Function, Target, Preheader, testOpcode(DefOpcode),
                                              InitBDef, {}) &&
                     buildJump(Function, Target, Preheader, Header),
                 "parallel PHI preheader must be populated");

  const std::array IncomingA{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*InitA, &Preheader},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*DestinationB, &Backedge}};
  const std::array IncomingB{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*InitB, &Preheader},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*DestinationA, &Backedge}};
  Context.expect(buildPhi(Function, Target, Header, *DestinationA, IncomingA, {}, 0x4, {}, 0x8) &&
                     buildPhi(Function, Target, Header, *DestinationB, IncomingB),
                 "parallel PHIs must be built");

  rcc::MachineIRBuilder HeaderBuilder(Function, Target);
  if (!HeaderBuilder.setInsertPoint(Header))
    return;
  auto Meta = HeaderBuilder.buildInstr(testOpcode(MetaOpcode));
  const std::array HeaderUse{*DestinationA};
  Context.expect(Meta && Meta->build() &&
                     buildRegisterInstruction(Function, Target, Header, testOpcode(UseOpcode), {},
                                              HeaderUse) &&
                     buildJump(Function, Target, Header, Backedge),
                 "parallel PHI header must contain meta, code, and terminator instructions");

  const std::array KilledUse{*DestinationA};
  Context.expect(buildRegisterInstruction(Function, Target, Backedge, testOpcode(UseOpcode), {},
                                          KilledUse, {}, {rcc::RegisterOperandFlag::Kill}) &&
                     buildJump(Function, Target, Backedge, Header),
                 "parallel PHI backedge must be populated");
  auto InitialLiveness = setSelectedSSAProperties(Function, Target);
  Context.expect(InitialLiveness.has_value(), "parallel PHI liveness rebuild must succeed");
  if (!InitialLiveness)
    return;

  auto Run = runPHIElimination(Function, Target, Info);
  Context.expect(Run.has_value(), "parallel PHI elimination must succeed");
  if (!Run)
    return;
  Context.expect(!Function.properties().has(rcc::MachineFunctionProperty::IsSSA) &&
                     Function.properties().has(rcc::MachineFunctionProperty::NoPHIs) &&
                     Function.properties().has(rcc::MachineFunctionProperty::TracksLiveness),
                 "PHI elimination must publish its exact property transition");
  Context.expect(instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::PHI)).empty(),
                 "PHI elimination must remove every PHI");

  const auto HeaderInstructions = linkedInstructions(Header);
  Context.expect(HeaderInstructions.size() == 5 &&
                     HeaderInstructions[0]->opcode() == testOpcode(MetaOpcode) &&
                     HeaderInstructions[1]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     HeaderInstructions[2]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     HeaderInstructions[3]->opcode() == testOpcode(UseOpcode) &&
                     HeaderInstructions[4]->opcode() == testOpcode(JumpOpcode),
                 "destination COPYs must follow the full leading PHI/meta region");
  if (HeaderInstructions.size() < 3)
    return;

  const rcc::Register *FreshA = HeaderInstructions[1]->operandAt(1)->getIf<rcc::Register>();
  const rcc::Register *FreshB = HeaderInstructions[2]->operandAt(1)->getIf<rcc::Register>();
  Context.expect(FreshA != nullptr && FreshB != nullptr && *FreshA != *FreshB &&
                     *FreshA != *DestinationA && *FreshB != *DestinationB,
                 "each PHI must receive an independent fresh incoming vreg");
  if (FreshA == nullptr || FreshB == nullptr)
    return;

  const auto BackedgeInstructions = linkedInstructions(Backedge);
  Context.expect(BackedgeInstructions.size() == 4 &&
                     BackedgeInstructions[1]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     BackedgeInstructions[2]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     BackedgeInstructions[3]->opcode() == testOpcode(JumpOpcode),
                 "incoming COPYs must be placed together before the first terminator");
  if (BackedgeInstructions.size() >= 3) {
    const auto FirstDef = BackedgeInstructions[1]->operandAt(0)->registerValue();
    const auto FirstUse = BackedgeInstructions[1]->operandAt(1)->registerValue();
    const auto SecondDef = BackedgeInstructions[2]->operandAt(0)->registerValue();
    const auto SecondUse = BackedgeInstructions[2]->operandAt(1)->registerValue();
    Context.expect(FirstDef == *FreshA && FirstUse == *DestinationB && SecondDef == *FreshB &&
                       SecondUse == *DestinationA,
                   "backedge COPYs must read old PHI destinations into distinct temporaries");
  }

  auto FreshInfo = Function.registerInfo().virtualRegisterInfo(*FreshA);
  Context.expect(FreshInfo.has_value(), "fresh PHI vreg must remain registered");
  if (!FreshInfo)
    return;
  Context.expect(FreshInfo->get().Type == I64, "fresh PHI vreg must clone its LLT");
  Context.expect(FreshInfo->get().Bank == rcc::test::TestRegisterBank::GPR,
                 "fresh PHI vreg must clone its register bank");
  Context.expect(FreshInfo->get().RegClass == rcc::test::TestRegisterClass::GPR64,
                 "fresh PHI vreg must clone its register class");
  Context.expect(FreshInfo->get().TargetFlags == 0x1,
                 "fresh PHI vreg must clone registered target flags");
  Context.expect(FreshInfo->get().AllocationHints.size() == 1,
                 "fresh PHI vreg must clone the allocation-hint count");
  Context.expect(!FreshInfo->get().AllocationHints.empty() &&
                     FreshInfo->get().AllocationHints.front() == Hint,
                 "fresh PHI vreg must clone the allocation-hint register");
  Context.expect(HeaderInstructions[1]->operandAt(0)->targetFlags() == 0x4 &&
                     HeaderInstructions[1]->operandAt(1)->targetFlags() == 0x4,
                 "successor COPY must preserve registered destination target flags");

  const rcc::MachineInstr *InitDefinition = Preheader.firstInstruction();
  Context.expect(
      InitDefinition != nullptr &&
          !InitDefinition->operandAt(0)->registerFlags().has(rcc::RegisterOperandFlag::Dead) &&
          !BackedgeInstructions[0]->operandAt(0)->registerFlags().has(
              rcc::RegisterOperandFlag::Kill),
      "liveness-preserving PHI elimination must conservatively clear kill/dead flags");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "parallel PHI result must pass MachineVerifier");
}

void testCriticalEdgeAndPhysicalLiveIn(TestContext &Context, const rcc::TargetDescription &Target,
                                       const TestInstrInfo &Info) {
  auto Fixture = createFunction(Context, "critical_edge", 2, 4);
  if (!Fixture.Function || Fixture.Blocks.size() != 4)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  if (!I64)
    return;
  auto &Function = *Fixture.Function;
  rcc::MachineBasicBlock &From = *Fixture.Blocks[0];
  rcc::MachineBasicBlock &Side = *Fixture.Blocks[1];
  rcc::MachineBasicBlock &Other = *Fixture.Blocks[2];
  rcc::MachineBasicBlock &Merge = *Fixture.Blocks[3];
  auto Condition = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                              rcc::test::TestRegisterClass::GPR64);
  auto OtherValue = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                               rcc::test::TestRegisterClass::GPR64);
  auto Destination = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                                rcc::test::TestRegisterClass::GPR64);
  if (!Condition || !OtherValue || !Destination)
    return;

  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function.cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(From, Side, *Half) &&
                     CFG.addSuccessor(From, Merge, *Half) &&
                     CFG.addSuccessor(Other, Merge, rcc::BranchProbability::one()),
                 "critical-edge CFG must be connected");

  const std::array ConditionDef{*Condition};
  Context.expect(
      buildRegisterInstruction(Function, Target, From, testOpcode(DefOpcode), ConditionDef, {})
          .has_value(),
      "critical-edge condition must be defined");
  rcc::MachineIRBuilder FromBuilder(Function, Target);
  if (!FromBuilder.setInsertPoint(From))
    return;
  auto Branch = FromBuilder.buildInstr(testOpcode(ConditionalBranchOpcode));
  Context.expect(Branch && Branch->addUse(*Condition) && Branch->addBasicBlock(Merge) &&
                     Branch->build(),
                 "critical-edge conditional branch must be built");
  const std::array OtherDef{*OtherValue};
  Context.expect(
      buildRegisterInstruction(Function, Target, Other, testOpcode(DefOpcode), OtherDef, {})
          .has_value(),
      "unrelated fallthrough predecessor must define its incoming value");

  const rcc::PhysicalRegister G0 = *rcc::PhysicalRegister::create(rcc::test::TestRegister::G0);
  const rcc::PhysicalRegister G1 = *rcc::PhysicalRegister::create(rcc::test::TestRegister::G1);
  Context.expect(Merge.addLiveIn(G1, rcc::LaneMask::fromRaw(0xF)).has_value(),
                 "merge block live-in setup must succeed");
  const std::array Incoming{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{rcc::Register::fromPhysical(G0), &From},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*OtherValue, &Other}};
  Context.expect(buildPhi(Function, Target, Merge, *Destination, Incoming).has_value(),
                 "critical-edge PHI must be built");
  const std::array G1Use{rcc::Register::fromPhysical(G1)};
  Context.expect(buildRegisterInstruction(Function, Target, Merge, testOpcode(UseOpcode), {}, G1Use)
                     .has_value(),
                 "merge block must consume its inherited physical live-in");
  auto InitialLiveness = setSelectedSSAProperties(Function, Target);
  Context.expect(InitialLiveness.has_value(), "critical-edge PHI liveness rebuild must succeed");
  if (!InitialLiveness)
    return;

  auto Run = runPHIElimination(Function, Target, Info);
  Context.expect(Run.has_value(), "critical-edge PHI elimination must succeed");
  if (!Run)
    return;

  rcc::MachineBasicBlock *Edge = Merge.previous();
  Context.expect(
      Edge != nullptr && Edge != &Other && Edge->previous() == &Other &&
          From.successors().size() == 2 &&
          std::ranges::any_of(From.successors(),
                              [Edge](const rcc::MachineSuccessor &S) { return S.Block == Edge; }) &&
          Edge->predecessors().size() == 1 && Edge->predecessors()[0] == &From &&
          Edge->successors().size() == 1 && Edge->successors()[0].Block == &Merge,
      "critical edge must become a reciprocal From-to-edge-to-Merge CFG path");
  if (Edge == nullptr || Edge == &Other)
    return;

  const rcc::MachineOperand *BranchTarget = From.lastInstruction()->operandAt(1);
  const auto *BranchBlock =
      BranchTarget == nullptr ? nullptr : BranchTarget->getIf<rcc::BasicBlockOperand>();
  Context.expect(BranchBlock != nullptr && BranchBlock->Block == Edge,
                 "explicit critical-edge branch must target the split block");
  const auto OtherInstructions = linkedInstructions(Other);
  const auto EdgeInstructions = linkedInstructions(*Edge);
  Context.expect(OtherInstructions.size() == 3 &&
                     OtherInstructions.back()->opcode() == testOpcode(JumpOpcode) &&
                     EdgeInstructions.size() == 2 &&
                     EdgeInstructions[0]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     EdgeInstructions[1]->opcode() == testOpcode(JumpOpcode),
                 "edge split must preserve unrelated fallthrough and terminator placement");
  Context.expect(hasLiveIn(*Edge, G0) && hasLiveIn(*Edge, G1),
                 "split edge must inherit successor live-ins and its physical PHI source");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "critical-edge result must pass MachineVerifier");
}

void testUndefAndFailureAtomicity(TestContext &Context, const rcc::TargetDescription &Target,
                                  const TestInstrInfo &Info) {
  auto BuildFixture = [&](std::uint64_t ID) {
    auto Fixture = createFunction(Context, "undef_phi", ID, 2);
    if (!Fixture.Function || Fixture.Blocks.size() != 2)
      return Fixture;
    auto I64 = rcc::LLT::makeInteger(64);
    if (!I64)
      return Fixture;
    auto Source = createVReg(*Fixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                             rcc::test::TestRegisterClass::GPR64);
    auto Destination = createVReg(*Fixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                                  rcc::test::TestRegisterClass::GPR64);
    if (!Source || !Destination)
      return Fixture;
    auto CFG = Fixture.Function->cfgUpdater();
    if (!CFG.addSuccessor(*Fixture.Blocks[0], *Fixture.Blocks[1], rcc::BranchProbability::one()) ||
        !buildJump(*Fixture.Function, Target, *Fixture.Blocks[0], *Fixture.Blocks[1])) {
      return Fixture;
    }
    const std::array Incoming{
        std::pair<rcc::Register, rcc::MachineBasicBlock *>{*Source, Fixture.Blocks[0]}};
    if (!buildPhi(*Fixture.Function, Target, *Fixture.Blocks[1], *Destination, Incoming, {}, 0,
                  {rcc::RegisterOperandFlag::Undef})) {
      return Fixture;
    }
    Context.expect(setSelectedSSAProperties(*Fixture.Function, Target).has_value(),
                   "undef PHI liveness rebuild must succeed");
    return Fixture;
  };

  auto Undef = BuildFixture(3);
  if (!Undef.Function)
    return;
  auto Run = runPHIElimination(*Undef.Function, Target, Info);
  Context.expect(Run.has_value(), "undef PHI elimination must succeed");
  if (Run) {
    const auto PredecessorInstructions = linkedInstructions(*Undef.Blocks[0]);
    Context.expect(PredecessorInstructions.size() == 2 &&
                       PredecessorInstructions[0]->opcode() ==
                           rcc::opcode(rcc::GenericOpcode::IMPLICIT_DEF) &&
                       PredecessorInstructions[1]->opcode() == testOpcode(JumpOpcode),
                   "undef PHI incoming values must become edge IMPLICIT_DEF instructions");
  }

  auto Atomic = BuildFixture(4);
  if (!Atomic.Function)
    return;
  const std::size_t BlocksBefore = Atomic.Function->blockCount();
  const std::size_t InstructionsBefore =
      instructionsWithOpcode(*Atomic.Function, rcc::opcode(rcc::GenericOpcode::PHI)).size();
  const std::size_t RegistersBefore = Atomic.Function->registerInfo().virtualRegisters().size();
  const std::uint64_t PropertiesBefore = Atomic.Function->properties().rawValue();
  TestInstrInfo RejectingInfo(true);
  rcc::MachineAnalysisManager Analyses;
  rcc::PHIEliminationPass FailingPass(RejectingInfo);
  auto Failed = FailingPass.run(*Atomic.Function, Target, Analyses);
  Context.expect(!Failed && Failed.error().code() == rcc::ErrorCode::Unsupported,
                 "COPY rejection must produce a structured PHI-elimination failure");
  Context.expect(
      Atomic.Function->blockCount() == BlocksBefore &&
          instructionsWithOpcode(*Atomic.Function, rcc::opcode(rcc::GenericOpcode::PHI)).size() ==
              InstructionsBefore &&
          Atomic.Function->registerInfo().virtualRegisters().size() == RegistersBefore &&
          Atomic.Function->properties().rawValue() == PropertiesBefore,
      "failed PHI transaction preflight must leave the original function unchanged");
}

void testMalformedPHIs(TestContext &Context, const rcc::TargetDescription &Target) {
  auto BuildBadPhi = [&](std::uint64_t ID, rcc::LLT DestinationType,
                         rcc::RegisterBankID DestinationBank, rcc::RegisterClassID DestinationClass,
                         rcc::LLT SourceType, rcc::RegisterBankID SourceBank,
                         rcc::RegisterClassID SourceClass, rcc::SubRegIndex SourceSubReg = {}) {
    auto Fixture = createFunction(Context, "bad_phi", ID, 2);
    if (!Fixture.Function || Fixture.Blocks.size() != 2)
      return Fixture;
    auto Destination =
        createVReg(*Fixture.Function, DestinationType, DestinationBank, DestinationClass);
    auto Source = createVReg(*Fixture.Function, SourceType, SourceBank, SourceClass);
    if (!Destination || !Source)
      return Fixture;
    auto CFG = Fixture.Function->cfgUpdater();
    if (!CFG.addSuccessor(*Fixture.Blocks[0], *Fixture.Blocks[1], rcc::BranchProbability::one())) {
      return Fixture;
    }
    const std::array SourceDef{*Source};
    if (!buildRegisterInstruction(*Fixture.Function, Target, *Fixture.Blocks[0],
                                  testOpcode(DefOpcode), SourceDef, {})) {
      return Fixture;
    }
    rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
    if (!Builder.setInsertPoint(*Fixture.Blocks[1]))
      return Fixture;
    auto Phi = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI));
    if (!Phi || !Phi->addDef(*Destination) || !Phi->addUse(*Source, {}, SourceSubReg) ||
        !Phi->addBasicBlock(*Fixture.Blocks[0]) || !Phi->build()) {
      return Fixture;
    }
    return Fixture;
  };

  auto I64 = rcc::LLT::makeInteger(64);
  auto I32 = rcc::LLT::makeInteger(32);
  if (!I64 || !I32)
    return;
  auto BadType =
      BuildBadPhi(6, *I64, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64,
                  *I32, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR32);
  Context.expect(BadType.Function && !rcc::MachineVerifier::verify(*BadType.Function, Target),
                 "MachineVerifier must reject incompatible PHI LLTs");

  auto BadClass =
      BuildBadPhi(7, *I64, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64,
                  *I64, rcc::test::TestRegisterBank::FPR, rcc::test::TestRegisterClass::FPR64);
  Context.expect(BadClass.Function && !rcc::MachineVerifier::verify(*BadClass.Function, Target),
                 "MachineVerifier must reject nonoverlapping PHI register classes");

  auto BadSubReg =
      BuildBadPhi(8, *I32, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR32Low,
                  *I64, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64,
                  rcc::test::TestSubReg::High32);
  Context.expect(BadSubReg.Function && !rcc::MachineVerifier::verify(*BadSubReg.Function, Target),
                 "MachineVerifier must reject incompatible PHI subregister projections");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "PHI-elimination TestTarget must be created");
  if (!Target)
    return Context.result();
  TestInstrInfo Info;
  testParallelPHIsAndPlacement(Context, *Target, Info);
  testCriticalEdgeAndPhysicalLiveIn(Context, *Target, Info);
  testUndefAndFailureAtomicity(Context, *Target, Info);
  testMalformedPHIs(Context, *Target);
  return Context.result();
}
