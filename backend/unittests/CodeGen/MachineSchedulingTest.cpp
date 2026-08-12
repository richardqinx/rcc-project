// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineScheduler.h"
#include "rcc/CodeGen/PostRAScheduler.h"
#include "rcc/CodeGen/ScheduleDAG.h"
#include "rcc/CodeGen/ScheduleHazardRecognizer.h"
#include "rcc/CodeGen/TargetSchedulingModel.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

struct Fixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Block = nullptr;
};

class OnePerCycleHazard final : public rcc::TargetScheduleHazardRecognizer {
public:
  [[nodiscard]] std::unique_ptr<rcc::TargetScheduleHazardRecognizer> clone() const override {
    return std::make_unique<OnePerCycleHazard>(*this);
  }

  [[nodiscard]] rcc::Expected<rcc::ScheduleHazardType>
  getHazardType(const rcc::ScheduleUnit &) const override {
    return Emitted ? rcc::ScheduleHazardType::Hazard : rcc::ScheduleHazardType::NoHazard;
  }

  [[nodiscard]] rcc::Expected<void> emitInstruction(const rcc::ScheduleUnit &) override {
    Emitted = true;
    return {};
  }

  [[nodiscard]] rcc::Expected<void> advanceCycle() override {
    Emitted = false;
    return {};
  }

  [[nodiscard]] rcc::Expected<void> recedeCycle() override {
    Emitted = false;
    return {};
  }

  [[nodiscard]] rcc::Expected<void> reset() override {
    Emitted = false;
    return {};
  }

private:
  bool Emitted = false;
};

class TestSchedulingHooks final : public rcc::TargetSchedulingHooks {
public:
  bool MakeADDIBoundary = false;
  bool AddMutation = false;
  bool PreferSecondUnit = false;
  bool EnableDynamicHazards = false;

  [[nodiscard]] bool isSchedulingBoundary(const rcc::MachineInstr &Instruction,
                                          const rcc::TargetDescription &) const noexcept override {
    return MakeADDIBoundary &&
           Instruction.opcode() == rcc::Opcode::fromValue(static_cast<std::uint32_t>(
                                       rcc::riscv::InstructionOpcode::ADDI));
  }

  [[nodiscard]] rcc::Expected<void> mutateScheduleDAG(rcc::ScheduleDAG &DAG,
                                                      const rcc::MachineFunction &,
                                                      const rcc::TargetDescription &,
                                                      rcc::ScheduleDAGMode) const override {
    if (!AddMutation || DAG.units().size() < 2)
      return {};
    return DAG.addArtificialDependency(0, 1, 2);
  }

  [[nodiscard]] std::int64_t candidatePreference(const rcc::ScheduleUnit &Unit,
                                                 rcc::ScheduleDirection,
                                                 const rcc::MachineBasicBlock &,
                                                 std::uint64_t) const noexcept override {
    return PreferSecondUnit && Unit.ID == 1 ? 100 : 0;
  }

  [[nodiscard]] std::unique_ptr<rcc::TargetScheduleHazardRecognizer>
  createHazardRecognizer(rcc::ScheduleDirection) const override {
    return EnableDynamicHazards ? std::make_unique<OnePerCycleHazard>() : nullptr;
  }
};

[[nodiscard]] rcc::Opcode opcode(rcc::riscv::InstructionOpcode Value) noexcept {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] rcc::Expected<rcc::TargetSchedulingModel>
makeSchedulingModel(const rcc::TargetSchedulingHooks *Hooks = nullptr) {
  rcc::TargetSchedulingModelDesc Description;
  Description.IssueWidth = 2;
  Description.DispatchLimit = 2;
  for (const rcc::riscv::SchedulingClassInfo &Class : rcc::riscv::schedulingClasses()) {
    Description.SchedulingClasses.push_back({
        .ID = Class.ID,
        .Name = std::string(Class.Name),
        .MicroOps = Class.MicroOps,
        .Latency = Class.Latency,
        .Resources = {},
        .WriteLatencies = {{0, Class.Latency}},
        .ReadAdvances = {},
    });
  }
  Description.PressureSets.push_back({
      .ID = rcc::RegisterPressureSetID::fromValue(1),
      .Name = "gpr",
      .RegisterClasses =
          {
              rcc::RegisterClassID::fromValue(
                  static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR)),
          },
      .Limit = 27,
  });
  Description.Hooks = Hooks;
  return rcc::TargetSchedulingModel::create(std::move(Description));
}

