// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetCallLowering.h"

#include "rcc/CodeGen/MachineIRBuilder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] bool isKnownExtension(ABIExtensionKind Extension) noexcept {
  return Extension < ABIExtensionKind::Count;
}

[[nodiscard]] bool isKnownLocationKind(CCLocationKind Kind) noexcept {
  return Kind <= CCLocationKind::Custom;
}

[[nodiscard]] bool isKnownTailCallKind(TailCallKind Kind) noexcept {
  return Kind <= TailCallKind::MustTail;
}

[[nodiscard]] bool addOverflows(std::uint64_t Left, std::uint64_t Right) noexcept {
  return Left > std::numeric_limits<std::uint64_t>::max() - Right;
}

[[nodiscard]] Expected<void> verifyAssignment(const CCAssignment &Assignment,
                                              bool AllowNoRegister) {
  if (Assignment.OriginalValue.isNoRegister() && !AllowNoRegister) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering assignment requires a machine register value");
  }
  if (!Assignment.ValueType.isValid() || !Assignment.LocationType.isValid() ||
      !Assignment.ValueType.isScalarValue() || !Assignment.LocationType.isScalarValue()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering assignments require scalar value and location LLTs");
  }
  if (Assignment.FragmentCount == 0 || Assignment.FragmentIndex >= Assignment.FragmentCount) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering assignment has invalid fragment coordinates");
  }
  if (Assignment.LocationSizeBytes == 0 || Assignment.StackSlotSizeBytes == 0 ||
      Assignment.LocationSizeBytes > Assignment.StackSlotSizeBytes ||
      !isKnownExtension(Assignment.Extension) || !isKnownLocationKind(Assignment.LocationKind)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering assignment has invalid size or extension metadata");
  }

  const bool HasRegister = Assignment.LocationRegister.has_value();
  const bool HasStack = Assignment.StackOffsetBytes.has_value();
  if (HasRegister && !Assignment.LocationRegister->isValid()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering assignment names an invalid physical register");
  }
  if (HasStack && *Assignment.StackOffsetBytes % Assignment.LocationAlignment.bytes() != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "stack assignment offset violates its location alignment");
  }
  switch (Assignment.LocationKind) {
  case CCLocationKind::Register:
    if (!HasRegister || HasStack) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "register assignment must name exactly one physical register");
    }
    break;
  case CCLocationKind::Stack:
    if (HasRegister || !HasStack) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "stack assignment must name exactly one stack offset");
    }
    break;
  case CCLocationKind::Indirect:
    if (HasRegister == HasStack || !Assignment.CustomFlags.has(CCAssignmentFlag::Indirect)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "indirect assignment must have one transport location and its flag");
    }
    break;
  case CCLocationKind::Custom:
    if (HasRegister && HasStack) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "custom assignment cannot use register and stack simultaneously");
    }
    break;
  }
  return {};
}

[[nodiscard]] Expected<void> verifyAssignments(std::span<const CCAssignment> Assignments,
                                               bool AllowNoRegister,
                                               const StackArgumentArea &StackArea) {
  std::unordered_set<PhysicalRegister> Registers;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> StackRanges;
  StackRanges.reserve(Assignments.size());
  for (const CCAssignment &Assignment : Assignments) {
    auto Verification = verifyAssignment(Assignment, AllowNoRegister);
    if (!Verification)
      return std::unexpected(Verification.error());
    if (Assignment.LocationRegister && !Registers.insert(*Assignment.LocationRegister).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "call-lowering assignments reuse a physical location register");
    }
    if (!Assignment.StackOffsetBytes)
      continue;
    if (addOverflows(*Assignment.StackOffsetBytes, Assignment.StackSlotSizeBytes)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "call-lowering stack assignment range overflows uint64");
    }
    const std::uint64_t End = *Assignment.StackOffsetBytes + Assignment.StackSlotSizeBytes;
    if (End > StackArea.SizeBytes) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "call-lowering stack assignment exceeds its argument area");
    }
    StackRanges.emplace_back(*Assignment.StackOffsetBytes, End);
  }

  std::ranges::sort(StackRanges);
  for (std::size_t Index = 1; Index < StackRanges.size(); ++Index) {
    if (StackRanges[Index].first < StackRanges[Index - 1].second) {
      return makeUnexpected(ErrorCode::InvalidArgument, "call-lowering stack assignments overlap");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> verifyVarArgs(const VarArgLoweringState &State) {
  if (!State.IsVariadic) {
    if (State.FixedArgumentCount != 0 || State.FirstUnallocatedIntegerRegister != 0 ||
        State.OverflowAreaOffsetBytes != 0 || State.RegisterSaveAreaOffsetBytes != 0 ||
        State.RegisterSaveAreaSizeBytes != 0 || State.RegisterSavePaddingBytes != 0 ||
        !State.RegistersToSave.empty()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "non-variadic plan contains variadic lowering state");
    }
    return {};
  }

  if (State.RegisterSaveAreaSizeBytes == 0 && !State.RegistersToSave.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "variadic register saves require a nonempty save area");
  }
  if (State.RegisterSaveAreaSizeBytes % 8 != 0 || State.RegisterSavePaddingBytes % 8 != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "variadic register-save size and padding must be XLEN aligned");
  }
  std::unordered_set<PhysicalRegister> SavedRegisters;
  std::unordered_set<std::uint64_t> SavedOffsets;
  for (const VarArgRegisterSave Save : State.RegistersToSave) {
    if (!Save.Reg.isValid() || !SavedRegisters.insert(Save.Reg).second ||
        Save.OffsetBytes % 8 != 0 || !SavedOffsets.insert(Save.OffsetBytes).second ||
        addOverflows(Save.OffsetBytes, std::uint64_t{8}) ||
        Save.OffsetBytes + 8 > State.RegisterSaveAreaSizeBytes) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "variadic register-save entry is invalid or outside its area");
    }
  }
  return {};
}

} // namespace

