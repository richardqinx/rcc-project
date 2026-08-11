// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVCallLowering.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

constexpr std::string_view PlanTarget = "riscv64-unknown-linux-gnu";
constexpr std::uint64_t XLenBytes = 8;
constexpr std::uint64_t ABIStackAlignmentBytes = 16;
constexpr std::uint32_t AllowedDirectCallFlags =
    static_cast<std::uint32_t>(CallTargetFlag::DirectCall);

[[nodiscard]] Opcode targetOpcode(InstructionOpcode Value) noexcept {
  return Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] std::uint64_t stackObjectFlag(CallStackObjectFlag Flag) noexcept {
  return static_cast<std::uint64_t>(Flag);
}

[[nodiscard]] Expected<Alignment> alignment(std::uint64_t Bytes) {
  return Alignment::create(Bytes);
}

[[nodiscard]] Expected<std::uint64_t> alignTo(std::uint64_t Value, std::uint64_t AlignmentBytes) {
  const std::uint64_t Remainder = Value % AlignmentBytes;
  if (Remainder == 0)
    return Value;
  const std::uint64_t Increment = AlignmentBytes - Remainder;
  if (Value > std::numeric_limits<std::uint64_t>::max() - Increment) {
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V ABI stack-area size overflows uint64");
  }
  return Value + Increment;
}

[[nodiscard]] std::uint64_t valueSizeBytes(LLT Type) noexcept {
  return (static_cast<std::uint64_t>(Type.scalarBitWidth()) + 7U) / 8U;
}

[[nodiscard]] bool isSupportedFloat(LLT Type) noexcept {
  if (!Type.isFloat())
    return false;
  const auto Semantics = Type.floatSemantics();
  return Semantics &&
         (*Semantics == FloatSemantics::IEEESingle || *Semantics == FloatSemantics::IEEEDouble);
}

[[nodiscard]] Expected<PhysicalRegister> floatingRegisterView(PhysicalRegister DoubleView,
                                                              LLT Type) {
  if (!isSupportedFloat(Type)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V floating-register view requires f32 or f64");
  }
  if (Type.scalarBitWidth() == 64)
    return DoubleView;

  constexpr std::uint32_t FirstDouble = static_cast<std::uint32_t>(PhysicalRegisterID::F0_D);
  constexpr std::uint32_t LastDouble = static_cast<std::uint32_t>(PhysicalRegisterID::F31_D);
  constexpr std::uint32_t FirstSingle = static_cast<std::uint32_t>(PhysicalRegisterID::F0_F);
  const std::uint32_t Number = DoubleView.number();
  if (Number < FirstDouble || Number > LastDouble) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V ABI floating register is not an FPR64 view");
  }
  return PhysicalRegister::create(FirstSingle + Number - FirstDouble);
}

[[nodiscard]] bool requiresIntegerTransport(const ABIValue &Value) noexcept {
  return !isSupportedFloat(Value.Type) || Value.Flags.has(ABIValueFlag::ByVal) ||
         Value.Flags.has(ABIValueFlag::StructReturn) || Value.Flags.has(ABIValueFlag::Indirect);
}

[[nodiscard]] Expected<void> verifyABIValue(const ABIValue &Value, bool RequireRegister) {
  if (RequireRegister && Value.Value.isNoRegister()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V ABI value requires a source or destination register");
  }
  if (!Value.Type.isValid() || !Value.Type.isScalarValue() || Value.Type.scalarBitWidth() > 64) {
    return makeUnexpected(
        ErrorCode::Unsupported,
        "RISC-V ABI accepts explicitly classified integer, pointer, f32, and f64 scalar fragments");
  }
  if (Value.Type.isFloat() && !isSupportedFloat(Value.Type)) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V LP64 ABIs only support f32 and f64 floating fragments");
  }
  if (Value.Type.isPointer() && Value.Type.scalarBitWidth() != 64) {
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V LP64 ABIs require 64-bit pointers");
  }
  if ((Value.Type.isInteger() || Value.Type.isAnyScalar()) && Value.Type.scalarBitWidth() < 64) {
    if (Value.Extension == ABIExtensionKind::None) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "narrow RISC-V ABI integers require an explicit extension kind");
    }
  } else if (Value.Extension != ABIExtensionKind::None) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "only narrow integer ABI fragments may carry an extension kind");
  }
  if (Value.Extension >= ABIExtensionKind::Count || Value.FragmentCount == 0 ||
      Value.FragmentIndex >= Value.FragmentCount || Value.FragmentCount > 2) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V ABI fragment coordinates or extension are invalid");
  }

  const bool IsByVal = Value.Flags.has(ABIValueFlag::ByVal);
  const bool IsSRet = Value.Flags.has(ABIValueFlag::StructReturn);
  const bool IsIndirect = Value.Flags.has(ABIValueFlag::Indirect);
  const unsigned SpecialFlagCount = static_cast<unsigned>(IsByVal) + static_cast<unsigned>(IsSRet) +
                                    static_cast<unsigned>(IsIndirect);
  if (SpecialFlagCount > 1) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "byval, sret, and indirect ABI flags are mutually exclusive");
  }
  if ((IsByVal || IsSRet || IsIndirect) &&
      (!Value.Type.isPointer() || Value.FragmentCount != 1 || Value.FragmentIndex != 0 ||
       Value.FragmentOffsetBytes != 0)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "byval, sret, and indirect values require one explicit pointer fragment");
  }
  if (IsByVal) {
    if (Value.ByValSizeBytes == 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "byval arguments require a nonzero copy size");
    }
  } else if (Value.ByValSizeBytes != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "non-byval values cannot carry a byval copy size");
  }
  return {};
}

[[nodiscard]] Expected<std::size_t> verifyValueSequence(std::span<const ABIValue> Values,
                                                        bool RequireRegisters) {
  std::size_t ExpectedOriginal = 0;
  std::size_t Index = 0;
  while (Index < Values.size()) {
    const ABIValue &First = Values[Index];
    auto ValueVerification = verifyABIValue(First, RequireRegisters);
    if (!ValueVerification)
      return std::unexpected(ValueVerification.error());
    if (First.OriginalValue != ExpectedOriginal || First.FragmentIndex != 0 ||
        Index + First.FragmentCount > Values.size()) {
      return makeUnexpected(
          ErrorCode::InvalidArgument,
          "RISC-V ABI values must form contiguous, zero-based original-value groups");
    }

    std::uint64_t PreviousEnd = 0;
    for (std::size_t Fragment = 0; Fragment < First.FragmentCount; ++Fragment) {
      const ABIValue &Value = Values[Index + Fragment];
      auto FragmentVerification = verifyABIValue(Value, RequireRegisters);
      if (!FragmentVerification)
        return std::unexpected(FragmentVerification.error());
      if (Value.OriginalValue != First.OriginalValue || Value.FragmentIndex != Fragment ||
          Value.FragmentCount != First.FragmentCount ||
          Value.OriginalAlignment != First.OriginalAlignment || Value.Flags != First.Flags ||
          Value.ByValSizeBytes != First.ByValSizeBytes ||
          Value.ByValAlignment != First.ByValAlignment || Value.FragmentOffsetBytes < PreviousEnd) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "RISC-V ABI fragments of one value have inconsistent metadata");
      }
      PreviousEnd =
          static_cast<std::uint64_t>(Value.FragmentOffsetBytes) + valueSizeBytes(Value.Type);
    }
    Index += First.FragmentCount;
    ++ExpectedOriginal;
  }
  return ExpectedOriginal;
}

[[nodiscard]] CCAssignmentFlags assignmentFlags(const ABIValue &Value, bool IsVariadic,
                                                bool IsBitcast) {
  CCAssignmentFlags Result;
  if (IsVariadic)
    Result.set(CCAssignmentFlag::Variadic);
  if (Value.Flags.has(ABIValueFlag::ByVal))
    Result.set(CCAssignmentFlag::ByVal);
  if (Value.Flags.has(ABIValueFlag::StructReturn))
    Result.set(CCAssignmentFlag::StructReturn);
  if (Value.Flags.has(ABIValueFlag::Indirect))
    Result.set(CCAssignmentFlag::Indirect);
  if (Value.Flags.has(ABIValueFlag::InRegister))
    Result.set(CCAssignmentFlag::InRegister);
  if (IsBitcast)
    Result.set(CCAssignmentFlag::Bitcast);
  return Result;
}