[[nodiscard]] rcc::Register physical(TestContext &Context, rcc::riscv::PhysicalRegisterID ID) {
  auto Value = rcc::riscv::physicalRegister(ID);
  Context.expect(Value.has_value(), "physical register creation must succeed");
  return Value ? rcc::Register::fromPhysical(*Value) : rcc::Register{};
}

[[nodiscard]] Fixture makeFixture(TestContext &Context, std::string Name, std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "scheduler function creation must succeed");
  if (!Function)
    return {};
  auto Block = (*Function)->createBasicBlock();
  Context.expect(Block.has_value() && (*Function)->appendBlock(**Block).has_value(),
                 "scheduler block creation must succeed");
  if (!Block)
    return {};
  (*Function)->properties().set(rcc::MachineFunctionProperty::Legalized);
  (*Function)->properties().set(rcc::MachineFunctionProperty::RegBankSelected);
  (*Function)->properties().set(rcc::MachineFunctionProperty::Selected);
  (*Function)->properties().set(rcc::MachineFunctionProperty::NoPHIs);
  (*Function)->properties().set(rcc::MachineFunctionProperty::TiedOpsRewritten);
  return {std::move(*Function), *Block};
}

[[nodiscard]] rcc::Register makeGPR(TestContext &Context, rcc::MachineFunction &Function) {
  auto Type = rcc::LLT::makeInteger(64);
  auto Reg = Type ? Function.registerInfo().createVirtualRegister(*Type)
                  : rcc::Expected<rcc::Register>(std::unexpected(
                        rcc::Error(rcc::ErrorCode::InternalError, "test LLT creation failed")));
  Context.expect(Reg.has_value(), "scheduler virtual register creation must succeed");
  if (!Reg)
    return {};
  const auto Bank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto Class =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  Context.expect(Function.registerInfo().setRegisterBank(*Reg, Bank).has_value() &&
                     Function.registerInfo().setRegisterClass(*Reg, Class).has_value(),
                 "scheduler virtual register constraints must be assigned");
  return *Reg;
}

