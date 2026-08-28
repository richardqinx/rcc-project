// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVCallLowering.h"

#include "../RISCVCallingConv.h"
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
constexpr std::uint32_t AllowedDirectCallFlags =
    static_cast<std::uint32_t>(CallTargetFlag::DirectCall);

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

} // namespace

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
  auto Immediate = APInt::fromUnsigned(64, Value);
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

[[nodiscard]] Expected<Register> extendOutgoingRegister(MachineIRBuilder &Builder, Register Source,
                                                        const CCAssignment &Assignment) {
  const std::uint32_t ValueWidth = Assignment.ValueType.scalarBitWidth();
  const std::uint32_t LocationWidth = Assignment.LocationType.scalarBitWidth();
  if (ValueWidth == LocationWidth)
    return Source;
  if (ValueWidth > LocationWidth) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V ABI register assignment narrows an outgoing value");
  }

  auto Extended = Builder.function().registerInfo().createVirtualRegister(Assignment.LocationType);
  if (!Extended)
    return std::unexpected(Extended.error());

  Expected<MachineInstr *> Built = makeUnexpected(
      ErrorCode::InternalError, "RISC-V ABI outgoing register extension was not initialized");
  switch (outgoingTransform(Assignment)) {
  case CallValueTransform::AnyExtend:
  case CallValueTransform::Bitcast:
    Built = Builder.buildAnyExt(*Extended, Source);
    break;
  case CallValueTransform::SignExtend:
    Built = Builder.buildSExt(*Extended, Source);
    break;
  case CallValueTransform::ZeroExtend:
    Built = Builder.buildZExt(*Extended, Source);
    break;
  case CallValueTransform::Full:
  case CallValueTransform::Truncate:
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V ABI outgoing register assignment lacks an extension transform");
  }
  if (!Built)
    return std::unexpected(Built.error());
  return *Extended;
}

struct OutgoingRegisterValue final {
  PhysicalRegister Location;
  Register Value;
};

[[nodiscard]] Expected<OutgoingRegisterValue>
prepareOutgoingRegisterAssignment(MachineIRBuilder &Builder, const CCAssignment &Assignment,
                                  Register Source) {
  if (!Assignment.LocationRegister) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V register assignment has no location register");
  }
  auto Extended = extendOutgoingRegister(Builder, Source, Assignment);
  if (!Extended)
    return std::unexpected(Extended.error());
  return OutgoingRegisterValue{
      .Location = *Assignment.LocationRegister,
      .Value = *Extended,
  };
}

[[nodiscard]] Expected<void>
emitOutgoingRegisterAssignments(MachineIRBuilder &Builder,
                                std::span<const OutgoingRegisterValue> Assignments) {
  for (const OutgoingRegisterValue &Assignment : Assignments) {
    auto Copy = Builder.buildCopy(Register::fromPhysical(Assignment.Location), Assignment.Value);
    if (!Copy)
      return std::unexpected(Copy.error());
  }
  return {};
}

[[nodiscard]] Expected<void> emitIncomingRegisterAssignment(MachineIRBuilder &Builder,
                                                            const CCAssignment &Assignment) {
  if (!Assignment.LocationRegister) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V register assignment has no location register");
  }

  const std::uint32_t ValueWidth = Assignment.ValueType.scalarBitWidth();
  const std::uint32_t LocationWidth = Assignment.LocationType.scalarBitWidth();
  if (ValueWidth == LocationWidth) {
    auto Copy = Builder.buildCopy(Assignment.OriginalValue,
                                  Register::fromPhysical(*Assignment.LocationRegister));
    if (!Copy)
      return std::unexpected(Copy.error());
    return {};
  }
  if (ValueWidth > LocationWidth) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V ABI register assignment widens an incoming value");
  }

  auto LocationValue =
      Builder.function().registerInfo().createVirtualRegister(Assignment.LocationType);
  if (!LocationValue)
    return std::unexpected(LocationValue.error());
  auto Copy =
      Builder.buildCopy(*LocationValue, Register::fromPhysical(*Assignment.LocationRegister));
  if (!Copy)
    return std::unexpected(Copy.error());
  auto Truncation = Builder.buildTrunc(Assignment.OriginalValue, *LocationValue);
  if (!Truncation)
    return std::unexpected(Truncation.error());
  return {};
}