[[nodiscard]] CCLocationKind locationKind(const ABIValue &Value, bool IsRegister) noexcept {
  if (Value.Flags.has(ABIValueFlag::Indirect))
    return CCLocationKind::Indirect;
  return IsRegister ? CCLocationKind::Register : CCLocationKind::Stack;
}

struct ArgumentAssignmentResult final {
  std::vector<CCAssignment> Assignments;
  StackArgumentArea StackArea;
  std::size_t FirstUnallocatedGPR = 0;
};

struct AssignmentState final {
  std::size_t GPR = 0;
  std::size_t FPR = 0;
  std::uint64_t StackOffset = 0;
  bool VariadicStackOnly = false;
};

[[nodiscard]] Expected<CCAssignment> registerAssignment(const ABIValue &Value,
                                                        std::size_t ValueIndex,
                                                        PhysicalRegister Location, LLT LocationType,
                                                        bool IsVariadic, bool IsBitcast) {
  auto Align =
      alignment(std::min<std::uint64_t>(Value.OriginalAlignment.bytes(), ABIStackAlignmentBytes));
  if (!Align)
    return std::unexpected(Align.error());
  return CCAssignment{
      .ValueIndex = ValueIndex,
      .OriginalValue = Value.Value,
      .OriginalValueIndex = Value.OriginalValue,
      .FragmentIndex = Value.FragmentIndex,
      .FragmentCount = Value.FragmentCount,
      .ValueType = Value.Type,
      .LocationType = LocationType,
      .LocationKind = locationKind(Value, true),
      .LocationRegister = Location,
      .StackOffsetBytes = std::nullopt,
      .LocationSizeBytes = valueSizeBytes(LocationType),
      .StackSlotSizeBytes = XLenBytes,
      .LocationAlignment = *Align,
      .Extension = Value.Extension,
      .CustomFlags = assignmentFlags(Value, IsVariadic, IsBitcast),
  };
}

[[nodiscard]] Expected<CCAssignment> stackAssignment(const ABIValue &Value, std::size_t ValueIndex,
                                                     std::uint64_t StackOffset, LLT LocationType,
                                                     bool IsVariadic,
                                                     std::uint64_t StackAlignment) {
  auto Align = alignment(StackAlignment);
  if (!Align)
    return std::unexpected(Align.error());
  return CCAssignment{
      .ValueIndex = ValueIndex,
      .OriginalValue = Value.Value,
      .OriginalValueIndex = Value.OriginalValue,
      .FragmentIndex = Value.FragmentIndex,
      .FragmentCount = Value.FragmentCount,
      .ValueType = Value.Type,
      .LocationType = LocationType,
      .LocationKind = locationKind(Value, false),
      .LocationRegister = std::nullopt,
      .StackOffsetBytes = StackOffset,
      .LocationSizeBytes = valueSizeBytes(LocationType),
      .StackSlotSizeBytes = XLenBytes,
      .LocationAlignment = *Align,
      .Extension = Value.Extension,
      .CustomFlags = assignmentFlags(Value, IsVariadic, false),
  };
}

[[nodiscard]] Expected<ArgumentAssignmentResult>
assignArguments(std::span<const ABIValue> Values, std::size_t FixedArgumentCount, bool IsVariadic,
                const CallingConventionInfo &Convention) {
  auto I64 = LLT::makeInteger(64);
  auto StackAlign = alignment(Convention.StackArgumentAlignmentBytes);
  if (!I64 || !StackAlign)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V ABI primitive type facts are invalid");

  ArgumentAssignmentResult Result;
  AssignmentState State;
  State.StackOffset = Convention.ShadowSpaceBytes;
  Result.Assignments.reserve(Values.size());

  std::size_t Index = 0;
  while (Index < Values.size()) {
    const ABIValue &First = Values[Index];
    const std::size_t GroupSize = First.FragmentCount;
    const bool VariadicArgument = IsVariadic && First.OriginalValue >= FixedArgumentCount;
    bool HasPreferredFloat = false;
    std::size_t RequiredGPRs = 0;
    std::size_t RequiredFPRs = 0;
    for (std::size_t Fragment = 0; Fragment < GroupSize; ++Fragment) {
      const ABIValue &Value = Values[Index + Fragment];
      if (!VariadicArgument && !requiresIntegerTransport(Value)) {
        HasPreferredFloat = true;
        ++RequiredFPRs;
      } else {
        ++RequiredGPRs;
      }
    }

    const bool CanUsePreferredAggregate =
        HasPreferredFloat &&
        State.GPR + RequiredGPRs <= Convention.IntegerArgumentRegisters.size() &&
        State.FPR + RequiredFPRs <= Convention.FloatingArgumentRegisters.size();
    if (CanUsePreferredAggregate) {
      for (std::size_t Fragment = 0; Fragment < GroupSize; ++Fragment) {
        const ABIValue &Value = Values[Index + Fragment];
        Expected<CCAssignment> Assignment = makeUnexpected(
            ErrorCode::InternalError, "RISC-V ABI argument assignment was not initialized");
        if (requiresIntegerTransport(Value)) {
          Assignment = registerAssignment(Value, Index + Fragment,
                                          Convention.IntegerArgumentRegisters[State.GPR++], *I64,
                                          false, false);
        } else {
          auto View =
              floatingRegisterView(Convention.FloatingArgumentRegisters[State.FPR++], Value.Type);
          if (!View)
            return std::unexpected(View.error());
          Assignment = registerAssignment(Value, Index + Fragment, *View, Value.Type, false, false);
        }
        if (!Assignment)
          return std::unexpected(Assignment.error());
        Result.Assignments.push_back(*Assignment);
      }
      Index += GroupSize;
      continue;
    }

    const bool RequiresAlignedPair =
        VariadicArgument && GroupSize == 2 && First.OriginalAlignment.bytes() >= 16;
    if (RequiresAlignedPair && State.GPR % 2 != 0 &&
        State.GPR < Convention.IntegerArgumentRegisters.size()) {
      ++State.GPR;
    }
    bool ForceEntireGroupToStack = State.VariadicStackOnly;
    if (RequiresAlignedPair && State.GPR + GroupSize > Convention.IntegerArgumentRegisters.size()) {
      ForceEntireGroupToStack = true;
      State.GPR = Convention.IntegerArgumentRegisters.size();
    }

    bool FirstStackFragment = true;
    for (std::size_t Fragment = 0; Fragment < GroupSize; ++Fragment) {
      const ABIValue &Value = Values[Index + Fragment];
      const bool HasGPR =
          !ForceEntireGroupToStack && State.GPR < Convention.IntegerArgumentRegisters.size();
      if (HasGPR) {
        const bool Bitcast = isSupportedFloat(Value.Type);
        auto Assignment = registerAssignment(Value, Index + Fragment,
                                             Convention.IntegerArgumentRegisters[State.GPR++], *I64,
                                             VariadicArgument, Bitcast);
        if (!Assignment)
          return std::unexpected(Assignment.error());
        Result.Assignments.push_back(*Assignment);
        continue;
      }

      if (Value.Flags.has(ABIValueFlag::InRegister)) {
        return makeUnexpected(ErrorCode::Unsupported,
                              "RISC-V inreg argument cannot be assigned to the stack");
      }
      std::uint64_t RequiredAlignment = XLenBytes;
      if (FirstStackFragment && ForceEntireGroupToStack) {
        RequiredAlignment = std::min<std::uint64_t>(
            std::max<std::uint64_t>(XLenBytes, First.OriginalAlignment.bytes()),
            ABIStackAlignmentBytes);
      }
      auto AlignedOffset = alignTo(State.StackOffset, RequiredAlignment);
      if (!AlignedOffset)
        return std::unexpected(AlignedOffset.error());
      State.StackOffset = *AlignedOffset;
      const LLT LocationType = isSupportedFloat(Value.Type) ? Value.Type : *I64;
      auto Assignment = stackAssignment(Value, Index + Fragment, State.StackOffset, LocationType,
                                        VariadicArgument, RequiredAlignment);
      if (!Assignment)
        return std::unexpected(Assignment.error());
      Result.Assignments.push_back(*Assignment);
      if (State.StackOffset > std::numeric_limits<std::uint64_t>::max() - XLenBytes) {
        return makeUnexpected(ErrorCode::Unsupported, "RISC-V argument area overflows uint64");
      }
      State.StackOffset += XLenBytes;
      FirstStackFragment = false;
      if (VariadicArgument)
        State.VariadicStackOnly = true;
    }
    Index += GroupSize;
  }

  auto FinalSize = alignTo(State.StackOffset, Convention.StackArgumentAlignmentBytes);
  if (!FinalSize)
    return std::unexpected(FinalSize.error());
  Result.StackArea = StackArgumentArea{*FinalSize, Convention.ShadowSpaceBytes, *StackAlign};
  Result.FirstUnallocatedGPR = State.GPR;
  return Result;
}