[[nodiscard]] rcc::MachineInstr *buildADDI(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                           rcc::Register Destination, rcc::Register Source,
                                           std::int64_t Immediate) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ADDI));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Source) ||
      !Pending->addImmediate(Immediate)) {
    Context.expect(false, "scheduler ADDI construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "scheduler ADDI insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] rcc::MachineInstr *buildBinary(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                             rcc::riscv::InstructionOpcode Opcode,
                                             rcc::Register Destination, rcc::Register Left,
                                             rcc::Register Right) {
  auto Pending = Builder.buildInstr(opcode(Opcode));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Left) ||
      !Pending->addUse(Right)) {
    Context.expect(false, "scheduler binary instruction construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "scheduler binary instruction insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] bool hasDependency(const rcc::ScheduleUnit &Unit, std::size_t Destination,
                                 rcc::ScheduleDependencyKind Kind) {
  for (const rcc::ScheduleDependency &Dependency : Unit.Successors) {
    if (Dependency.Destination == Destination && Dependency.Kind == Kind)
      return true;
  }
  return false;
}

void testDAGRegisterDependencies(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "schedule_dag", 1);
  if (!Fixture.Function || Fixture.Block == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                 "schedule DAG insertion point must be established");
  const rcc::Register X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::Register X10 = physical(Context, rcc::riscv::PhysicalRegisterID::X10);
  const rcc::Register Value = makeGPR(Context, *Fixture.Function);
  const rcc::Register Consumer = makeGPR(Context, *Fixture.Function);
  rcc::MachineInstr *Definition = buildADDI(Context, Builder, Value, X0, 1);
  rcc::MachineInstr *Use =
      buildBinary(Context, Builder, rcc::riscv::InstructionOpcode::ADD, Consumer, Value, X10);
  rcc::MachineInstr *Redefinition = buildADDI(Context, Builder, Value, X0, 2);
  std::array<rcc::MachineInstr *, 3> Instructions{Definition, Use, Redefinition};
  auto DAG = rcc::ScheduleDAG::build(*Fixture.Block, Instructions, *Fixture.Function, Target,
                                     rcc::riscv::schedulingModel(),
                                     rcc::ScheduleDAGMode::PreRegisterAllocation);
  if (!DAG)
    std::cerr << "schedule DAG failure: " << DAG.error().message() << '\n';
  Context.expect(DAG.has_value() && DAG->units().size() == 3,
                 "schedule DAG must create one unit per instruction");
  if (!DAG)
    return;
  Context.expect(hasDependency(DAG->units()[0], 1, rcc::ScheduleDependencyKind::Data),
                 "schedule DAG must model virtual-register data dependencies");
  Context.expect(hasDependency(DAG->units()[0], 2, rcc::ScheduleDependencyKind::Output),
                 "schedule DAG must model virtual-register output dependencies");
  Context.expect(hasDependency(DAG->units()[1], 2, rcc::ScheduleDependencyKind::Anti),
                 "schedule DAG must model virtual-register anti dependencies");
  Context.expect(DAG->units()[0].Latency != 0 && DAG->units()[0].MicroOps != 0,
                 "schedule DAG must attach target latency and resource facts");
}

void testHazardScoreboard(TestContext &Context) {
  auto Hazards = rcc::ScheduleHazardRecognizer::create(rcc::riscv::schedulingModel());
  Context.expect(Hazards.has_value(), "hazard recognizer construction must succeed");
  if (!Hazards)
    return;
  const auto Memory = rcc::ProcessorResourceID::fromValue(3);
  rcc::ScheduleUnit Load;
  Load.MicroOps = 1;
  Load.Resources = {{Memory, 1}};
  Context.expect(Hazards->issue(Load, 0).has_value(),
                 "first memory operation must issue in cycle zero");
  auto SecondCycle = Hazards->earliestIssueCycle(Load, 0);
  Context.expect(SecondCycle.has_value() && *SecondCycle == 1,
                 "single-unit memory resource must serialize operations");

  rcc::ScheduleUnit Wide;
  Wide.MicroOps = 5;
  auto Fresh = rcc::ScheduleHazardRecognizer::create(rcc::riscv::schedulingModel());
  Context.expect(Fresh.has_value() && Fresh->issue(Wide, 0).has_value(),
                 "multi-cycle dispatch must accept instructions wider than issue width");
  auto AfterWide = Fresh ? Fresh->earliestIssueCycle(Load, 0)
                         : rcc::Expected<std::uint64_t>(std::unexpected(
                               rcc::Error(rcc::ErrorCode::InternalError, "no recognizer")));
  Context.expect(AfterWide.has_value() && *AfterWide == 3,
                 "multi-cycle dispatch must block the complete dispatch interval");
}

