// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetCallLowering.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"

#include <concepts>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] rcc::Alignment align(std::uint64_t Bytes) {
  const auto Value = rcc::Alignment::create(Bytes);
  if (!Value)
    std::terminate();
  return *Value;
}

[[nodiscard]] rcc::LowLevelType integer(std::uint32_t Bits) {
  const auto Type = rcc::LowLevelType::makeCategoryNeutralScalar(Bits);
  if (!Type)
    std::terminate();
  return *Type;
}

[[nodiscard]] rcc::PhysicalRegister physical(std::uint32_t ID) {
  const auto Register = rcc::PhysicalRegister::create(ID);
  if (!Register)
    std::terminate();
  return *Register;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  rcc::InstructionDescriptor Nop;
  Nop.InstructionOpcode = rcc::Opcode::fromValue(1);
  Nop.Name = "TEST_NOP";
  Nop.Flags = {rcc::InstructionFlag::ProducesCode};
  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Nop));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] rcc::CallLoweringPlanDesc emptyPlanDescription(rcc::CallLoweringPlanKind Kind) {
  rcc::CallLoweringPlanDesc Description;
  Description.Kind = Kind;
  Description.Target = "test-target:test-abi";
  Description.Convention = rcc::CallingConventionID::fromValue(1);
  Description.StackArguments.Align = align(8);
  Description.CallFrame.Align = align(8);
  if (Kind == rcc::CallLoweringPlanKind::Call) {
    Description.CallFrame.EmitSetup = true;
    Description.CallFrame.EmitDestroy = true;
    Description.PreservedMask = rcc::test::createTestCallPreservedMask();
    Description.Callee = *rcc::MachineCallTarget::externalSymbol("callee");
  }
  return Description;
}

[[nodiscard]] rcc::Expected<rcc::CallLoweringPlan> emptyPlan(rcc::CallLoweringPlanKind Kind) {
  return rcc::CallLoweringPlan::create(emptyPlanDescription(Kind));
}

[[nodiscard]] rcc::CCAssignment registerAssignment() {
  return {
      .ValueIndex = 0,
      .OriginalValue = rcc::Register::fromPhysical(physical(2)),
      .OriginalValueIndex = 0,
      .FragmentIndex = 0,
      .FragmentCount = 1,
      .ValueType = integer(64),
      .LocationType = integer(64),
      .LocationKind = rcc::CCLocationKind::Register,
      .LocationRegister = physical(1),
      .StackOffsetBytes = std::nullopt,
      .LocationSizeBytes = 8,
      .StackSlotSizeBytes = 8,
      .LocationAlignment = align(8),
      .Extension = rcc::ABIExtensionKind::None,
      .CustomFlags = {},
  };
}

