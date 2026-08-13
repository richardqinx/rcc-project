// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TwoAddressInstructionPass.h"

#include "SSAExitTestSupport.h"
#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/MachineVerifier.h"

#include <array>
#include <memory>
#include <optional>

namespace {

using namespace rcc::test::ssa;

void testTwoAddressAndSubregisterPseudos(TestContext &Context, const rcc::TargetDescription &Target,
                                         const TestInstrInfo &Info) {
  auto Fixture = createFunction(Context, "two_address", 5, 1);
  if (!Fixture.Function || Fixture.Blocks.empty())
    return;
  auto I64 = rcc::LowLevelType::makeCategoryNeutralScalar(64);
  auto I32 = rcc::LowLevelType::makeCategoryNeutralScalar(32);
  if (!I64 || !I32)
    return;
  auto &Function = *Fixture.Function;
  rcc::MachineBasicBlock &Block = *Fixture.Blocks[0];
  auto GPR64 = [&](std::uint8_t Flags = 0) {
    return createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                      rcc::test::TestRegisterClass::GPR64, Flags);
  };
  auto GPR32 = [&]() {
    return createVReg(Function, *I32, rcc::test::TestRegisterBank::GPR,
                      rcc::test::TestRegisterClass::GPR32);
  };

  auto Satisfied = GPR64();
  auto UndefSource = GPR64();
  auto UndefDestination = GPR64();
  auto CommuteSource = GPR64();
  auto CommuteDestination = GPR64();
  auto ConvertSource = GPR64();
  auto ConvertDestination = GPR64();
  auto FallbackSource = GPR64();
  auto FallbackDestination = GPR64();
  auto SequenceDestination = GPR64();
  auto SequenceLow = GPR32();
  auto SequenceHigh = GPR32();
  auto InsertDestination = GPR64();
  auto InsertBase = GPR64();
  auto Inserted = GPR32();
  const bool Complete = Satisfied && UndefSource && UndefDestination && CommuteSource &&
                        CommuteDestination && ConvertSource && ConvertDestination &&
                        FallbackSource && FallbackDestination && SequenceDestination &&
                        SequenceLow && SequenceHigh && InsertDestination && InsertBase && Inserted;
  Context.expect(Complete, "two-address test registers must be created");
  if (!Complete)
    return;

  for (rcc::Register Source : {*CommuteSource, *ConvertSource, *FallbackSource, *SequenceLow,
                               *SequenceHigh, *InsertBase, *Inserted}) {
    const std::array Def{Source};
    Context.expect(buildRegisterInstruction(Function, Target, Block, testOpcode(DefOpcode), Def, {},
                                            {rcc::RegisterOperandFlag::Dead})
                       .has_value(),
                   "two-address source definition must be built");
  }
  const std::array UndefDef{*UndefSource};
  Context.expect(buildRegisterInstruction(Function, Target, Block,
                                          rcc::opcode(rcc::GenericOpcode::IMPLICIT_DEF), UndefDef,
                                          {})
                     .has_value(),
                 "two-address undef source must be defined by IMPLICIT_DEF");