void testOperandLatencyAndModelValidation(TestContext &Context) {
  const rcc::SchedulingClassID ALU = rcc::SchedulingClassID::fromValue(
      static_cast<std::uint32_t>(rcc::riscv::SchedulingClass::ALU));
  const rcc::SchedulingClassID Load = rcc::SchedulingClassID::fromValue(
      static_cast<std::uint32_t>(rcc::riscv::SchedulingClass::LOAD));
  auto Forwarded = rcc::riscv::schedulingModel().operandLatency(Load, 0, ALU, 1);
  auto Unforwarded = rcc::riscv::schedulingModel().operandLatency(Load, 0, ALU, 0);
  auto ALUChain = rcc::riscv::schedulingModel().operandLatency(ALU, 0, ALU, 1);
  Context.expect(Forwarded.has_value() && *Forwarded == 2 && Unforwarded.has_value() &&
                     *Unforwarded == 3 && ALUChain.has_value() && *ALUChain == 1,
                 "operand latency must combine write latency with exact read advance");

  rcc::TargetSchedulingModelDesc Invalid;
  Invalid.SchedulingClasses.push_back({
      .ID = ALU,
      .Name = "invalid",
      .MicroOps = 1,
      .Latency = 1,
      .Resources = {},
      .WriteLatencies = {{0, 1}, {0, 2}},
      .ReadAdvances = {},
  });
  auto Rejected = rcc::TargetSchedulingModel::create(std::move(Invalid));
  Context.expect(!Rejected && Rejected.error().code() == rcc::ErrorCode::InvalidArgument,
                 "scheduling model must reject duplicate operand latency facts");
}

void testDynamicTargetHazards(TestContext &Context) {
  TestSchedulingHooks Hooks;
  Hooks.EnableDynamicHazards = true;
  auto Model = makeSchedulingModel(&Hooks);
  Context.expect(Model.has_value(), "dynamic-hazard scheduling model construction must succeed");
  if (!Model)
    return;

  rcc::ScheduleUnit First;
  First.ID = 1;
  First.MicroOps = 1;
  rcc::ScheduleUnit Second;
  Second.ID = 2;
  Second.MicroOps = 1;
  auto Top = rcc::ScheduleHazardRecognizer::create(*Model, rcc::ScheduleDirection::TopDown);
  Context.expect(Top.has_value() && Top->issue(First, 0).has_value(),
                 "target dynamic hazard must accept the first top-down issue");
  auto TopCycle = Top ? Top->earliestIssueCycle(Second, 0)
                      : rcc::Expected<std::uint64_t>(std::unexpected(
                            rcc::Error(rcc::ErrorCode::InternalError, "no top-down recognizer")));
  Context.expect(TopCycle.has_value() && *TopCycle == 1,
                 "target dynamic hazard must advance the top-down cycle");

  auto Bottom = rcc::ScheduleHazardRecognizer::create(*Model, rcc::ScheduleDirection::BottomUp);
  Context.expect(Bottom.has_value() && Bottom->issue(First, 0).has_value(),
                 "target dynamic hazard must accept the first bottom-up issue");
  auto BottomCycle = Bottom ? Bottom->earliestIssueCycle(Second, 0)
                            : rcc::Expected<std::uint64_t>(std::unexpected(rcc::Error(
                                  rcc::ErrorCode::InternalError, "no bottom-up recognizer")));
  Context.expect(BottomCycle.has_value() && *BottomCycle == 1,
                 "target dynamic hazard must recede the bottom-up cycle");
}

