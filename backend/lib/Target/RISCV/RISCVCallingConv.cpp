// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVCallingConv.h"

#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>

namespace rcc::riscv {
namespace {

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

[[nodiscard]] std::uint64_t valueSizeBytes(LowLevelType Type) noexcept {
  return (static_cast<std::uint64_t>(Type.scalarBitWidth()) + 7U) / 8U;
}

[[nodiscard]] bool isSupportedFloat(const ABIValue &Value) noexcept {
  return Value.Category == ABIValueCategory::FloatingPoint &&
         (Value.Type.scalarBitWidth() == 32 || Value.Type.scalarBitWidth() == 64);
}

[[nodiscard]] Expected<PhysicalRegister> floatingRegisterView(PhysicalRegister DoubleView,
                                                              const ABIValue &Value) {
  if (!isSupportedFloat(Value)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V floating-register view requires f32 or f64");
  }
  if (Value.Type.scalarBitWidth() == 64)
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
  return !isSupportedFloat(Value) || Value.Flags.has(ABIValueFlag::ByVal) ||
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
  if (Value.Category == ABIValueCategory::Invalid || Value.Category >= ABIValueCategory::Count) {
    return makeUnexpected(ErrorCode::InvalidArgument, "RISC-V ABI value category is invalid");
  }
  if (Value.Category == ABIValueCategory::FloatingPoint && !isSupportedFloat(Value)) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V LP64 ABIs only support f32 and f64 floating fragments");
  }
  if ((Value.Category == ABIValueCategory::Pointer) != Value.Type.isPointer()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V ABI pointer category must match its pointer LowLevelType");
  }
  if (Value.Type.isPointer() && Value.Type.scalarBitWidth() != 64) {
    return makeUnexpected(ErrorCode::Unsupported, "RISC-V LP64 ABIs require 64-bit pointers");
  }
  if (Value.Category == ABIValueCategory::Integer && Value.Type.scalarBitWidth() < 64) {
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

} // namespace

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

namespace {

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

struct AssignmentState final {
  std::size_t GPR = 0;
  std::size_t FPR = 0;
  std::uint64_t StackOffset = 0;
  bool VariadicStackOnly = false;
};

[[nodiscard]] Expected<CCAssignment>
registerAssignment(const ABIValue &Value, std::size_t ValueIndex, PhysicalRegister Location,
                   LowLevelType LocationType, bool IsVariadic, bool IsBitcast) {
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
                                                     std::uint64_t StackOffset,
                                                     LowLevelType LocationType, bool IsVariadic,
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

} // namespace

[[nodiscard]] Expected<ArgumentAssignmentResult>
assignArguments(std::span<const ABIValue> Values, std::size_t FixedArgumentCount, bool IsVariadic,
                const CallingConventionInfo &Convention) {
  auto I64 = LowLevelType::makeCategoryNeutralScalar(64);
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
              floatingRegisterView(Convention.FloatingArgumentRegisters[State.FPR++], Value);
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
        const bool Bitcast = isSupportedFloat(Value);
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
      const LowLevelType LocationType = isSupportedFloat(Value) ? Value.Type : *I64;
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
  auto I64 = LowLevelType::makeCategoryNeutralScalar(64);
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
        isSupportedFloat(Value) && FPR < Convention.FloatingReturnRegisters.size();
    Expected<CCAssignment> Assignment = makeUnexpected(
        ErrorCode::Unsupported, "RISC-V calling convention has no return register available");
    if (UseFloatingRegister) {
      auto View = floatingRegisterView(Convention.FloatingReturnRegisters[FPR++], Value);
      if (!View)
        return std::unexpected(View.error());
      Assignment = registerAssignment(Value, Index, *View, Value.Type, false, false);
    } else if (GPR < Convention.IntegerReturnRegisters.size()) {
      Assignment = registerAssignment(Value, Index, Convention.IntegerReturnRegisters[GPR++], *I64,
                                      false, isSupportedFloat(Value));
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

namespace {

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

} // namespace

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

} // namespace rcc::riscv