ABIValueFlags::ABIValueFlags(std::initializer_list<ABIValueFlag> Flags) noexcept {
  for (const ABIValueFlag Flag : Flags)
    set(Flag);
}

CCAssignmentFlags::CCAssignmentFlags(std::initializer_list<CCAssignmentFlag> Flags) noexcept {
  for (const CCAssignmentFlag Flag : Flags)
    set(Flag);
}

Expected<MachineCallTarget> MachineCallTarget::globalSymbol(std::string Name,
                                                            std::uint32_t TargetFlags) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "global call target name cannot be empty");
  return MachineCallTarget(MachineCallTargetKind::GlobalSymbol, std::move(Name), TargetFlags);
}

Expected<MachineCallTarget> MachineCallTarget::externalSymbol(std::string Name,
                                                              std::uint32_t TargetFlags) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "external call target name cannot be empty");
  return MachineCallTarget(MachineCallTargetKind::ExternalSymbol, std::move(Name), TargetFlags);
}

Expected<MachineCallTarget> MachineCallTarget::indirect(Register Callee,
                                                        std::uint32_t TargetFlags) {
  if (Callee.isNoRegister()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "indirect call target requires a machine register");
  }
  return MachineCallTarget(MachineCallTargetKind::IndirectRegister, Callee, TargetFlags);
}

std::string_view MachineCallTarget::symbolName() const noexcept {
  const auto *Name = std::get_if<std::string>(&Value);
  return Name == nullptr ? std::string_view{} : std::string_view(*Name);
}

Register MachineCallTarget::indirectRegister() const noexcept {
  const auto *Reg = std::get_if<Register>(&Value);
  return Reg == nullptr ? Register{} : *Reg;
}