[[nodiscard]] Expected<std::vector<CCAssignment>>
assignReturns(std::span<const ABIValue> Values, bool RequireRegisters,
              const CallingConventionInfo &Convention) {
  auto Sequence = verifyValueSequence(Values, RequireRegisters);
  if (!Sequence)
    return std::unexpected(Sequence.error());
  if (Values.size() > 2) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V direct returns are limited to two explicit fragments");
  }
  auto I64 = LLT::makeInteger(64);
  if (!I64)
    return std::unexpected(I64.error());

  std::vector<CCAssignment> Result;
  Result.reserve(Values.size());
  std::size_t GPR = 0;
  std::size_t FPR = 0;
  for (std::size_t Index = 0; Index < Values.size(); ++Index) {
    const ABIValue &Value = Values[Index];
    if (Value.Flags.rawValue() != 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "return fragments cannot carry argument-only ABI flags");
    }
    const bool UseFloatingRegister =
        isSupportedFloat(Value.Type) && FPR < Convention.FloatingReturnRegisters.size();
    Expected<CCAssignment> Assignment = makeUnexpected(
        ErrorCode::Unsupported, "RISC-V calling convention has no return register available");
    if (UseFloatingRegister) {
      auto View = floatingRegisterView(Convention.FloatingReturnRegisters[FPR++], Value.Type);
      if (!View)
        return std::unexpected(View.error());
      Assignment = registerAssignment(Value, Index, *View, Value.Type, false, false);
    } else if (GPR < Convention.IntegerReturnRegisters.size()) {
      Assignment = registerAssignment(Value, Index, Convention.IntegerReturnRegisters[GPR++], *I64,
                                      false, isSupportedFloat(Value.Type));
    }
    if (!Assignment)
      return std::unexpected(Assignment.error());
    Result.push_back(*Assignment);
  }
  return Result;
}

[[nodiscard]] bool hasStructReturn(std::span<const ABIValue> Arguments) noexcept {
  return std::ranges::any_of(
      Arguments, [](const ABIValue &Value) { return Value.Flags.has(ABIValueFlag::StructReturn); });
}

[[nodiscard]] bool equivalentReturnLocations(std::span<const CCAssignment> Left,
                                             std::span<const CCAssignment> Right) noexcept {
  if (Left.size() != Right.size())
    return false;
  for (std::size_t Index = 0; Index < Left.size(); ++Index) {
    if (Left[Index].ValueType != Right[Index].ValueType ||
        Left[Index].LocationType != Right[Index].LocationType ||
        Left[Index].LocationKind != Right[Index].LocationKind ||
        Left[Index].LocationRegister != Right[Index].LocationRegister ||
        Left[Index].Extension != Right[Index].Extension ||
        Left[Index].CustomFlags != Right[Index].CustomFlags) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] Expected<std::string>
tailCallRejection(const CallLoweringRequest &Request, const ArgumentAssignmentResult &Arguments,
                  std::span<const CCAssignment> Results,
                  std::span<const ByValCopyAssignment> ByValCopies,
                  const CallingConventionInfo &Convention) {
  if (!Convention.SupportsTailCalls)
    return std::string("the calling convention does not support tail calls");
  if (!Request.TailContext)
    return std::string("tail-call context is missing");
  const TailCallContext &Context = *Request.TailContext;
  if (Context.CallerConvention != Request.Convention)
    return std::string("caller and callee calling conventions differ");
  if (Context.CallerIsVariadic != Request.IsVariadic ||
      (Request.IsVariadic && Context.CallerFixedArgumentCount != Request.FixedArgumentCount)) {
    return std::string("caller and callee variadic signatures differ");
  }
  if (Context.CallerHasDynamicStackAllocation)
    return std::string("the caller has a dynamic stack allocation");
  if (!ByValCopies.empty())
    return std::string("tail calls cannot create byval copies");
  if (Context.CallerUsesStructReturn != hasStructReturn(Request.Arguments))
    return std::string("caller and callee sret conventions differ");
  if (Arguments.StackArea.SizeBytes > Context.ReusableIncomingArgumentBytes)
    return std::string("outgoing stack arguments exceed the reusable incoming area");
  if (std::ranges::any_of(Request.Results,
                          [](const ABIValue &Value) { return !Value.Value.isNoRegister(); })) {
    return std::string("tail-call result signatures must not request local result copies");
  }
  auto CallerReturns = assignReturns(Context.CallerReturns, false, Convention);
  if (!CallerReturns)
    return std::unexpected(CallerReturns.error());
  if (!equivalentReturnLocations(Results, *CallerReturns))
    return std::string("caller and callee return locations differ");
  return std::string{};
}

[[nodiscard]] Expected<const CallingConventionInfo *>
riscvConvention(CallingConventionID Convention) {
  CallingConvention TargetConvention = CallingConvention::None;
  if (Convention == callingConventionID(CallingConvention::LP64D))
    TargetConvention = CallingConvention::LP64D;
  else if (Convention == callingConventionID(CallingConvention::LP64))
    TargetConvention = CallingConvention::LP64;
  else
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V backend only supports LP64 and LP64D");
  const CallingConventionInfo *Info = callingConvention(TargetConvention);
  if (Info == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V calling-convention target facts are unavailable");
  return Info;
}

class RISCVCallLowering final : public TargetCallLowering {
public:
  [[nodiscard]] Expected<CallLoweringPlan>
  planFormalArguments(const FormalArgumentRequest &Request) const override;
  [[nodiscard]] Expected<CallLoweringPlan>
  planCall(const CallLoweringRequest &Request) const override;
  [[nodiscard]] Expected<CallLoweringPlan>
  planReturn(const ReturnLoweringRequest &Request) const override;
  [[nodiscard]] Expected<void> emitPlan(MachineIRBuilder &Builder,
                                        const CallLoweringPlan &Plan) const override;
};

Expected<CallLoweringPlan>
RISCVCallLowering::planFormalArguments(const FormalArgumentRequest &Request) const {
  auto Convention = riscvConvention(Request.Convention);
  if (!Convention)
    return std::unexpected(Convention.error());
  auto OriginalCount = verifyValueSequence(Request.Arguments, true);
  if (!OriginalCount)
    return std::unexpected(OriginalCount.error());
  if (Request.FixedArgumentCount != *OriginalCount) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "formal-argument request must identify every declared argument as fixed");
  }
  if (!Request.IsVariadic && Request.FixedArgumentCount != *OriginalCount) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "non-variadic formal request has an invalid fixed-argument boundary");
  }
  if (hasStructReturn(Request.Arguments) &&
      (Request.Arguments.empty() || Request.Arguments.front().OriginalValue != 0 ||
       !Request.Arguments.front().Flags.has(ABIValueFlag::StructReturn))) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V sret must be the first formal argument");
  }

  auto Assignments = assignArguments(Request.Arguments, Request.FixedArgumentCount,
                                     Request.IsVariadic, **Convention);
  if (!Assignments)
    return std::unexpected(Assignments.error());

  VarArgLoweringState VarArgs;
  if (Request.IsVariadic) {
    VarArgs.IsVariadic = true;
    VarArgs.FixedArgumentCount = Request.FixedArgumentCount;
    VarArgs.FirstUnallocatedIntegerRegister = Assignments->FirstUnallocatedGPR;
    VarArgs.OverflowAreaOffsetBytes = Assignments->StackArea.SizeBytes;
    const std::size_t First = Assignments->FirstUnallocatedGPR;
    const std::size_t RegisterCount = (*Convention)->IntegerArgumentRegisters.size();
    VarArgs.RegisterSaveAreaSizeBytes = (RegisterCount - First) * XLenBytes;
    VarArgs.RegisterSaveAreaOffsetBytes =
        VarArgs.RegisterSaveAreaSizeBytes == 0
            ? static_cast<std::int64_t>(VarArgs.OverflowAreaOffsetBytes)
            : -static_cast<std::int64_t>(VarArgs.RegisterSaveAreaSizeBytes);
    if (VarArgs.RegisterSaveAreaSizeBytes != 0 && First % 2 != 0)
      VarArgs.RegisterSavePaddingBytes = XLenBytes;
    for (std::size_t Index = First; Index < RegisterCount; ++Index) {
      VarArgs.RegistersToSave.push_back(
          {(*Convention)->IntegerArgumentRegisters[Index], (Index - First) * XLenBytes});
    }
  }

  CallLoweringPlanDesc Plan;
  Plan.Kind = CallLoweringPlanKind::FormalArguments;
  Plan.Target = PlanTarget;
  Plan.Convention = Request.Convention;
  Plan.ArgumentAssignments = std::move(Assignments->Assignments);
  Plan.StackArguments = Assignments->StackArea;
  Plan.VarArgs = std::move(VarArgs);
  return CallLoweringPlan::create(std::move(Plan));
}