void testPlanValidation(TestContext &Context) {
  static_assert(
      std::same_as<decltype(std::declval<const rcc::CallLoweringPlan &>().argumentAssignments()),
                   std::span<const rcc::CCAssignment>>);
  static_assert(std::same_as<decltype(std::declval<const rcc::CallLoweringPlan &>().byValCopies()),
                             std::span<const rcc::ByValCopyAssignment>>);

  auto Valid = emptyPlan(rcc::CallLoweringPlanKind::FormalArguments);
  Context.expect(Valid && Valid->target() == "test-target:test-abi" &&
                     Valid->argumentAssignments().empty(),
                 "a valid immutable formal-argument plan must be accepted");

  auto MissingRegister = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  auto Assignment = registerAssignment();
  Assignment.OriginalValue = {};
  MissingRegister.ArgumentAssignments.push_back(Assignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(MissingRegister)),
                 "an assignment without a machine value must be rejected");

  auto BadStack = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  BadStack.StackArguments.SizeBytes = 8;
  auto StackAssignment = registerAssignment();
  StackAssignment.LocationKind = rcc::CCLocationKind::Stack;
  StackAssignment.LocationRegister.reset();
  StackAssignment.StackOffsetBytes = 8;
  BadStack.ArgumentAssignments.push_back(StackAssignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(BadStack)),
                 "an assignment outside the declared stack area must be rejected");

  auto Overlap = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  Overlap.StackArguments.SizeBytes = 16;
  StackAssignment.StackOffsetBytes = 0;
  Overlap.ArgumentAssignments.push_back(StackAssignment);
  StackAssignment.OriginalValue = rcc::Register::fromPhysical(physical(3));
  StackAssignment.StackOffsetBytes = 4;
  Overlap.ArgumentAssignments.push_back(StackAssignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(Overlap)),
                 "overlapping stack assignments must be rejected");

  auto MisalignedStack = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  MisalignedStack.StackArguments.SizeBytes = 16;
  StackAssignment.StackOffsetBytes = 4;
  MisalignedStack.ArgumentAssignments.push_back(StackAssignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(MisalignedStack)),
                 "a stack assignment must satisfy its declared alignment");

  auto OversizedLocation = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  auto OversizedAssignment = registerAssignment();
  OversizedAssignment.LocationSizeBytes = 16;
  OversizedLocation.ArgumentAssignments.push_back(OversizedAssignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(OversizedLocation)),
                 "an ABI location cannot exceed its transport slot");

  auto UnknownLocation = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  auto UnknownAssignment = registerAssignment();
  UnknownAssignment.LocationKind =
      static_cast<rcc::CCLocationKind>(255); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  UnknownLocation.ArgumentAssignments.push_back(UnknownAssignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(UnknownLocation)),
                 "an unknown ABI location kind must be rejected");

  auto BadFrame = emptyPlanDescription(rcc::CallLoweringPlanKind::Call);
  BadFrame.CallFrame.EmitDestroy = false;
  Context.expect(!rcc::CallLoweringPlan::create(std::move(BadFrame)),
                 "unpaired call-frame setup and destroy requirements must be rejected");

  auto SmallFrame = emptyPlanDescription(rcc::CallLoweringPlanKind::Call);
  SmallFrame.StackArguments.SizeBytes = 16;
  SmallFrame.CallFrame.SizeBytes = 8;
  StackAssignment.StackOffsetBytes = 0;
  SmallFrame.ArgumentAssignments.push_back(StackAssignment);
  Context.expect(!rcc::CallLoweringPlan::create(std::move(SmallFrame)),
                 "a call frame must contain its complete stack-argument area");

  auto BadTail = emptyPlanDescription(rcc::CallLoweringPlanKind::Call);
  BadTail.CallFrame.EmitSetup = false;
  BadTail.CallFrame.EmitDestroy = false;
  BadTail.CallFrame.ReusesIncomingArea = true;
  BadTail.TailDecision = rcc::TailCallKind::Tail;
  BadTail.TailRejectionReason = "inconsistent accepted tail call";
  Context.expect(!rcc::CallLoweringPlan::create(std::move(BadTail)),
                 "an accepted tail call cannot retain a rejection reason");

  auto UnknownTail = emptyPlanDescription(rcc::CallLoweringPlanKind::Call);
  UnknownTail.TailDecision =
      static_cast<rcc::TailCallKind>(255); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
  Context.expect(!rcc::CallLoweringPlan::create(std::move(UnknownTail)),
                 "an unknown tail-call decision must be rejected");

  auto BadVarArgs = emptyPlanDescription(rcc::CallLoweringPlanKind::FormalArguments);
  BadVarArgs.VarArgs.IsVariadic = true;
  BadVarArgs.VarArgs.RegisterSaveAreaSizeBytes = 8;
  BadVarArgs.VarArgs.RegistersToSave.push_back({physical(1), 8});
  Context.expect(!rcc::CallLoweringPlan::create(std::move(BadVarArgs)),
                 "a variadic register save outside its area must be rejected");

  auto BadByVal = emptyPlanDescription(rcc::CallLoweringPlanKind::Call);
  auto ByValAssignment = registerAssignment();
  ByValAssignment.ValueType = *rcc::LowLevelType::makePointer(0, 64);
  ByValAssignment.LocationType = ByValAssignment.ValueType;
  ByValAssignment.CustomFlags.set(rcc::CCAssignmentFlag::ByVal);
  BadByVal.ArgumentAssignments.push_back(ByValAssignment);
  BadByVal.ByValCopies.push_back(
      {0, ByValAssignment.OriginalValue, ByValAssignment.ValueType, 0, 16, align(8)});
  Context.expect(!rcc::CallLoweringPlan::create(std::move(BadByVal)),
                 "a byval copy outside the call frame must be rejected");

  auto OverlappingByVal = emptyPlanDescription(rcc::CallLoweringPlanKind::Call);
  OverlappingByVal.CallFrame.SizeBytes = 32;
  auto FirstByVal = registerAssignment();
  FirstByVal.ValueType = *rcc::LowLevelType::makePointer(0, 64);
  FirstByVal.LocationType = FirstByVal.ValueType;
  FirstByVal.CustomFlags.set(rcc::CCAssignmentFlag::ByVal);
  auto SecondByVal = FirstByVal;
  SecondByVal.ValueIndex = 1;
  SecondByVal.OriginalValue = rcc::Register::fromPhysical(physical(3));
  SecondByVal.OriginalValueIndex = 1;
  SecondByVal.LocationRegister = physical(2);
  OverlappingByVal.ArgumentAssignments = {FirstByVal, SecondByVal};
  OverlappingByVal.ByValCopies = {
      {0, FirstByVal.OriginalValue, FirstByVal.ValueType, 0, 16, align(8)},
      {1, SecondByVal.OriginalValue, SecondByVal.ValueType, 8, 16, align(8)},
  };
  Context.expect(!rcc::CallLoweringPlan::create(std::move(OverlappingByVal)),
                 "overlapping byval copy ranges must be rejected");

  Context.expect(!rcc::MachineCallTarget::globalSymbol("") && !rcc::MachineCallTarget::indirect({}),
                 "call-target factories must reject missing targets");
}