Expected<CallLoweringPlan> CallLoweringPlan::create(CallLoweringPlanDesc Description) {
  if (Description.Target.empty() || Description.Convention.value() == 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering plan requires target and calling-convention identities");
  }
  if (Description.Kind > CallLoweringPlanKind::Return ||
      !isKnownTailCallKind(Description.TailDecision)) {
    return makeUnexpected(ErrorCode::InvalidArgument, "call-lowering plan kind is invalid");
  }
  if (Description.StackArguments.ShadowSpaceBytes > Description.StackArguments.SizeBytes ||
      Description.StackArguments.SizeBytes % Description.StackArguments.Align.bytes() != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering stack argument area is malformed");
  }
  if (Description.CallFrame.SizeBytes % Description.CallFrame.Align.bytes() != 0 ||
      Description.CallFrame.EmitSetup != Description.CallFrame.EmitDestroy) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering call-frame requirements are inconsistent");
  }

  auto ArgumentVerification =
      verifyAssignments(Description.ArgumentAssignments, false, Description.StackArguments);
  if (!ArgumentVerification)
    return std::unexpected(ArgumentVerification.error());
  const bool AllowNoReturnRegister = Description.Kind == CallLoweringPlanKind::Call &&
                                     Description.TailDecision != TailCallKind::NotTail;
  auto ReturnVerification = verifyAssignments(Description.ReturnAssignments, AllowNoReturnRegister,
                                              StackArgumentArea{0, 0, Alignment{}});
  if (!ReturnVerification)
    return std::unexpected(ReturnVerification.error());
  auto VarArgVerification = verifyVarArgs(Description.VarArgs);
  if (!VarArgVerification)
    return std::unexpected(VarArgVerification.error());

  switch (Description.Kind) {
  case CallLoweringPlanKind::FormalArguments:
    if (!Description.ReturnAssignments.empty() || Description.Callee || Description.PreservedMask ||
        Description.CallSiteID != 0 || Description.TailDecision != TailCallKind::NotTail ||
        Description.CallFrame.EmitSetup || Description.CallFrame.ReusesIncomingArea ||
        !Description.ByValCopies.empty() || !Description.TailRejectionReason.empty()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "formal-argument plan contains call or return state");
    }
    break;
  case CallLoweringPlanKind::Return:
    if (!Description.ArgumentAssignments.empty() || Description.Callee ||
        Description.PreservedMask || Description.CallSiteID != 0 ||
        Description.TailDecision != TailCallKind::NotTail || Description.CallFrame.EmitSetup ||
        Description.CallFrame.ReusesIncomingArea || !Description.ByValCopies.empty() ||
        Description.VarArgs.IsVariadic || !Description.TailRejectionReason.empty()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "return plan contains formal-argument or call state");
    }
    break;
  case CallLoweringPlanKind::Call:
    if (!Description.Callee || !Description.Callee->isValid() || !Description.PreservedMask ||
        Description.CallSiteID == 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "call plan requires a callee, call-site ID, and preserved mask");
    }
    if (Description.TailDecision == TailCallKind::NotTail) {
      if (!Description.CallFrame.EmitSetup || Description.CallFrame.ReusesIncomingArea) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "ordinary call plan requires a private call frame");
      }
    } else if (Description.CallFrame.EmitSetup || !Description.CallFrame.ReusesIncomingArea ||
               !Description.TailRejectionReason.empty()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "tail-call plan must reuse the caller's incoming argument area");
    }
    if (Description.CallFrame.SizeBytes < Description.StackArguments.SizeBytes) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "call frame does not contain the stack-argument area");
    }
    break;
  }

  std::unordered_set<std::size_t> ByValAssignments;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ByValRanges;
  for (const ByValCopyAssignment &Copy : Description.ByValCopies) {
    if (Copy.ArgumentAssignmentIndex >= Description.ArgumentAssignments.size() ||
        !ByValAssignments.insert(Copy.ArgumentAssignmentIndex).second ||
        Copy.Source.isNoRegister() || !Copy.PointerType.isPointer() || Copy.SizeBytes == 0 ||
        Copy.StackOffsetBytes % Copy.Align.bytes() != 0 ||
        Copy.StackOffsetBytes < Description.StackArguments.SizeBytes ||
        !Description.ArgumentAssignments[Copy.ArgumentAssignmentIndex].CustomFlags.has(
            CCAssignmentFlag::ByVal) ||
        addOverflows(Copy.StackOffsetBytes, Copy.SizeBytes) ||
        Copy.StackOffsetBytes + Copy.SizeBytes > Description.CallFrame.SizeBytes) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "call-lowering byval copy is malformed or outside the call frame");
    }
    ByValRanges.emplace_back(Copy.StackOffsetBytes, Copy.StackOffsetBytes + Copy.SizeBytes);
  }
  std::ranges::sort(ByValRanges);
  for (std::size_t Index = 1; Index < ByValRanges.size(); ++Index) {
    if (ByValRanges[Index].first < ByValRanges[Index - 1].second) {
      return makeUnexpected(ErrorCode::InvalidArgument, "call-lowering byval copy ranges overlap");
    }
  }

  return CallLoweringPlan(std::move(Description));
}

Expected<void>
TargetCallLowering::lowerFormalArguments(MachineIRBuilder &Builder,
                                         const FormalArgumentRequest &Request) const {
  auto Plan = planFormalArguments(Request);
  if (!Plan)
    return std::unexpected(Plan.error());
  if (Plan->kind() != CallLoweringPlanKind::FormalArguments) {
    return makeUnexpected(ErrorCode::InternalError,
                          "target returned the wrong call-lowering plan kind for formal arguments");
  }
  return emitPlan(Builder, *Plan);
}

Expected<void> TargetCallLowering::lowerCall(MachineIRBuilder &Builder,
                                             const CallLoweringRequest &Request) const {
  auto Plan = planCall(Request);
  if (!Plan)
    return std::unexpected(Plan.error());
  if (Plan->kind() != CallLoweringPlanKind::Call) {
    return makeUnexpected(ErrorCode::InternalError,
                          "target returned the wrong call-lowering plan kind for a call");
  }
  return emitPlan(Builder, *Plan);
}

Expected<void> TargetCallLowering::lowerReturn(MachineIRBuilder &Builder,
                                               const ReturnLoweringRequest &Request) const {
  auto Plan = planReturn(Request);
  if (!Plan)
    return std::unexpected(Plan.error());
  if (Plan->kind() != CallLoweringPlanKind::Return) {
    return makeUnexpected(ErrorCode::InternalError,
                          "target returned the wrong call-lowering plan kind for a return");
  }
  return emitPlan(Builder, *Plan);
}

} // namespace rcc