Expected<CallLoweringPlan> RISCVCallLowering::planCall(const CallLoweringRequest &Request) const {
  auto Convention = riscvConvention(Request.Convention);
  if (!Convention)
    return std::unexpected(Convention.error());
  auto OriginalCount = verifyValueSequence(Request.Arguments, true);
  if (!OriginalCount)
    return std::unexpected(OriginalCount.error());
  if ((!Request.IsVariadic && Request.FixedArgumentCount != *OriginalCount) ||
      (Request.IsVariadic && Request.FixedArgumentCount > *OriginalCount)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call request has an invalid fixed/variadic argument boundary");
  }
  if (!Request.Callee.isValid())
    return makeUnexpected(ErrorCode::InvalidArgument, "call request requires a valid callee");
  if (hasStructReturn(Request.Arguments) &&
      (Request.Arguments.empty() ||
       !Request.Arguments.front().Flags.has(ABIValueFlag::StructReturn))) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V sret must be the first call argument");
  }
  if (hasStructReturn(Request.Arguments) && !Request.Results.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V sret calls cannot also request direct return values");
  }

  MachineCallTarget EffectiveCallee = Request.Callee;
  switch (Request.Callee.kind()) {
  case MachineCallTargetKind::GlobalSymbol: {
    if ((Request.Callee.targetFlags() & ~AllowedDirectCallFlags) != 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "direct RISC-V call uses unknown target flags");
    }
    auto Callee = MachineCallTarget::globalSymbol(
        std::string(Request.Callee.symbolName()),
        Request.Callee.targetFlags() | static_cast<std::uint32_t>(CallTargetFlag::DirectCall));
    if (!Callee)
      return std::unexpected(Callee.error());
    EffectiveCallee = std::move(*Callee);
    break;
  }
  case MachineCallTargetKind::ExternalSymbol: {
    if ((Request.Callee.targetFlags() & ~AllowedDirectCallFlags) != 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "direct RISC-V call uses unknown target flags");
    }
    auto Callee = MachineCallTarget::externalSymbol(
        std::string(Request.Callee.symbolName()),
        Request.Callee.targetFlags() | static_cast<std::uint32_t>(CallTargetFlag::DirectCall));
    if (!Callee)
      return std::unexpected(Callee.error());
    EffectiveCallee = std::move(*Callee);
    break;
  }
  case MachineCallTargetKind::IndirectRegister:
    if (Request.Callee.targetFlags() != 0) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "indirect RISC-V calls cannot carry direct-call target flags");
    }
    break;
  case MachineCallTargetKind::Invalid:
    return makeUnexpected(ErrorCode::InvalidArgument, "call target is invalid");
  }

  const bool RequireResultRegisters = Request.TailKind == TailCallKind::NotTail;
  auto ResultSequence = verifyValueSequence(Request.Results, RequireResultRegisters);
  if (!ResultSequence)
    return std::unexpected(ResultSequence.error());
  auto Arguments = assignArguments(Request.Arguments, Request.FixedArgumentCount,
                                   Request.IsVariadic, **Convention);
  if (!Arguments)
    return std::unexpected(Arguments.error());
  auto Results = assignReturns(Request.Results, RequireResultRegisters, **Convention);
  if (!Results)
    return std::unexpected(Results.error());

  auto Align16 = alignment(ABIStackAlignmentBytes);
  if (!Align16)
    return std::unexpected(Align16.error());
  std::uint64_t CallFrameCursor = Arguments->StackArea.SizeBytes;
  std::vector<ByValCopyAssignment> ByValCopies;
  for (std::size_t Index = 0; Index < Arguments->Assignments.size(); ++Index) {
    const CCAssignment &Assignment = Arguments->Assignments[Index];
    if (!Assignment.CustomFlags.has(CCAssignmentFlag::ByVal))
      continue;
    const ABIValue &Value = Request.Arguments[Assignment.ValueIndex];
    const std::uint64_t CopyAlignment =
        std::min<std::uint64_t>(Value.ByValAlignment.bytes(), ABIStackAlignmentBytes);
    auto CopyOffset = alignTo(CallFrameCursor, CopyAlignment);
    if (!CopyOffset)
      return std::unexpected(CopyOffset.error());
    if (*CopyOffset > std::numeric_limits<std::uint64_t>::max() - Value.ByValSizeBytes) {
      return makeUnexpected(ErrorCode::Unsupported, "RISC-V byval area overflows uint64");
    }
    auto CopyAlign = alignment(CopyAlignment);
    if (!CopyAlign)
      return std::unexpected(CopyAlign.error());
    ByValCopies.push_back(
        {Index, Value.Value, Value.Type, *CopyOffset, Value.ByValSizeBytes, *CopyAlign});
    CallFrameCursor = *CopyOffset + Value.ByValSizeBytes;
  }
  auto CallFrameSize = alignTo(CallFrameCursor, ABIStackAlignmentBytes);
  if (!CallFrameSize)
    return std::unexpected(CallFrameSize.error());

  TailCallKind Decision = TailCallKind::NotTail;
  std::string RejectionReason;
  if (Request.TailKind != TailCallKind::NotTail) {
    auto Rejection = tailCallRejection(Request, *Arguments, *Results, ByValCopies, **Convention);
    if (!Rejection)
      return std::unexpected(Rejection.error());
    RejectionReason = std::move(*Rejection);
    if (RejectionReason.empty()) {
      Decision = Request.TailKind;
    } else if (Request.TailKind == TailCallKind::MustTail) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "musttail call is not ABI-compatible: " + RejectionReason);
    } else {
      auto NormalResults = verifyValueSequence(Request.Results, true);
      if (!NormalResults)
        return std::unexpected(NormalResults.error());
    }
  }

  CallLoweringPlanDesc Plan;
  Plan.Kind = CallLoweringPlanKind::Call;
  Plan.Target = PlanTarget;
  Plan.Convention = Request.Convention;
  Plan.ArgumentAssignments = std::move(Arguments->Assignments);
  Plan.ReturnAssignments = std::move(*Results);
  Plan.StackArguments = Arguments->StackArea;
  Plan.CallFrame = CallFrameRequirements{
      Decision == TailCallKind::NotTail ? *CallFrameSize : Arguments->StackArea.SizeBytes, *Align16,
      Decision == TailCallKind::NotTail, Decision == TailCallKind::NotTail,
      Decision != TailCallKind::NotTail};
  if (Request.IsVariadic) {
    Plan.VarArgs.IsVariadic = true;
    Plan.VarArgs.FixedArgumentCount = Request.FixedArgumentCount;
  }
  Plan.ByValCopies = std::move(ByValCopies);
  Plan.PreservedMask = createCallPreservedMask((*Convention)->ID);
  if (!Plan.PreservedMask)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V call-preserved mask is unavailable");
  Plan.Callee = std::move(EffectiveCallee);
  Plan.TailDecision = Decision;
  Plan.TailRejectionReason = std::move(RejectionReason);
  return CallLoweringPlan::create(std::move(Plan));
}