  auto BuildTied = [&](rcc::Opcode Opcode, rcc::Register Def, rcc::Register Use,
                       std::optional<rcc::Register> Other = std::nullopt) {
    rcc::MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPoint(Block);
    if (!Point)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(Point.error())};
    auto Pending = Builder.buildInstr(Opcode);
    if (!Pending)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(Pending.error())};
    auto AddedDef = Pending->addDef(Def);
    if (!AddedDef)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(AddedDef.error())};
    auto AddedUse = Pending->addUse(Use, {rcc::RegisterOperandFlag::Kill});
    if (!AddedUse)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(AddedUse.error())};
    if (Other) {
      auto AddedOther = Pending->addUse(*Other);
      if (!AddedOther)
        return rcc::Expected<rcc::MachineInstr *>{std::unexpected(AddedOther.error())};
    }
    return Pending->build();
  };

  auto AlreadyTied = BuildTied(testOpcode(TiedOpcode), *Satisfied, *Satisfied);
  auto UndefTied = BuildTied(testOpcode(TiedOpcode), *UndefDestination, *UndefSource);
  auto Commutable = BuildTied(testOpcode(CommutableTiedOpcode), *CommuteDestination, *CommuteSource,
                              *CommuteDestination);
  auto Convertible =
      BuildTied(testOpcode(ConvertibleTiedOpcode), *ConvertDestination, *ConvertSource);
  auto Fallback = BuildTied(testOpcode(TiedOpcode), *FallbackDestination, *FallbackSource);
  Context.expect(AlreadyTied && UndefTied && Commutable && Convertible && Fallback,
                 "two-address instruction fixtures must be built");
  if (!AlreadyTied || !UndefTied || !Commutable || !Convertible || !Fallback)
    return;

  rcc::MachineIRBuilder Builder(Function, Target);
  if (!Builder.setInsertPoint(Block))
    return;
  auto Sequence = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::REG_SEQUENCE));
  Context.expect(
      Sequence && Sequence->addDef(*SequenceDestination) && Sequence->addUse(*SequenceLow) &&
          Sequence->addImmediate(rcc::test::TestSubReg::Low32.value()) &&
          Sequence->addUse(*SequenceHigh) &&
          Sequence->addImmediate(rcc::test::TestSubReg::High32.value()) && Sequence->build(),
      "REG_SEQUENCE fixture must be built");
  auto Insert = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::INSERT_SUBREG));
  Context.expect(Insert && Insert->addDef(*InsertDestination) && Insert->addUse(*InsertBase) &&
                     Insert->addUse(*Inserted) &&
                     Insert->addImmediate(rcc::test::TestSubReg::Low32.value()) && Insert->build(),
                 "INSERT_SUBREG fixture must be built");
  auto Return = Builder.buildInstr(testOpcode(ReturnOpcode));
  Context.expect(Return && Return->build(), "two-address return must be built");

  auto InitialLiveness = setSelectedSSAProperties(Function, Target);
  Context.expect(InitialLiveness.has_value(), "two-address liveness rebuild must succeed");
  if (!InitialLiveness)
    return;
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  rcc::MachineFunctionPassManager Pipeline;
  Context.expect(
      Pipeline.addPass(std::make_unique<rcc::TwoAddressInstructionPass>(Info)).has_value(),
      "two-address pass must be registered");
  rcc::MachineFunctionAnalysisManager Analyses;
  auto Run = Pipeline.run(Function, Target, Analyses);
  Context.expect(Run.has_value(), "two-address rewrite must succeed");
  if (!Run)
    return;

  Context.expect(Function.properties().has(rcc::MachineFunctionProperty::TiedOpsRewritten) &&
                     !Function.properties().has(rcc::MachineFunctionProperty::IsSSA),
                 "two-address rewrite must publish TiedOpsRewritten and clear IsSSA");
  Context.expect(
      instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::REG_SEQUENCE)).empty() &&
          instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::INSERT_SUBREG)).empty(),
      "TwoAddressInstructionPass must consume all Stage 10 subregister pseudos");

  Context.expect((*AlreadyTied)->operandAt(1)->registerValue() == *Satisfied,
                 "an already-satisfied tie must remain unchanged");
  Context.expect(
      (*UndefTied)->operandAt(1)->registerValue() == *UndefDestination &&
          (*UndefTied)->operandAt(1)->registerFlags().has(rcc::RegisterOperandFlag::Undef),
      "undef tied uses must be rewritten without a COPY");
  Context.expect((*Commutable)->operandAt(1)->registerValue() == *CommuteDestination &&
                     (*Commutable)->operandAt(2)->registerValue() == *CommuteSource,
                 "commutation must satisfy a tie before COPY fallback");
  Context.expect((*Convertible)->opcode() == testOpcode(ThreeAddressOpcode) &&
                     !(*Convertible)->operandAt(0)->tiedOperandIndex() &&
                     !(*Convertible)->operandAt(1)->tiedOperandIndex(),
                 "target three-address conversion must run before COPY fallback");
  Context.expect((*Fallback)->previous() != nullptr &&
                     (*Fallback)->previous()->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     (*Fallback)->previous()->operandAt(0)->registerValue() ==
                         *FallbackDestination &&
                     (*Fallback)->previous()->operandAt(1)->registerValue() == *FallbackSource &&
                     (*Fallback)->operandAt(1)->registerValue() == *FallbackDestination,
                 "unsatisfied ties must receive a COPY immediately before the instruction");

  const auto Copies = instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::COPY));
  const auto ImplicitDefs =
      instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::IMPLICIT_DEF));
  Context.expect(Copies.size() >= 5 && ImplicitDefs.size() >= 2,
                 "subregister pseudo expansion must materialize base and lane definitions");
  Context.expect(
      std::ranges::any_of(Copies,
                          [InsertDestination](const rcc::MachineInstr *Copy) {
                            return Copy->operandAt(0)->registerValue() == *InsertDestination &&
                                   Copy->operandAt(0)->subReg() == rcc::test::TestSubReg::Low32;
                          }),
      "INSERT_SUBREG must copy its inserted value into the requested destination lane");
  const bool HasKill = std::ranges::any_of(Function.allInstructions(), [](const auto *MI) {
    return MI->parent() != nullptr &&
           std::ranges::any_of(MI->operands(), [](const rcc::MachineOperand &Operand) {
             return Operand.registerFlags().has(rcc::RegisterOperandFlag::Kill);
           });
  });
  const bool HasDead = std::ranges::any_of(Function.allInstructions(), [](const auto *MI) {
    return MI->parent() != nullptr &&
           std::ranges::any_of(MI->operands(), [](const rcc::MachineOperand &Operand) {
             return Operand.registerFlags().has(rcc::RegisterOperandFlag::Dead);
           });
  });
  Context.expect(HasKill && HasDead,
                 "two-address rewrite must rebuild precise kill and dead flags");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "two-address and subregister expansion result must verify");

  auto AtomicFixture = createFunction(Context, "two_address_atomic", 10, 1);
  if (!AtomicFixture.Function || AtomicFixture.Blocks.empty())
    return;
  auto AtomicSource = createVReg(*AtomicFixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                                 rcc::test::TestRegisterClass::GPR64);
  auto AtomicDestination =
      createVReg(*AtomicFixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                 rcc::test::TestRegisterClass::GPR64);
  if (!AtomicSource || !AtomicDestination)
    return;
  const std::array AtomicSourceDef{*AtomicSource};
  Context.expect(buildRegisterInstruction(*AtomicFixture.Function, Target, *AtomicFixture.Blocks[0],
                                          testOpcode(DefOpcode), AtomicSourceDef, {})
                     .has_value(),
                 "two-address atomicity source definition must build");
  rcc::MachineIRBuilder AtomicBuilder(*AtomicFixture.Function, Target);
  if (!AtomicBuilder.setInsertPoint(*AtomicFixture.Blocks[0]))
    return;
  auto AtomicPending = AtomicBuilder.buildInstr(testOpcode(TiedOpcode));
  auto AtomicInstruction =
      AtomicPending && AtomicPending->addDef(*AtomicDestination) &&
              AtomicPending->addUse(*AtomicSource, {rcc::RegisterOperandFlag::Kill})
          ? AtomicPending->build()
          : rcc::Expected<rcc::MachineInstr *>{rcc::makeUnexpected(
                rcc::ErrorCode::InternalError, "failed to build atomicity fixture")};
  Context.expect(AtomicInstruction.has_value(),
                 "two-address atomicity tied instruction must build");
  if (!AtomicInstruction)
    return;
  auto AtomicLiveness = setSelectedSSAProperties(*AtomicFixture.Function, Target);
  Context.expect(AtomicLiveness.has_value(), "two-address atomicity liveness rebuild must succeed");
  if (!AtomicLiveness)
    return;
  AtomicFixture.Function->properties().set(rcc::MachineFunctionProperty::NoPHIs);
  const std::size_t AtomicInstructionsBefore = AtomicFixture.Function->allInstructions().size();
  const std::size_t AtomicRegistersBefore =
      AtomicFixture.Function->registerInfo().virtualRegisters().size();
  const std::uint64_t AtomicPropertiesBefore = AtomicFixture.Function->properties().rawValue();

  TestInstrInfo RejectingInfo(true);
  rcc::MachineFunctionAnalysisManager AtomicAnalyses;
  rcc::TwoAddressInstructionPass FailingPass(RejectingInfo);
  auto Failed = FailingPass.run(*AtomicFixture.Function, Target, AtomicAnalyses);
  Context.expect(!Failed && Failed.error().code() == rcc::ErrorCode::Unsupported,
                 "COPY rejection must produce a structured two-address failure");
  Context.expect(
      AtomicFixture.Function->allInstructions().size() == AtomicInstructionsBefore &&
          AtomicFixture.Function->registerInfo().virtualRegisters().size() ==
              AtomicRegistersBefore &&
          AtomicFixture.Function->properties().rawValue() == AtomicPropertiesBefore &&
          (*AtomicInstruction)->operandAt(0)->registerValue() == *AtomicDestination &&
          (*AtomicInstruction)->operandAt(1)->registerValue() == *AtomicSource &&
          (*AtomicInstruction)->operandAt(1)->registerFlags().has(rcc::RegisterOperandFlag::Kill),
      "failed two-address transaction preflight must leave the original function unchanged");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "two-address TestTarget must be created");
  if (!Target)
    return Context.result();
  TestInstrInfo Info;
  testTwoAddressAndSubregisterPseudos(Context, *Target, Info);
  return Context.result();
}