void testTargetSchedulingHooks(TestContext &Context, const rcc::TargetDescription &Target) {
  TestSchedulingHooks MutationHooks;
  MutationHooks.AddMutation = true;
  auto MutationModel = makeSchedulingModel(&MutationHooks);
  auto Fixture = makeFixture(Context, "schedule_mutation", 4);
  if (!MutationModel || !Fixture.Function || Fixture.Block == nullptr) {
    Context.expect(false, "target schedule mutation fixture must be created");
    return;
  }
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                 "target schedule mutation insertion point must be established");
  const rcc::Register X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  rcc::MachineInstr *First =
      buildADDI(Context, Builder, makeGPR(Context, *Fixture.Function), X0, 1);
  rcc::MachineInstr *Second =
      buildADDI(Context, Builder, makeGPR(Context, *Fixture.Function), X0, 2);
  std::array<rcc::MachineInstr *, 2> Instructions{First, Second};
  auto DAG = rcc::ScheduleDAG::build(*Fixture.Block, Instructions, *Fixture.Function, Target,
                                     *MutationModel, rcc::ScheduleDAGMode::PreRegisterAllocation);
  Context.expect(DAG.has_value() &&
                     hasDependency(DAG->units()[0], 1, rcc::ScheduleDependencyKind::Artificial),
                 "target schedule mutation must add a checked artificial dependency");

  TestSchedulingHooks PreferenceHooks;
  PreferenceHooks.PreferSecondUnit = true;
  auto PreferenceModel = makeSchedulingModel(&PreferenceHooks);
  if (!PreferenceModel) {
    Context.expect(false, "target scheduling preference model must be created");
    return;
  }
  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineSchedulerPass PreferencePass(
      *PreferenceModel, {.MaximumRegionSize = 8, .EnableBidirectionalScheduling = false});
  auto Preferred = PreferencePass.run(*Fixture.Function, Target, Analyses);
  Context.expect(Preferred.has_value() && Preferred->Changed &&
                     Fixture.Block->firstInstruction() == Second,
                 "target candidate preference must affect the production scheduler");

  auto BoundaryFixture = makeFixture(Context, "schedule_boundary", 5);
  TestSchedulingHooks BoundaryHooks;
  BoundaryHooks.MakeADDIBoundary = true;
  auto BoundaryModel = makeSchedulingModel(&BoundaryHooks);
  if (!BoundaryFixture.Function || BoundaryFixture.Block == nullptr || !BoundaryModel) {
    Context.expect(false, "target schedule boundary fixture must be created");
    return;
  }
  rcc::MachineIRBuilder BoundaryBuilder(*BoundaryFixture.Function, Target);
  Context.expect(BoundaryBuilder.setInsertPoint(*BoundaryFixture.Block).has_value(),
                 "target schedule boundary insertion point must be established");
  rcc::MachineInstr *BoundaryFirst =
      buildADDI(Context, BoundaryBuilder, makeGPR(Context, *BoundaryFixture.Function), X0, 1);
  (void)buildADDI(Context, BoundaryBuilder, makeGPR(Context, *BoundaryFixture.Function), X0, 2);
  rcc::MachineFunctionAnalysisManager BoundaryAnalyses;
  rcc::MachineSchedulerPass BoundaryPass(
      *BoundaryModel, {.MaximumRegionSize = 8, .EnableBidirectionalScheduling = false});
  auto BoundaryResult = BoundaryPass.run(*BoundaryFixture.Function, Target, BoundaryAnalyses);
  Context.expect(BoundaryResult.has_value() && !BoundaryResult->Changed &&
                     BoundaryFixture.Block->firstInstruction() == BoundaryFirst,
                 "target scheduling boundary must split production schedule regions");
}

void buildSchedulingChain(TestContext &Context, Fixture &Fixture,
                          const rcc::TargetDescription &Target, bool PhysicalRegisters,
                          rcc::MachineInstr *&Short, rcc::MachineInstr *&Long) {
  rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
  Context.expect(Builder.setInsertPoint(*Fixture.Block).has_value(),
                 "scheduler chain insertion point must be established");
  const rcc::Register X0 = physical(Context, rcc::riscv::PhysicalRegisterID::X0);
  const rcc::Register X10 = physical(Context, rcc::riscv::PhysicalRegisterID::X10);
  const rcc::Register X11 = physical(Context, rcc::riscv::PhysicalRegisterID::X11);
  const rcc::Register ShortValue = PhysicalRegisters
                                       ? physical(Context, rcc::riscv::PhysicalRegisterID::X12)
                                       : makeGPR(Context, *Fixture.Function);
  const rcc::Register LongValue = PhysicalRegisters
                                      ? physical(Context, rcc::riscv::PhysicalRegisterID::X13)
                                      : makeGPR(Context, *Fixture.Function);
  const rcc::Register LongResult = PhysicalRegisters
                                       ? physical(Context, rcc::riscv::PhysicalRegisterID::X14)
                                       : makeGPR(Context, *Fixture.Function);
  const rcc::Register ShortResult = PhysicalRegisters
                                        ? physical(Context, rcc::riscv::PhysicalRegisterID::X15)
                                        : makeGPR(Context, *Fixture.Function);
  Short = buildADDI(Context, Builder, ShortValue, X0, 1);
  Long = buildBinary(Context, Builder, rcc::riscv::InstructionOpcode::DIV, LongValue, X10, X11);
  (void)buildBinary(Context, Builder, rcc::riscv::InstructionOpcode::ADD, LongResult, LongValue,
                    X10);
  (void)buildBinary(Context, Builder, rcc::riscv::InstructionOpcode::ADD, ShortResult, ShortValue,
                    X11);
}