Expected<CallLoweringPlan>
RISCVCallLowering::planReturn(const ReturnLoweringRequest &Request) const {
  auto Convention = riscvConvention(Request.Convention);
  if (!Convention)
    return std::unexpected(Convention.error());
  auto Values = assignReturns(Request.Values, true, **Convention);
  if (!Values)
    return std::unexpected(Values.error());

  CallLoweringPlanDesc Plan;
  Plan.Kind = CallLoweringPlanKind::Return;
  Plan.Target = PlanTarget;
  Plan.Convention = Request.Convention;
  Plan.ReturnAssignments = std::move(*Values);
  return CallLoweringPlan::create(std::move(Plan));
}

[[nodiscard]] CallValueTransform outgoingTransform(const CCAssignment &Assignment) noexcept {
  if (Assignment.CustomFlags.has(CCAssignmentFlag::Bitcast))
    return CallValueTransform::Bitcast;
  switch (Assignment.Extension) {
  case ABIExtensionKind::None:
    return CallValueTransform::Full;
  case ABIExtensionKind::AnyExtend:
    return CallValueTransform::AnyExtend;
  case ABIExtensionKind::SignExtend:
    return CallValueTransform::SignExtend;
  case ABIExtensionKind::ZeroExtend:
    return CallValueTransform::ZeroExtend;
  case ABIExtensionKind::Count:
    break;
  }
  return CallValueTransform::Full;
}

[[nodiscard]] CallValueTransform incomingTransform(const CCAssignment &Assignment) noexcept {
  if (Assignment.CustomFlags.has(CCAssignmentFlag::Bitcast))
    return CallValueTransform::Bitcast;
  if (Assignment.ValueType.scalarBitWidth() < Assignment.LocationType.scalarBitWidth())
    return CallValueTransform::Truncate;
  return CallValueTransform::Full;
}

[[nodiscard]] Expected<void> addUnsignedImmediate(MachineInstrBuilder &Builder,
                                                  std::uint64_t Value) {
  auto Immediate = IntegerImmediate::fromUnsigned(64, Value);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  return Builder.addIntegerImmediate(std::move(*Immediate));
}

[[nodiscard]] Expected<const MachineMemOperand *>
createMemoryOperand(MachineFunction &Function, MemoryAccessFlags Accesses, std::uint64_t SizeBytes,
                    Alignment Align, MemoryBase Base, std::int64_t Offset = 0) {
  auto Size = MemorySize::fixed(SizeBytes);
  if (!Size)
    return std::unexpected(Size.error());
  MachineMemOperandDesc Description;
  Description.Accesses = Accesses;
  Description.Size = *Size;
  Description.Align = Align;
  Description.Base = std::move(Base);
  Description.Offset = Offset;
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] Expected<void> emitCCCopy(MachineIRBuilder &Builder, Register Destination,
                                        Register Source, CallValueTransform Transform) {
  const Opcode CopyOpcode = Transform == CallValueTransform::Full
                                ? opcode(GenericOpcode::COPY)
                                : targetOpcode(InstructionOpcode::PseudoCC_COPY);
  auto Instruction = Builder.buildInstr(CopyOpcode);
  if (!Instruction)
    return std::unexpected(Instruction.error());
  auto Def = Instruction->addDef(Destination);
  if (!Def)
    return std::unexpected(Def.error());
  auto Use = Instruction->addUse(Source);
  if (!Use)
    return std::unexpected(Use.error());
  if (Transform != CallValueTransform::Full) {
    auto Action = Instruction->addImmediate(static_cast<std::int64_t>(Transform));
    if (!Action)
      return std::unexpected(Action.error());
  }
  auto Built = Instruction->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitStackLoad(MachineIRBuilder &Builder,
                                           const CCAssignment &Assignment, FrameIndex Object,
                                           std::uint64_t ObjectOffset,
                                           const MachineMemOperand &Memory) {
  auto Instruction = Builder.buildInstr(targetOpcode(InstructionOpcode::PseudoLOAD_STACK_ARG));
  if (!Instruction)
    return std::unexpected(Instruction.error());
  auto Def = Instruction->addDef(Assignment.OriginalValue);
  if (!Def)
    return std::unexpected(Def.error());
  auto Frame = Instruction->addFrameIndex(Object);
  if (!Frame)
    return std::unexpected(Frame.error());
  auto Offset = addUnsignedImmediate(*Instruction, ObjectOffset);
  if (!Offset)
    return std::unexpected(Offset.error());
  auto Transform =
      Instruction->addImmediate(static_cast<std::int64_t>(incomingTransform(Assignment)));
  if (!Transform)
    return std::unexpected(Transform.error());
  auto Mem = Instruction->addMemOperand(Memory);
  if (!Mem)
    return std::unexpected(Mem.error());
  auto Built = Instruction->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitStackStore(MachineIRBuilder &Builder, Register Source,
                                            FrameIndex Object, std::uint64_t ObjectOffset,
                                            const MachineMemOperand &Memory,
                                            CallValueTransform Transform) {
  auto Instruction = Builder.buildInstr(targetOpcode(InstructionOpcode::PseudoSTORE_STACK_ARG));
  if (!Instruction)
    return std::unexpected(Instruction.error());
  auto Use = Instruction->addUse(Source);
  if (!Use)
    return std::unexpected(Use.error());
  auto Frame = Instruction->addFrameIndex(Object);
  if (!Frame)
    return std::unexpected(Frame.error());
  auto Offset = addUnsignedImmediate(*Instruction, ObjectOffset);
  if (!Offset)
    return std::unexpected(Offset.error());
  auto Action = Instruction->addImmediate(static_cast<std::int64_t>(Transform));
  if (!Action)
    return std::unexpected(Action.error());
  auto Mem = Instruction->addMemOperand(Memory);
  if (!Mem)
    return std::unexpected(Mem.error());
  auto Built = Instruction->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> ensureFunctionLiveIn(MachineRegisterInfo &Registers,
                                                  PhysicalRegister Reg,
                                                  std::optional<Register> InitialValue) {
  const auto Existing =
      std::ranges::find(Registers.liveIns(), Reg, &PhysicalRegisterLiveIn::PhysReg);
  if (Existing == Registers.liveIns().end())
    return Registers.addLiveIn(Reg, InitialValue);
  if (InitialValue && Existing->InitialVirtualRegister &&
      Existing->InitialVirtualRegister != InitialValue) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "physical formal register already maps to another virtual live-in");
  }
  return {};
}

[[nodiscard]] Expected<void> ensureBlockLiveIn(MachineBasicBlock &Block, PhysicalRegister Reg) {
  return Block.addLiveIn(Reg, LaneMask::all());
}

[[nodiscard]] Expected<void> verifyPlanRegisters(const CallLoweringPlan &Plan,
                                                 const MachineFunction &Function,
                                                 const TargetDescription &Target) {
  const MachineRegisterInfo &Registers = Function.registerInfo();
  const auto Verify = [&Registers, &Target](const CCAssignment &Assignment) -> Expected<void> {
    if (Assignment.LocationRegister &&
        !Target.registers().isKnownPhysicalRegister(*Assignment.LocationRegister)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "call-lowering plan names an unknown ABI location register");
    }
    const Register Reg = Assignment.OriginalValue;
    if (Reg.isNoRegister())
      return {};
    if (Reg.isPhysical()) {
      if (!Target.registers().isKnownPhysicalRegister(*Reg.physicalRegister())) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "call-lowering plan names an unknown physical value register");
      }
      return {};
    }
    auto Info = Registers.virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (!Info->get().Type || *Info->get().Type != Assignment.ValueType) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "call-lowering plan register LLT differs from MachineRegisterInfo");
    }
    return {};
  };
  for (const CCAssignment &Assignment : Plan.argumentAssignments()) {
    auto Result = Verify(Assignment);
    if (!Result)
      return std::unexpected(Result.error());
  }
  for (const CCAssignment &Assignment : Plan.returnAssignments()) {
    auto Result = Verify(Assignment);
    if (!Result)
      return std::unexpected(Result.error());
  }
  if (const MachineCallTarget *Callee = Plan.callee();
      Callee != nullptr && Callee->kind() == MachineCallTargetKind::IndirectRegister) {
    const Register CalleeReg = Callee->indirectRegister();
    if (CalleeReg.isPhysical()) {
      const std::optional<PhysicalRegister> Physical = CalleeReg.physicalRegister();
      if (!Physical || !Target.registers().isKnownPhysicalRegister(*Physical)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "indirect RISC-V callee names an unknown physical register");
      }
    } else {
      auto Info = Registers.virtualRegisterInfo(CalleeReg);
      if (!Info)
        return std::unexpected(Info.error());
      const std::optional<LLT> Type = Info->get().Type;
      if (!Type || ((!Type->isPointer() && !Type->isInteger()) || Type->scalarBitWidth() != 64)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "indirect RISC-V callee requires a 64-bit pointer or integer LLT");
      }
    }
  }
  return {};
}