class RecordingCallLowering final : public rcc::TargetCallLowering {
public:
  enum class Behavior : std::uint8_t {
    Valid,
    PlannerFailure,
    WrongKind,
  };

  explicit RecordingCallLowering(Behavior CurrentBehavior) : CurrentBehavior(CurrentBehavior) {}

  [[nodiscard]] rcc::Expected<rcc::CallLoweringPlan>
  planFormalArguments(const rcc::FormalArgumentRequest &) const override {
    return plan(rcc::CallLoweringPlanKind::FormalArguments);
  }

  [[nodiscard]] rcc::Expected<rcc::CallLoweringPlan>
  planCall(const rcc::CallLoweringRequest &) const override {
    return plan(rcc::CallLoweringPlanKind::Call);
  }

  [[nodiscard]] rcc::Expected<rcc::CallLoweringPlan>
  planReturn(const rcc::ReturnLoweringRequest &) const override {
    return plan(rcc::CallLoweringPlanKind::Return);
  }

  [[nodiscard]] rcc::Expected<void> emitPlan(rcc::MachineIRBuilder &,
                                             const rcc::CallLoweringPlan &) const override {
    ++EmissionCount;
    return {};
  }

  [[nodiscard]] std::uint32_t emissions() const noexcept {
    return EmissionCount;
  }

private:
  [[nodiscard]] rcc::Expected<rcc::CallLoweringPlan>
  plan(rcc::CallLoweringPlanKind RequestedKind) const {
    if (CurrentBehavior == Behavior::PlannerFailure) {
      return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                                 "intentional call-lowering planner failure");
    }
    if (CurrentBehavior == Behavior::WrongKind)
      RequestedKind = RequestedKind == rcc::CallLoweringPlanKind::Call
                          ? rcc::CallLoweringPlanKind::Return
                          : rcc::CallLoweringPlanKind::Call;
    return emptyPlan(RequestedKind);
  }

  Behavior CurrentBehavior;
  mutable std::uint32_t EmissionCount = 0;
};

void testPlanThenEmitBoundary(TestContext &Context) {
  auto Target = createTarget();
  auto Function =
      rcc::MachineFunction::create("plan_then_emit", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Target && Function, "call-lowering workflow fixtures must be valid");
  if (!Target || !Function)
    return;
  rcc::MachineIRBuilder Builder(**Function, *Target);
  rcc::CallLoweringRequest Request;

  RecordingCallLowering Failing(RecordingCallLowering::Behavior::PlannerFailure);
  auto Failure = Failing.lowerCall(Builder, Request);
  Context.expect(!Failure && Failing.emissions() == 0 && (*Function)->allInstructions().empty(),
                 "planner failure must not invoke emission or mutate MachineIR");

  RecordingCallLowering WrongKind(RecordingCallLowering::Behavior::WrongKind);
  auto Mismatch = WrongKind.lowerCall(Builder, Request);
  Context.expect(!Mismatch && Mismatch.error().code() == rcc::ErrorCode::InternalError &&
                     WrongKind.emissions() == 0,
                 "a target returning the wrong plan kind must be rejected before emission");

  RecordingCallLowering Valid(RecordingCallLowering::Behavior::Valid);
  Context.expect(Valid.lowerCall(Builder, Request) && Valid.emissions() == 1,
                 "a valid plan must be emitted exactly once");
}

} // namespace

int main() {
  TestContext Context;
  testPlanValidation(Context);
  testPlanThenEmitBoundary(Context);
  return Context.result();
}