void testPreRAScheduler(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "pre_ra_schedule", 2);
  if (!Fixture.Function || Fixture.Block == nullptr)
    return;
  rcc::MachineInstr *Short = nullptr;
  rcc::MachineInstr *Long = nullptr;
  buildSchedulingChain(Context, Fixture, Target, false, Short, Long);
  Context.expect(Fixture.Block->firstInstruction() == Short,
                 "pre-RA fixture must begin in intentionally suboptimal order");
  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::MachineSchedulerPass Pass(rcc::riscv::schedulingModel(),
                                 {.MaximumRegionSize = 64, .EnableBidirectionalScheduling = false});
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "pre-RA scheduler failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "pre-RA scheduler must reorder the latency-critical chain");
  Context.expect(Fixture.Block->firstInstruction() == Long,
                 "pre-RA scheduler must prioritize the long critical path");
}

void testPostRASchedulerAndLivenessRepair(TestContext &Context,
                                          const rcc::TargetDescription &Target) {
  auto Fixture = makeFixture(Context, "post_ra_schedule", 3);
  if (!Fixture.Function || Fixture.Block == nullptr)
    return;
  Fixture.Function->properties().set(rcc::MachineFunctionProperty::NoVRegs);
  Fixture.Function->properties().set(rcc::MachineFunctionProperty::TracksLiveness);
  rcc::MachineInstr *Short = nullptr;
  rcc::MachineInstr *Long = nullptr;
  buildSchedulingChain(Context, Fixture, Target, true, Short, Long);
  auto Rebuilt = rcc::rebuildMachineLiveness(*Fixture.Function, Target);
  Context.expect(Rebuilt.has_value(), "post-RA fixture liveness rebuild must succeed");
  if (!Rebuilt)
    return;

  rcc::MachineFunctionAnalysisManager Analyses;
  rcc::PostRASchedulerPass Pass(rcc::riscv::schedulingModel(),
                                {.MaximumRegionSize = 64, .EnableBidirectionalScheduling = false});
  auto Result = Pass.run(*Fixture.Function, Target, Analyses);
  if (!Result)
    std::cerr << "post-RA scheduler failure: " << Result.error().message() << '\n';
  Context.expect(Result.has_value() && Result->Changed,
                 "post-RA scheduler must reorder the latency-critical chain");
  Context.expect(Fixture.Block->firstInstruction() == Long,
                 "post-RA scheduler must prioritize the long critical path");
  auto Verified = rcc::verifyTrackedMachineLiveness(*Fixture.Function, Target);
  if (!Verified)
    std::cerr << "post-RA liveness verification failure: " << Verified.error().message() << '\n';
  Context.expect(Verified.has_value(),
                 "post-RA scheduler must locally repair exact kill/dead state");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "scheduler target creation must succeed");
  if (!Target)
    return Context.result();
  testDAGRegisterDependencies(Context, *Target);
  testHazardScoreboard(Context);
  testOperandLatencyAndModelValidation(Context);
  testDynamicTargetHazards(Context);
  testTargetSchedulingHooks(Context, *Target);
  testPreRAScheduler(Context, *Target);
  testPostRASchedulerAndLivenessRepair(Context, *Target);
  return Context.result();
}