[[nodiscard]] Expected<void> emitStackLoad(MachineIRBuilder &Builder,
                                           const CCAssignment &Assignment, FrameIndex Object,
                                           std::uint64_t ObjectOffset,
                                           const MachineMemOperand &Memory) {
  auto Instruction = Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoLOAD_STACK_ARG));
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
  auto Instruction =
      Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoSTORE_STACK_ARG));
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
      return makeUnexpected(
          ErrorCode::VerificationError,
          "call-lowering plan register LowLevelType differs from MachineRegisterInfo");
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
      const std::optional<LowLevelType> Type = Info->get().Type;
      if (!Type || ((!Type->isPointer() && !Type->isCategoryNeutralScalar()) ||
                    Type->scalarBitWidth() != 64)) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "indirect RISC-V callee requires a 64-bit pointer or integer LowLevelType");
      }
    }
  }
  return {};
}

[[nodiscard]] Expected<void> verifyRequiredOpcodes(const MachineIRBuilder &Builder) {
  static constexpr std::array Required{
      InstructionOpcode::PseudoLOAD_STACK_ARG, InstructionOpcode::PseudoSTORE_STACK_ARG,
      InstructionOpcode::PseudoBYVAL_COPY,     InstructionOpcode::ADJCALLSTACKDOWN,
      InstructionOpcode::ADJCALLSTACKUP,       InstructionOpcode::PseudoCALL,
      InstructionOpcode::PseudoTAIL,           InstructionOpcode::PseudoRET,
  };
  for (const InstructionOpcode OpcodeValue : Required) {
    if (Builder.target().instructionDescriptor(instructionOpcode(OpcodeValue)) == nullptr) {
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
      auto Copy = emitIncomingRegisterAssignment(Builder, Assignment);
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
  auto Instruction = Builder.buildInstr(instructionOpcode(OpcodeValue));
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

    auto Instruction = Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoBYVAL_COPY));
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
  std::vector<OutgoingRegisterValue> RegisterAssignments;
  RegisterAssignments.reserve(Assignments.size());
  for (std::size_t Index = 0; Index < Assignments.size(); ++Index) {
    const CCAssignment &Assignment = Assignments[Index];
    const auto Replacement = Replacements.find(Index);
    const Register Source =
        Replacement == Replacements.end() ? Assignment.OriginalValue : Replacement->second;
    if (Assignment.LocationRegister) {
      auto Prepared = prepareOutgoingRegisterAssignment(Builder, Assignment, Source);
      if (!Prepared)
        return std::unexpected(Prepared.error());
      RegisterAssignments.push_back(*Prepared);
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
  return emitOutgoingRegisterAssignments(
      Builder, std::span<const OutgoingRegisterValue>(RegisterAssignments));
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
      instructionOpcode(IsTail ? InstructionOpcode::PseudoTAIL : InstructionOpcode::PseudoCALL));
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
      auto Copy = emitIncomingRegisterAssignment(Builder, Assignment);
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
  std::vector<OutgoingRegisterValue> RegisterAssignments;
  RegisterAssignments.reserve(Plan.returnAssignments().size());
  for (const CCAssignment &Assignment : Plan.returnAssignments()) {
    auto Prepared =
        prepareOutgoingRegisterAssignment(Builder, Assignment, Assignment.OriginalValue);
    if (!Prepared)
      return std::unexpected(Prepared.error());
    RegisterAssignments.push_back(*Prepared);
  }
  auto Copies = emitOutgoingRegisterAssignments(
      Builder, std::span<const OutgoingRegisterValue>(RegisterAssignments));
  if (!Copies)
    return std::unexpected(Copies.error());

  auto Return = Builder.buildInstr(instructionOpcode(InstructionOpcode::PseudoRET));
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

} // namespace rcc::riscv