[[nodiscard]] Expected<void> verifyRequiredOpcodes(const MachineIRBuilder &Builder) {
  static constexpr std::array Required{
      InstructionOpcode::PseudoCC_COPY,
      InstructionOpcode::PseudoLOAD_STACK_ARG,
      InstructionOpcode::PseudoSTORE_STACK_ARG,
      InstructionOpcode::PseudoBYVAL_COPY,
      InstructionOpcode::ADJCALLSTACKDOWN,
      InstructionOpcode::ADJCALLSTACKUP,
      InstructionOpcode::PseudoCALL,
      InstructionOpcode::PseudoTAIL,
      InstructionOpcode::PseudoRET,
  };
  for (const InstructionOpcode OpcodeValue : Required) {
    if (Builder.target().instructionDescriptor(targetOpcode(OpcodeValue)) == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "RISC-V target description lacks a call-lowering pseudo");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> verifyFormalLiveIns(const CallLoweringPlan &Plan,
                                                 const MachineFunction &Function) {
  if (Plan.kind() != CallLoweringPlanKind::FormalArguments)
    return {};

  const auto LiveIns = Function.registerInfo().liveIns();
  for (const CCAssignment &Assignment : Plan.argumentAssignments()) {
    if (!Assignment.LocationRegister || !Assignment.OriginalValue.isVirtual())
      continue;
    const auto Existing =
        std::ranges::find(LiveIns, *Assignment.LocationRegister, &PhysicalRegisterLiveIn::PhysReg);
    if (Existing != LiveIns.end() && Existing->InitialVirtualRegister &&
        *Existing->InitialVirtualRegister != Assignment.OriginalValue) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical formal register already maps to another virtual live-in");
    }
  }
  return {};
}

[[nodiscard]] Expected<void> preflightPlan(MachineIRBuilder &Builder,
                                           const CallLoweringPlan &Plan) {
  auto Convention = riscvConvention(Plan.convention());
  if (!Convention)
    return std::unexpected(Convention.error());
  if (Plan.target() != PlanTarget) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "call-lowering plan belongs to another target or ABI");
  }
  auto InsertPoint = Builder.verifyInsertPoint();
  if (!InsertPoint)
    return std::unexpected(InsertPoint.error());
  auto Opcodes = verifyRequiredOpcodes(Builder);
  if (!Opcodes)
    return std::unexpected(Opcodes.error());
  auto Registers = verifyPlanRegisters(Plan, Builder.function(), Builder.target());
  if (!Registers)
    return std::unexpected(Registers.error());
  auto LiveIns = verifyFormalLiveIns(Plan, Builder.function());
  if (!LiveIns)
    return std::unexpected(LiveIns.error());
  for (const CCAssignment &Assignment : Plan.argumentAssignments()) {
    if (Assignment.StackOffsetBytes &&
        *Assignment.StackOffsetBytes >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V stack assignment offset exceeds the frame-index range");
    }
  }
  for (const ByValCopyAssignment &Copy : Plan.byValCopies()) {
    if (Copy.StackOffsetBytes >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "RISC-V byval offset exceeds the frame-index range");
    }
  }
  if (Plan.kind() != CallLoweringPlanKind::Return && Builder.function().frameInfo().isFrozen()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "call lowering cannot modify a frozen machine frame");
  }
  if (Plan.kind() == CallLoweringPlanKind::FormalArguments &&
      (Builder.function().firstBlock() == nullptr ||
       Builder.insertionBlock() != Builder.function().firstBlock())) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "formal arguments must be lowered in the entry machine block");
  }
  return {};
}

[[nodiscard]] Expected<void> updateMaximumFrameAlignment(MachineFrameState &State,
                                                         Alignment Required) {
  if (!State.MaxAlignment || State.MaxAlignment->bytes() < Required.bytes())
    State.MaxAlignment = Required;
  return {};
}

[[nodiscard]] Expected<void> emitFormalArguments(MachineIRBuilder &Builder,
                                                 const CallLoweringPlan &Plan) {
  MachineFunction &Function = Builder.function();
  MachineBasicBlock &Entry = *Function.firstBlock();
  MachineRegisterInfo &Registers = Function.registerInfo();
  MachineFrameInfo &Frame = Function.frameInfo();
  Alignment RequiredFrameAlignment;

  for (const CCAssignment &Assignment : Plan.argumentAssignments()) {
    if (Assignment.LocationRegister) {
      const PhysicalRegister Location = *Assignment.LocationRegister;
      const std::optional<Register> Initial =
          Assignment.OriginalValue.isVirtual() ? std::optional<Register>(Assignment.OriginalValue)
                                               : std::nullopt;
      auto FunctionLiveIn = ensureFunctionLiveIn(Registers, Location, Initial);
      if (!FunctionLiveIn)
        return std::unexpected(FunctionLiveIn.error());
      auto BlockLiveIn = ensureBlockLiveIn(Entry, Location);
      if (!BlockLiveIn)
        return std::unexpected(BlockLiveIn.error());
      auto Copy = emitCCCopy(Builder, Assignment.OriginalValue, Register::fromPhysical(Location),
                             incomingTransform(Assignment));
      if (!Copy)
        return std::unexpected(Copy.error());
      continue;
    }

    if (!Assignment.StackOffsetBytes) {
      return makeUnexpected(ErrorCode::InternalError,
                            "verified formal stack assignment has no stack offset");
    }
    StackObjectDesc Object;
    Object.Kind = StackObjectKind::FixedABI;
    Object.Size = Assignment.StackSlotSizeBytes;
    Object.Align = Assignment.LocationAlignment;
    Object.Offset = static_cast<std::int64_t>(*Assignment.StackOffsetBytes);
    Object.IsImmutable = true;
    Object.TargetFlags = stackObjectFlag(CallStackObjectFlag::IncomingArgument);
    auto FrameObject = Frame.createObject(std::move(Object));
    if (!FrameObject)
      return std::unexpected(FrameObject.error());
    auto Memory =
        createMemoryOperand(Function, {MemoryAccessFlag::Load}, Assignment.LocationSizeBytes,
                            Assignment.LocationAlignment, MemoryBase::frameIndex(*FrameObject));
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Load = emitStackLoad(Builder, Assignment, *FrameObject, 0, **Memory);
    if (!Load)
      return std::unexpected(Load.error());
    if (RequiredFrameAlignment.bytes() < Assignment.LocationAlignment.bytes())
      RequiredFrameAlignment = Assignment.LocationAlignment;
  }

  const VarArgLoweringState &VarArgs = Plan.varArgs();
  if (!VarArgs.IsVariadic) {
    if (RequiredFrameAlignment.bytes() == 1)
      return {};
    MachineFrameState State = Frame.state();
    auto AlignmentUpdate = updateMaximumFrameAlignment(State, RequiredFrameAlignment);
    if (!AlignmentUpdate)
      return std::unexpected(AlignmentUpdate.error());
    return Frame.setState(std::move(State));
  }

  auto Align8 = alignment(XLenBytes);
  auto Align16 = alignment(ABIStackAlignmentBytes);
  if (!Align8 || !Align16)
    return makeUnexpected(ErrorCode::InternalError, "RISC-V ABI primitive type facts are invalid");

  FrameIndex SaveObject;
  if (VarArgs.RegisterSaveAreaSizeBytes == 0) {
    StackObjectDesc Object;
    Object.Kind = StackObjectKind::FixedABI;
    Object.Size = XLenBytes;
    Object.Align = *Align8;
    Object.Offset = VarArgs.RegisterSaveAreaOffsetBytes;
    Object.IsImmutable = true;
    Object.TargetFlags = stackObjectFlag(CallStackObjectFlag::VarArgSaveArea);
    auto Created = Frame.createObject(std::move(Object));
    if (!Created)
      return std::unexpected(Created.error());
    SaveObject = *Created;
  } else {
    StackObjectDesc Object;
    Object.Kind = StackObjectKind::FixedABI;
    Object.Size = VarArgs.RegisterSaveAreaSizeBytes;
    Object.Align = VarArgs.FirstUnallocatedIntegerRegister % 2 == 0 ? *Align16 : *Align8;
    Object.Offset = VarArgs.RegisterSaveAreaOffsetBytes;
    Object.TargetFlags = stackObjectFlag(CallStackObjectFlag::VarArgSaveArea);
    auto Created = Frame.createObject(std::move(Object));
    if (!Created)
      return std::unexpected(Created.error());
    SaveObject = *Created;

    if (VarArgs.RegisterSavePaddingBytes != 0) {
      StackObjectDesc Padding;
      Padding.Kind = StackObjectKind::FixedABI;
      Padding.Size = VarArgs.RegisterSavePaddingBytes;
      Padding.Align = *Align8;
      Padding.Offset = VarArgs.RegisterSaveAreaOffsetBytes -
                       static_cast<std::int64_t>(VarArgs.RegisterSavePaddingBytes);
      Padding.IsImmutable = true;
      Padding.TargetFlags = stackObjectFlag(CallStackObjectFlag::VarArgAlignmentPadding);
      auto CreatedPadding = Frame.createObject(std::move(Padding));
      if (!CreatedPadding)
        return std::unexpected(CreatedPadding.error());
    }
  }

  for (const VarArgRegisterSave Save : VarArgs.RegistersToSave) {
    auto FunctionLiveIn = ensureFunctionLiveIn(Registers, Save.Reg, std::nullopt);
    if (!FunctionLiveIn)
      return std::unexpected(FunctionLiveIn.error());
    auto BlockLiveIn = ensureBlockLiveIn(Entry, Save.Reg);
    if (!BlockLiveIn)
      return std::unexpected(BlockLiveIn.error());
    auto Memory = createMemoryOperand(Function, {MemoryAccessFlag::Store}, XLenBytes, *Align8,
                                      MemoryBase::frameIndex(SaveObject),
                                      static_cast<std::int64_t>(Save.OffsetBytes));
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Store = emitStackStore(Builder, Register::fromPhysical(Save.Reg), SaveObject,
                                Save.OffsetBytes, **Memory, CallValueTransform::Full);
    if (!Store)
      return std::unexpected(Store.error());
  }

  MachineFrameState State = Frame.state();
  const Alignment VarArgAlignment =
      RequiredFrameAlignment.bytes() > Align16->bytes() ? RequiredFrameAlignment : *Align16;
  auto AlignmentUpdate = updateMaximumFrameAlignment(State, VarArgAlignment);
  if (!AlignmentUpdate)
    return std::unexpected(AlignmentUpdate.error());
  return Frame.setState(std::move(State));
}

[[nodiscard]] Expected<void> emitCallFrameAdjustment(MachineIRBuilder &Builder,
                                                     InstructionOpcode OpcodeValue,
                                                     const CallFrameRequirements &Frame) {
  if (Frame.SizeBytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V call-frame size does not fit a MIR immediate");
  }
  auto Instruction = Builder.buildInstr(targetOpcode(OpcodeValue));
  if (!Instruction)
    return std::unexpected(Instruction.error());
  auto Size = Instruction->addImmediate(static_cast<std::int64_t>(Frame.SizeBytes));
  if (!Size)
    return std::unexpected(Size.error());
  auto PreparedBytes = Instruction->addImmediate(0);
  if (!PreparedBytes)
    return std::unexpected(PreparedBytes.error());
  auto Built = Instruction->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<std::unordered_map<std::size_t, Register>>
emitByValCopies(MachineIRBuilder &Builder, const CallLoweringPlan &Plan) {
  MachineFunction &Function = Builder.function();
  MachineFrameInfo &Frame = Function.frameInfo();
  std::unordered_map<std::size_t, Register> Replacements;
  for (const ByValCopyAssignment Copy : Plan.byValCopies()) {
    StackObjectDesc Object;
    Object.Kind = StackObjectKind::Target;
    Object.Size = Copy.SizeBytes;
    Object.Align = Copy.Align;
    Object.LocalOffset = static_cast<std::int64_t>(Copy.StackOffsetBytes);
    Object.TargetFlags = stackObjectFlag(CallStackObjectFlag::ByValCopy);
    auto FrameObject = Frame.createObject(std::move(Object));
    if (!FrameObject)
      return std::unexpected(FrameObject.error());
    auto Temporary = Function.registerInfo().createVirtualRegister(Copy.PointerType);
    if (!Temporary)
      return std::unexpected(Temporary.error());

    auto SourceMemory = createMemoryOperand(Function, {MemoryAccessFlag::Load}, Copy.SizeBytes,
                                            Copy.Align, MemoryBase{});
    if (!SourceMemory)
      return std::unexpected(SourceMemory.error());
    auto DestinationMemory =
        createMemoryOperand(Function, {MemoryAccessFlag::Store}, Copy.SizeBytes, Copy.Align,
                            MemoryBase::frameIndex(*FrameObject));
    if (!DestinationMemory)
      return std::unexpected(DestinationMemory.error());

    auto Instruction = Builder.buildInstr(targetOpcode(InstructionOpcode::PseudoBYVAL_COPY));
    if (!Instruction)
      return std::unexpected(Instruction.error());
    auto Def = Instruction->addDef(*Temporary);
    if (!Def)
      return std::unexpected(Def.error());
    auto Use = Instruction->addUse(Copy.Source);
    if (!Use)
      return std::unexpected(Use.error());
    auto FrameOperand = Instruction->addFrameIndex(*FrameObject);
    if (!FrameOperand)
      return std::unexpected(FrameOperand.error());
    auto Size = addUnsignedImmediate(*Instruction, Copy.SizeBytes);
    if (!Size)
      return std::unexpected(Size.error());
    auto Align = addUnsignedImmediate(*Instruction, Copy.Align.bytes());
    if (!Align)
      return std::unexpected(Align.error());
    auto SourceMem = Instruction->addMemOperand(**SourceMemory);
    if (!SourceMem)
      return std::unexpected(SourceMem.error());
    auto DestinationMem = Instruction->addMemOperand(**DestinationMemory);
    if (!DestinationMem)
      return std::unexpected(DestinationMem.error());
    auto Built = Instruction->build();
    if (!Built)
      return std::unexpected(Built.error());
    Replacements.emplace(Copy.ArgumentAssignmentIndex, *Temporary);
  }
  return Replacements;
}

[[nodiscard]] Expected<void>
emitOutgoingArguments(MachineIRBuilder &Builder, const CallLoweringPlan &Plan,
                      const std::unordered_map<std::size_t, Register> &Replacements) {
  MachineFunction &Function = Builder.function();
  MachineFrameInfo &Frame = Function.frameInfo();
  const auto Assignments = Plan.argumentAssignments();
  for (std::size_t Index = 0; Index < Assignments.size(); ++Index) {
    const CCAssignment &Assignment = Assignments[Index];
    const auto Replacement = Replacements.find(Index);
    const Register Source =
        Replacement == Replacements.end() ? Assignment.OriginalValue : Replacement->second;
    if (Assignment.LocationRegister) {
      auto Copy = emitCCCopy(Builder, Register::fromPhysical(*Assignment.LocationRegister), Source,
                             outgoingTransform(Assignment));
      if (!Copy)
        return std::unexpected(Copy.error());
      continue;
    }

    if (!Assignment.StackOffsetBytes) {
      return makeUnexpected(ErrorCode::InternalError,
                            "verified outgoing stack assignment has no stack offset");
    }
    StackObjectDesc Object;
    Object.Size = Assignment.StackSlotSizeBytes;
    Object.Align = Assignment.LocationAlignment;
    Object.TargetFlags = stackObjectFlag(CallStackObjectFlag::OutgoingArgument);
    if (Plan.tailDecision() == TailCallKind::NotTail) {
      Object.Kind = StackObjectKind::Target;
      Object.LocalOffset = static_cast<std::int64_t>(*Assignment.StackOffsetBytes);
    } else {
      Object.Kind = StackObjectKind::FixedABI;
      Object.Offset = static_cast<std::int64_t>(*Assignment.StackOffsetBytes);
    }
    auto FrameObject = Frame.createObject(std::move(Object));
    if (!FrameObject)
      return std::unexpected(FrameObject.error());
    auto Memory =
        createMemoryOperand(Function, {MemoryAccessFlag::Store}, Assignment.LocationSizeBytes,
                            Assignment.LocationAlignment, MemoryBase::frameIndex(*FrameObject));
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Store =
        emitStackStore(Builder, Source, *FrameObject, 0, **Memory, outgoingTransform(Assignment));
    if (!Store)
      return std::unexpected(Store.error());
  }
  return {};
}

[[nodiscard]] Expected<void> addCallTarget(MachineInstrBuilder &Instruction,
                                           const MachineCallTarget &Callee) {
  switch (Callee.kind()) {
  case MachineCallTargetKind::GlobalSymbol:
    return Instruction.addGlobalSymbol(std::string(Callee.symbolName()), 0, Callee.targetFlags());
  case MachineCallTargetKind::ExternalSymbol:
    return Instruction.addExternalSymbol(std::string(Callee.symbolName()), 0, Callee.targetFlags());
  case MachineCallTargetKind::IndirectRegister:
    return Instruction.addUse(Callee.indirectRegister());
  case MachineCallTargetKind::Invalid:
    break;
  }
  return makeUnexpected(ErrorCode::InternalError,
                        "verified call-lowering plan has an invalid callee");
}

[[nodiscard]] Expected<PhysicalRegister> requiredLocationRegister(const CCAssignment &Assignment) {
  if (!Assignment.LocationRegister) {
    return makeUnexpected(ErrorCode::InternalError,
                          "verified return assignment has no location register");
  }
  return *Assignment.LocationRegister;
}

[[nodiscard]] Expected<void> emitCallInstruction(MachineIRBuilder &Builder,
                                                 const CallLoweringPlan &Plan) {
  const bool IsTail = Plan.tailDecision() != TailCallKind::NotTail;
  auto Instruction = Builder.buildInstr(
      targetOpcode(IsTail ? InstructionOpcode::PseudoTAIL : InstructionOpcode::PseudoCALL));
  if (!Instruction)
    return std::unexpected(Instruction.error());
  auto Callee = addCallTarget(*Instruction, *Plan.callee());
  if (!Callee)
    return std::unexpected(Callee.error());
  auto Mask = Instruction->addRegisterMask(Plan.preservedMask());
  if (!Mask)
    return std::unexpected(Mask.error());

  std::unordered_set<PhysicalRegister> DynamicUses;
  for (const CCAssignment &Assignment : Plan.argumentAssignments()) {
    if (Assignment.LocationRegister && DynamicUses.insert(*Assignment.LocationRegister).second) {
      auto Use = Instruction->addImplicitUse(Register::fromPhysical(*Assignment.LocationRegister));
      if (!Use)
        return std::unexpected(Use.error());
    }
  }
  std::unordered_set<PhysicalRegister> DynamicDefs;
  if (!IsTail) {
    for (const CCAssignment &Assignment : Plan.returnAssignments()) {
      if (Assignment.LocationRegister && DynamicDefs.insert(*Assignment.LocationRegister).second) {
        auto Def =
            Instruction->addImplicitDef(Register::fromPhysical(*Assignment.LocationRegister));
        if (!Def)
          return std::unexpected(Def.error());
      }
    }
  }
  auto Built = Instruction->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

[[nodiscard]] Expected<void> emitCall(MachineIRBuilder &Builder, const CallLoweringPlan &Plan) {
  if (Plan.callFrame().EmitSetup) {
    auto Down =
        emitCallFrameAdjustment(Builder, InstructionOpcode::ADJCALLSTACKDOWN, Plan.callFrame());
    if (!Down)
      return std::unexpected(Down.error());
  }

  auto Replacements = emitByValCopies(Builder, Plan);
  if (!Replacements)
    return std::unexpected(Replacements.error());
  auto Arguments = emitOutgoingArguments(Builder, Plan, *Replacements);
  if (!Arguments)
    return std::unexpected(Arguments.error());
  auto Call = emitCallInstruction(Builder, Plan);
  if (!Call)
    return std::unexpected(Call.error());

  if (Plan.callFrame().EmitDestroy) {
    auto Up = emitCallFrameAdjustment(Builder, InstructionOpcode::ADJCALLSTACKUP, Plan.callFrame());
    if (!Up)
      return std::unexpected(Up.error());
  }
  if (Plan.tailDecision() == TailCallKind::NotTail) {
    for (const CCAssignment &Assignment : Plan.returnAssignments()) {
      auto Location = requiredLocationRegister(Assignment);
      if (!Location)
        return std::unexpected(Location.error());
      auto Copy = emitCCCopy(Builder, Assignment.OriginalValue, Register::fromPhysical(*Location),
                             incomingTransform(Assignment));
      if (!Copy)
        return std::unexpected(Copy.error());
    }
  }

  MachineFrameInfo &Frame = Builder.function().frameInfo();
  MachineFrameState State = Frame.state();
  State.HasCalls = true;
  State.HasTailCall = State.HasTailCall || Plan.tailDecision() != TailCallKind::NotTail;
  State.AdjustsStack =
      State.AdjustsStack || (Plan.callFrame().EmitSetup && Plan.callFrame().SizeBytes != 0);
  State.HasMustTailInVarArgFunc =
      State.HasMustTailInVarArgFunc ||
      (Plan.tailDecision() == TailCallKind::MustTail && Plan.varArgs().IsVariadic);
  if (Plan.tailDecision() == TailCallKind::NotTail) {
    State.MaxCallFrameSize =
        std::max(State.MaxCallFrameSize.value_or(0), Plan.callFrame().SizeBytes);
  }
  auto AlignmentUpdate = updateMaximumFrameAlignment(State, Plan.callFrame().Align);
  if (!AlignmentUpdate)
    return std::unexpected(AlignmentUpdate.error());
  return Frame.setState(std::move(State));
}

[[nodiscard]] Expected<void> emitReturn(MachineIRBuilder &Builder, const CallLoweringPlan &Plan) {
  for (const CCAssignment &Assignment : Plan.returnAssignments()) {
    auto Location = requiredLocationRegister(Assignment);
    if (!Location)
      return std::unexpected(Location.error());
    auto Copy = emitCCCopy(Builder, Register::fromPhysical(*Location), Assignment.OriginalValue,
                           outgoingTransform(Assignment));
    if (!Copy)
      return std::unexpected(Copy.error());
  }

  auto Return = Builder.buildInstr(targetOpcode(InstructionOpcode::PseudoRET));
  if (!Return)
    return std::unexpected(Return.error());
  for (const CCAssignment &Assignment : Plan.returnAssignments()) {
    auto Location = requiredLocationRegister(Assignment);
    if (!Location)
      return std::unexpected(Location.error());
    auto Use = Return->addImplicitUse(Register::fromPhysical(*Location));
    if (!Use)
      return std::unexpected(Use.error());
  }
  auto Built = Return->build();
  if (!Built)
    return std::unexpected(Built.error());
  return {};
}

Expected<void> RISCVCallLowering::emitPlan(MachineIRBuilder &Builder,
                                           const CallLoweringPlan &Plan) const {
  auto Preflight = preflightPlan(Builder, Plan);
  if (!Preflight)
    return std::unexpected(Preflight.error());
  switch (Plan.kind()) {
  case CallLoweringPlanKind::FormalArguments:
    return emitFormalArguments(Builder, Plan);
  case CallLoweringPlanKind::Call:
    return emitCall(Builder, Plan);
  case CallLoweringPlanKind::Return:
    return emitReturn(Builder, Plan);
  }
  return makeUnexpected(ErrorCode::InternalError, "call-lowering plan kind is invalid");
}

} // namespace

const TargetCallLowering &callLowering() noexcept {
  static const RISCVCallLowering Lowering;
  return Lowering;
}

} // namespace rcc::riscv
