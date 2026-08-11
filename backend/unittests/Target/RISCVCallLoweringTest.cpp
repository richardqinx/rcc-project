// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVCallLowering.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
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

[[nodiscard]] rcc::CallingConventionID convention() noexcept {
  return rcc::riscv::callingConventionID(rcc::riscv::CallingConvention::LP64D);
}

[[nodiscard]] rcc::Alignment align(std::uint64_t Bytes) {
  const auto Value = rcc::Alignment::create(Bytes);
  if (!Value)
    std::terminate();
  return *Value;
}

[[nodiscard]] rcc::LLT integer(std::uint32_t Bits) {
  const auto Type = rcc::LLT::makeInteger(Bits);
  if (!Type)
    std::terminate();
  return *Type;
}

[[nodiscard]] rcc::LLT anyScalar(std::uint32_t Bits) {
  const auto Type = rcc::LLT::makeAnyScalar(Bits);
  if (!Type)
    std::terminate();
  return *Type;
}

[[nodiscard]] rcc::LLT floating(rcc::FloatSemantics Semantics) {
  const auto Type = rcc::LLT::makeFloat(Semantics);
  if (!Type)
    std::terminate();
  return *Type;
}

[[nodiscard]] rcc::LLT pointer() {
  const auto Type = rcc::LLT::makePointer(0, 64);
  if (!Type)
    std::terminate();
  return *Type;
}

[[nodiscard]] rcc::PhysicalRegister physical(rcc::riscv::PhysicalRegisterID ID) {
  const auto Register = rcc::riscv::physicalRegister(ID);
  if (!Register)
    std::terminate();
  return *Register;
}

[[nodiscard]] rcc::PhysicalRegister argumentGPR(std::size_t Index) {
  const auto *Convention = rcc::riscv::callingConvention(rcc::riscv::CallingConvention::LP64D);
  if (Convention == nullptr || Index >= Convention->IntegerArgumentRegisters.size())
    std::terminate();
  return Convention->IntegerArgumentRegisters[Index];
}

[[nodiscard]] rcc::PhysicalRegister argumentFPR(std::size_t Index) {
  const auto *Convention = rcc::riscv::callingConvention(rcc::riscv::CallingConvention::LP64D);
  if (Convention == nullptr || Index >= Convention->FloatingArgumentRegisters.size())
    std::terminate();
  return Convention->FloatingArgumentRegisters[Index];
}

[[nodiscard]] rcc::PhysicalRegister returnGPR(std::size_t Index) {
  const auto *Convention = rcc::riscv::callingConvention(rcc::riscv::CallingConvention::LP64D);
  if (Convention == nullptr || Index >= Convention->IntegerReturnRegisters.size())
    std::terminate();
  return Convention->IntegerReturnRegisters[Index];
}

[[nodiscard]] rcc::PhysicalRegister returnFPR(std::size_t Index) {
  const auto *Convention = rcc::riscv::callingConvention(rcc::riscv::CallingConvention::LP64D);
  if (Convention == nullptr || Index >= Convention->FloatingReturnRegisters.size())
    std::terminate();
  return Convention->FloatingReturnRegisters[Index];
}

[[nodiscard]] rcc::Register planningRegister() {
  return rcc::Register::fromPhysical(physical(rcc::riscv::PhysicalRegisterID::X5));
}

[[nodiscard]] rcc::ABIValue
abiValue(rcc::LLT Type, std::uint32_t OriginalValue, rcc::Register Value = planningRegister(),
         rcc::ABIExtensionKind Extension = rcc::ABIExtensionKind::None) {
  const std::uint64_t Size = std::max<std::uint64_t>(1, (Type.scalarBitWidth() + 7U) / 8U);
  return {
      .Value = Value,
      .Type = Type,
      .OriginalValue = OriginalValue,
      .FragmentIndex = 0,
      .FragmentCount = 1,
      .FragmentOffsetBytes = 0,
      .OriginalAlignment = align(std::min<std::uint64_t>(Size, 8)),
      .Extension = Extension,
      .Flags = {},
      .ByValSizeBytes = 0,
      .ByValAlignment = align(1),
  };
}

[[nodiscard]] std::vector<rcc::ABIValue> integerValues(std::size_t Count) {
  std::vector<rcc::ABIValue> Values;
  Values.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index)
    Values.push_back(abiValue(integer(64), static_cast<std::uint32_t>(Index)));
  return Values;
}

[[nodiscard]] std::vector<rcc::ABIValue> floatingValues(std::size_t Count) {
  std::vector<rcc::ABIValue> Values;
  Values.reserve(Count);
  for (std::size_t Index = 0; Index < Count; ++Index) {
    Values.push_back(
        abiValue(floating(rcc::FloatSemantics::IEEEDouble), static_cast<std::uint32_t>(Index)));
  }
  return Values;
}

[[nodiscard]] rcc::MachineCallTarget externalTarget() {
  auto Target = rcc::MachineCallTarget::externalSymbol("callee");
  if (!Target)
    std::terminate();
  return std::move(*Target);
}

[[nodiscard]] rcc::CallLoweringRequest callRequest(std::vector<rcc::ABIValue> Arguments,
                                                   std::size_t FixedArgumentCount,
                                                   bool IsVariadic = false) {
  rcc::CallLoweringRequest Request;
  Request.Convention = convention();
  Request.Callee = externalTarget();
  Request.Arguments = std::move(Arguments);
  Request.FixedArgumentCount = FixedArgumentCount;
  Request.IsVariadic = IsVariadic;
  return Request;
}

[[nodiscard]] bool isLocation(const rcc::CCAssignment &Assignment, rcc::PhysicalRegister Register) {
  return Assignment.LocationRegister && *Assignment.LocationRegister == Register;
}

void testIntegerAndFloatingAssignments(TestContext &Context) {
  auto Integers = callRequest(integerValues(9), 9);
  auto IntegerPlan = rcc::riscv::callLowering().planCall(Integers);
  Context.expect(IntegerPlan.has_value(), "nine integer arguments must produce an LP64D call plan");
  if (!IntegerPlan)
    return;
  const auto IntegerAssignments = IntegerPlan->argumentAssignments();
  const auto LP64DMask = rcc::riscv::createCallPreservedMask(rcc::riscv::CallingConvention::LP64D);
  bool IntegerRegistersMatch = IntegerAssignments.size() == 9;
  for (std::size_t Index = 0; IntegerRegistersMatch && Index < 8; ++Index)
    IntegerRegistersMatch = isLocation(IntegerAssignments[Index], argumentGPR(Index));
  Context.expect(IntegerRegistersMatch &&
                     IntegerAssignments[8].LocationKind == rcc::CCLocationKind::Stack &&
                     IntegerAssignments[8].StackOffsetBytes == 0 &&
                     IntegerPlan->stackArguments().SizeBytes == 16 &&
                     IntegerPlan->callFrame().SizeBytes == 16 &&
                     IntegerPlan->callee()->targetFlags() ==
                         static_cast<std::uint32_t>(rcc::riscv::CallTargetFlag::DirectCall) &&
                     LP64DMask != nullptr && IntegerPlan->preservedMask() != nullptr &&
                     *IntegerPlan->preservedMask() == *LP64DMask,
                 "LP64D integer arguments must use a0-a7 before a 16-byte stack area");

  auto Floats = callRequest(floatingValues(17), 17);
  auto FloatPlan = rcc::riscv::callLowering().planCall(Floats);
  Context.expect(FloatPlan.has_value(),
                 "seventeen floating arguments must produce an LP64D call plan");
  if (!FloatPlan)
    return;
  const auto FloatAssignments = FloatPlan->argumentAssignments();
  bool FloatingRegistersMatch = FloatAssignments.size() == 17;
  for (std::size_t Index = 0; FloatingRegistersMatch && Index < 8; ++Index)
    FloatingRegistersMatch = isLocation(FloatAssignments[Index], argumentFPR(Index));
  for (std::size_t Index = 0; FloatingRegistersMatch && Index < 8; ++Index) {
    FloatingRegistersMatch =
        isLocation(FloatAssignments[Index + 8], argumentGPR(Index)) &&
        FloatAssignments[Index + 8].CustomFlags.has(rcc::CCAssignmentFlag::Bitcast);
  }
  Context.expect(FloatingRegistersMatch &&
                     FloatAssignments[16].LocationKind == rcc::CCLocationKind::Stack &&
                     FloatAssignments[16].StackOffsetBytes == 0,
                 "LP64D floating arguments must fall back from fa0-fa7 to a0-a7 and then stack");

  auto Single = callRequest({abiValue(floating(rcc::FloatSemantics::IEEESingle), 0)}, 1);
  Single.Results = {abiValue(floating(rcc::FloatSemantics::IEEESingle), 0)};
  auto SinglePlan = rcc::riscv::callLowering().planCall(Single);
  Context.expect(SinglePlan && SinglePlan->argumentAssignments().size() == 1 &&
                     SinglePlan->returnAssignments().size() == 1 &&
                     isLocation(SinglePlan->argumentAssignments()[0],
                                physical(rcc::riscv::PhysicalRegisterID::F10_F)) &&
                     isLocation(SinglePlan->returnAssignments()[0],
                                physical(rcc::riscv::PhysicalRegisterID::F10_F)),
                 "LP64D f32 arguments and returns must use the FPR32 physical-register view");

  auto FloatFragment = abiValue(floating(rcc::FloatSemantics::IEEEDouble), 0);
  FloatFragment.FragmentCount = 2;
  FloatFragment.OriginalAlignment = align(16);
  auto IntegerFragment = abiValue(integer(64), 0);
  IntegerFragment.FragmentIndex = 1;
  IntegerFragment.FragmentCount = 2;
  IntegerFragment.FragmentOffsetBytes = 8;
  IntegerFragment.OriginalAlignment = align(16);
  auto Mixed = callRequest({FloatFragment, IntegerFragment}, 1);
  auto MixedPlan = rcc::riscv::callLowering().planCall(Mixed);
  Context.expect(MixedPlan && MixedPlan->argumentAssignments().size() == 2 &&
                     isLocation(MixedPlan->argumentAssignments()[0], argumentFPR(0)) &&
                     isLocation(MixedPlan->argumentAssignments()[1], argumentGPR(0)),
                 "a mixed floating/integer aggregate must use one FPR and one GPR");

  auto GenericScalar = callRequest({abiValue(anyScalar(64), 0)}, 1);
  GenericScalar.Results = {abiValue(anyScalar(64), 0)};
  auto GenericScalarPlan = rcc::riscv::callLowering().planCall(GenericScalar);
  Context.expect(GenericScalarPlan && GenericScalarPlan->argumentAssignments().size() == 1 &&
                     GenericScalarPlan->returnAssignments().size() == 1 &&
                     GenericScalarPlan->argumentAssignments()[0].ValueType == anyScalar(64) &&
                     GenericScalarPlan->returnAssignments()[0].ValueType == anyScalar(64),
                 "LLVM generic scalar LLTs must use integer ABI transport without type loss");

  auto LP64Float = callRequest({abiValue(floating(rcc::FloatSemantics::IEEEDouble), 0)}, 1);
  LP64Float.Convention = rcc::riscv::callingConventionID(rcc::riscv::CallingConvention::LP64);
  LP64Float.Results = {abiValue(floating(rcc::FloatSemantics::IEEEDouble), 0)};
  auto LP64Plan = rcc::riscv::callLowering().planCall(LP64Float);
  const auto LP64Mask = rcc::riscv::createCallPreservedMask(rcc::riscv::CallingConvention::LP64);
  Context.expect(
      LP64Plan && LP64Plan->argumentAssignments().size() == 1 &&
          LP64Plan->returnAssignments().size() == 1 &&
          isLocation(LP64Plan->argumentAssignments()[0], argumentGPR(0)) &&
          isLocation(LP64Plan->returnAssignments()[0], returnGPR(0)) &&
          LP64Plan->argumentAssignments()[0].CustomFlags.has(rcc::CCAssignmentFlag::Bitcast) &&
          LP64Plan->returnAssignments()[0].CustomFlags.has(rcc::CCAssignmentFlag::Bitcast) &&
          LP64Mask != nullptr && LP64Plan->preservedMask() != nullptr &&
          *LP64Plan->preservedMask() == *LP64Mask,
      "LP64 must transport floating values through GPRs and use the integer preserved mask");

  auto Narrow = abiValue(integer(32), 0);
  auto InvalidNarrow = callRequest({Narrow}, 1);
  Context.expect(!rcc::riscv::callLowering().planCall(InvalidNarrow),
                 "a narrow integer without an explicit ABI extension must be rejected");
  Narrow.Extension = rcc::ABIExtensionKind::SignExtend;
  auto ExtendedNarrow = callRequest({Narrow}, 1);
  auto NarrowPlan = rcc::riscv::callLowering().planCall(ExtendedNarrow);
  Context.expect(NarrowPlan && NarrowPlan->argumentAssignments()[0].LocationType == integer(64) &&
                     NarrowPlan->argumentAssignments()[0].Extension ==
                         rcc::ABIExtensionKind::SignExtend,
                 "a narrow integer extension must be explicit in its LP64D assignment");
}

void testVariadicAssignments(TestContext &Context) {
  auto FixedFloat = abiValue(floating(rcc::FloatSemantics::IEEEDouble), 0);
  auto VariableFloat = abiValue(floating(rcc::FloatSemantics::IEEEDouble), 1);
  auto Request = callRequest({FixedFloat, VariableFloat}, 1, true);
  auto Plan = rcc::riscv::callLowering().planCall(Request);
  Context.expect(
      Plan && isLocation(Plan->argumentAssignments()[0], argumentFPR(0)) &&
          isLocation(Plan->argumentAssignments()[1], argumentGPR(0)) &&
          Plan->argumentAssignments()[1].CustomFlags.has(rcc::CCAssignmentFlag::Variadic) &&
          Plan->argumentAssignments()[1].CustomFlags.has(rcc::CCAssignmentFlag::Bitcast),
      "variadic floating arguments must use integer calling-convention transport");

  auto FixedInteger = abiValue(integer(64), 0);
  auto PairLow = abiValue(integer(64), 1);
  PairLow.FragmentCount = 2;
  PairLow.OriginalAlignment = align(16);
  auto PairHigh = abiValue(integer(64), 1);
  PairHigh.FragmentIndex = 1;
  PairHigh.FragmentCount = 2;
  PairHigh.FragmentOffsetBytes = 8;
  PairHigh.OriginalAlignment = align(16);
  auto PairRequest = callRequest({FixedInteger, PairLow, PairHigh}, 1, true);
  auto PairPlan = rcc::riscv::callLowering().planCall(PairRequest);
  Context.expect(PairPlan && isLocation(PairPlan->argumentAssignments()[0], argumentGPR(0)) &&
                     isLocation(PairPlan->argumentAssignments()[1], argumentGPR(2)) &&
                     isLocation(PairPlan->argumentAssignments()[2], argumentGPR(3)),
                 "an aligned variadic pair must skip an odd GPR before assignment");

  rcc::FormalArgumentRequest Formal;
  Formal.Convention = convention();
  Formal.Arguments = {FixedInteger};
  Formal.FixedArgumentCount = 1;
  Formal.IsVariadic = true;
  auto FormalPlan = rcc::riscv::callLowering().planFormalArguments(Formal);
  Context.expect(FormalPlan && FormalPlan->varArgs().IsVariadic &&
                     FormalPlan->varArgs().FirstUnallocatedIntegerRegister == 1 &&
                     FormalPlan->varArgs().RegisterSaveAreaSizeBytes == 56 &&
                     FormalPlan->varArgs().RegisterSaveAreaOffsetBytes == -56 &&
                     FormalPlan->varArgs().RegisterSavePaddingBytes == 8 &&
                     FormalPlan->varArgs().RegistersToSave.size() == 7 &&
                     FormalPlan->varArgs().RegistersToSave.front().Reg == argumentGPR(1) &&
                     FormalPlan->varArgs().RegistersToSave.back().Reg == argumentGPR(7),
                 "a variadic formal function must describe its GPR save area and padding");
}

void testSpecialArgumentsAndReturns(TestContext &Context) {
  auto ByVal = abiValue(pointer(), 0);
  ByVal.Flags.set(rcc::ABIValueFlag::ByVal);
  ByVal.ByValSizeBytes = 24;
  ByVal.ByValAlignment = align(16);
  auto ByValRequest = callRequest({ByVal}, 1);
  auto ByValPlan = rcc::riscv::callLowering().planCall(ByValRequest);
  Context.expect(
      ByValPlan && ByValPlan->byValCopies().size() == 1 &&
          ByValPlan->byValCopies()[0].StackOffsetBytes == 0 &&
          ByValPlan->callFrame().SizeBytes == 32 &&
          ByValPlan->argumentAssignments()[0].CustomFlags.has(rcc::CCAssignmentFlag::ByVal),
      "byval must allocate an aligned caller-owned copy and pass its pointer");

  auto SRet = abiValue(pointer(), 0);
  SRet.Flags.set(rcc::ABIValueFlag::StructReturn);
  auto SRetPlan = rcc::riscv::callLowering().planCall(callRequest({SRet}, 1));
  Context.expect(
      SRetPlan && isLocation(SRetPlan->argumentAssignments()[0], argumentGPR(0)) &&
          SRetPlan->argumentAssignments()[0].CustomFlags.has(rcc::CCAssignmentFlag::StructReturn),
      "sret must be carried as the first integer argument");
  auto SRetWithDirectResult = callRequest({SRet}, 1);
  SRetWithDirectResult.Results = {abiValue(integer(64), 0)};
  Context.expect(!rcc::riscv::callLowering().planCall(SRetWithDirectResult),
                 "an sret call cannot also expose direct ABI return registers");
  auto Plain = abiValue(integer(64), 0);
  SRet.OriginalValue = 1;
  Context.expect(!rcc::riscv::callLowering().planCall(callRequest({Plain, SRet}, 2)),
                 "sret outside the first argument position must be rejected");

  auto Indirect = abiValue(pointer(), 0);
  Indirect.Flags.set(rcc::ABIValueFlag::Indirect);
  auto IndirectPlan = rcc::riscv::callLowering().planCall(callRequest({Indirect}, 1));
  Context.expect(
      IndirectPlan &&
          IndirectPlan->argumentAssignments()[0].LocationKind == rcc::CCLocationKind::Indirect &&
          IndirectPlan->argumentAssignments()[0].CustomFlags.has(rcc::CCAssignmentFlag::Indirect),
      "an explicitly indirect ABI value must preserve its transport classification");

  auto InRegister = integerValues(9);
  InRegister.back().Flags.set(rcc::ABIValueFlag::InRegister);
  Context.expect(!rcc::riscv::callLowering().planCall(callRequest(std::move(InRegister), 9)),
                 "an inreg argument that would spill to the stack must be rejected");

  rcc::ReturnLoweringRequest Return;
  Return.Convention = convention();
  Return.Values = {abiValue(integer(64), 0),
                   abiValue(floating(rcc::FloatSemantics::IEEEDouble), 1)};
  auto ReturnPlan = rcc::riscv::callLowering().planReturn(Return);
  Context.expect(ReturnPlan && ReturnPlan->returnAssignments().size() == 2 &&
                     isLocation(ReturnPlan->returnAssignments()[0], returnGPR(0)) &&
                     isLocation(ReturnPlan->returnAssignments()[1], returnFPR(0)),
                 "mixed integer and floating returns must use a0 and fa0 independently");
}

[[nodiscard]] rcc::TailCallContext emptyTailContext() {
  rcc::TailCallContext Context;
  Context.CallerConvention = convention();
  Context.ReusableIncomingArgumentBytes = 0;
  Context.CallerFixedArgumentCount = 0;
  return Context;
}

void testTailCalls(TestContext &Context) {
  auto Tail = callRequest({}, 0);
  Tail.TailKind = rcc::TailCallKind::Tail;
  Tail.TailContext = emptyTailContext();
  auto TailPlan = rcc::riscv::callLowering().planCall(Tail);
  Context.expect(TailPlan && TailPlan->tailDecision() == rcc::TailCallKind::Tail &&
                     TailPlan->callFrame().ReusesIncomingArea && !TailPlan->callFrame().EmitSetup,
                 "an ABI-compatible tail call must reuse the incoming argument area");

  auto Fallback = callRequest({}, 0);
  Fallback.TailKind = rcc::TailCallKind::Tail;
  auto FallbackPlan = rcc::riscv::callLowering().planCall(Fallback);
  Context.expect(FallbackPlan && FallbackPlan->tailDecision() == rcc::TailCallKind::NotTail &&
                     !FallbackPlan->tailRejectionReason().empty() &&
                     FallbackPlan->callFrame().EmitSetup,
                 "a non-mandatory incompatible tail call must fall back with a reason");

  auto MustTail = callRequest({}, 0);
  MustTail.TailKind = rcc::TailCallKind::MustTail;
  auto MustTailPlan = rcc::riscv::callLowering().planCall(MustTail);
  Context.expect(!MustTailPlan && MustTailPlan.error().code() == rcc::ErrorCode::Unsupported,
                 "an incompatible musttail call must fail instead of falling back");

  auto StackTail = callRequest(integerValues(9), 9);
  StackTail.TailKind = rcc::TailCallKind::Tail;
  auto StackContext = emptyTailContext();
  StackContext.CallerFixedArgumentCount = 9;
  StackContext.ReusableIncomingArgumentBytes = 16;
  StackTail.TailContext = std::move(StackContext);
  auto StackTailPlan = rcc::riscv::callLowering().planCall(StackTail);
  Context.expect(StackTailPlan && StackTailPlan->tailDecision() == rcc::TailCallKind::Tail &&
                     StackTailPlan->callFrame().SizeBytes == 16,
                 "a tail call may reuse a sufficiently large incoming stack argument area");
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction> createFunction(TestContext &Context,
                                                                   std::string Name,
                                                                   std::uint64_t ID,
                                                                   rcc::MachineBasicBlock *&Block) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  auto CreatedBlock = (*Function)->createBasicBlock();
  Context.expect(CreatedBlock && (*Function)->appendBlock(**CreatedBlock),
                 "machine entry block creation must succeed");
  if (!CreatedBlock || !(*Function)->firstBlock())
    return nullptr;
  Block = *CreatedBlock;
  return std::move(*Function);
}

[[nodiscard]] rcc::Register virtualRegister(rcc::MachineFunction &Function, rcc::LLT Type) {
  auto Register = Function.registerInfo().createVirtualRegister(Type);
  if (!Register)
    std::terminate();
  return *Register;
}

[[nodiscard]] rcc::Opcode opcode(rcc::riscv::InstructionOpcode Opcode) noexcept {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Opcode));
}

[[nodiscard]] std::vector<rcc::MachineInstr *> instructions(rcc::MachineBasicBlock &Block) {
  std::vector<rcc::MachineInstr *> Result;
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    Result.push_back(Instruction);
  }
  return Result;
}

[[nodiscard]] rcc::MachineInstr *findInstruction(rcc::MachineBasicBlock &Block,
                                                 rcc::riscv::InstructionOpcode OpcodeValue) {
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (Instruction->opcode() == opcode(OpcodeValue))
      return Instruction;
  }
  return nullptr;
}

[[nodiscard]] bool verifyFunction(const rcc::MachineFunction &Function,
                                  const rcc::TargetDescription &Target) {
  rcc::MachineVerifierOptions Options;
  Options.VerifyProperties = false;
  return rcc::MachineVerifier::verify(Function, Target, Options).has_value();
}

void testOrdinaryAndIndirectEmission(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  rcc::MachineBasicBlock *Block = nullptr;
  auto Function = createFunction(Context, "ordinary_call", 1, Block);
  Context.expect(Target && Function && Block != nullptr, "ordinary-call fixtures must be valid");
  if (!Target || !Function || Block == nullptr)
    return;

  const rcc::LLT I64 = integer(64);
  const rcc::LLT F64 = floating(rcc::FloatSemantics::IEEEDouble);
  const rcc::Register Argument = virtualRegister(*Function, I64);
  const rcc::Register Result = virtualRegister(*Function, F64);
  auto Request = callRequest({abiValue(I64, 0, Argument)}, 1);
  Request.Results = {abiValue(F64, 0, Result)};

  rcc::MachineIRBuilder Builder(*Function, *Target);
  Context.expect(Builder.setInsertPoint(*Block).has_value(),
                 "ordinary-call insertion point must be valid");
  auto Lowered = rcc::riscv::callLowering().lowerCall(Builder, Request);
  Context.expect(Lowered.has_value(), "ordinary direct call emission must succeed");
  if (!Lowered)
    return;

  const auto Instructions = instructions(*Block);
  rcc::MachineInstr *FrameDown = Instructions.empty() ? nullptr : Instructions.front();
  rcc::MachineInstr *FrameUp = Instructions.size() > 3 ? Instructions[3] : nullptr;
  rcc::MachineInstr *Call = findInstruction(*Block, rcc::riscv::InstructionOpcode::PseudoCALL);
  Context.expect(Instructions.size() == 5 &&
                     Instructions.front()->opcode() ==
                         opcode(rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN) &&
                     Instructions[1]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     Instructions.back()->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     Call != nullptr && Call->operandCount() >= 5 &&
                     Call->operandAt(0)->kind() == rcc::MachineOperand::Kind::ExternalSymbol &&
                     Call->operandAt(0)->targetFlags() ==
                         static_cast<std::uint32_t>(rcc::riscv::CallTargetFlag::DirectCall) &&
                     Call->operandAt(1)->kind() == rcc::MachineOperand::Kind::RegisterMask,
                 "ordinary emission must materialize canonical full-value copies, call-frame, "
                 "target, and RegMask");
  const rcc::Register StackPointer =
      rcc::Register::fromPhysical(physical(rcc::riscv::PhysicalRegisterID::X2));
  const auto HasStackPointerEffects = [StackPointer](const rcc::MachineInstr *Instruction) {
    bool HasDef = false;
    bool HasUse = false;
    if (Instruction == nullptr)
      return false;
    for (const rcc::MachineOperand &Operand : Instruction->operands()) {
      if (!Operand.isRegister() || !Operand.isImplicit() ||
          Operand.registerValue() != StackPointer) {
        continue;
      }
      HasDef |= Operand.isDef();
      HasUse |= Operand.isUse();
    }
    return HasDef && HasUse;
  };
  const auto ImmediateAt = [](const rcc::MachineInstr *Instruction, std::size_t Index) {
    const rcc::MachineOperand *Operand =
        Instruction == nullptr ? nullptr : Instruction->operandAt(Index);
    return Operand == nullptr ? nullptr : Operand->getIf<std::int64_t>();
  };
  const auto *DownAmount = ImmediateAt(FrameDown, 0);
  const auto *DownPrepared = ImmediateAt(FrameDown, 1);
  const auto *UpAmount = ImmediateAt(FrameUp, 0);
  const auto *UpPrepared = ImmediateAt(FrameUp, 1);
  Context.expect(
      DownAmount != nullptr && *DownAmount == 0 && DownPrepared != nullptr && *DownPrepared == 0 &&
          UpAmount != nullptr && *UpAmount == 0 && UpPrepared != nullptr && *UpPrepared == 0 &&
          HasStackPointerEffects(FrameDown) && HasStackPointerEffects(FrameUp) &&
          !FrameDown->flags().has(rcc::MachineInstrFlag::FrameSetup) &&
          !FrameUp->flags().has(rcc::MachineInstrFlag::FrameDestroy),
      "call-frame pseudos must use LLVM amount/prepared-byte operands and implicit SP effects");
  bool HasArgumentUse = false;
  bool HasResultDef = false;
  std::optional<std::size_t> ArgumentUseIndex;
  std::optional<std::size_t> ResultDefIndex;
  std::size_t OperandIndex = 0;
  for (const rcc::MachineOperand &Operand : Call->operands()) {
    if (!Operand.isRegister() || !Operand.isImplicit()) {
      ++OperandIndex;
      continue;
    }
    if (Operand.isUse() && Operand.registerValue() == rcc::Register::fromPhysical(argumentGPR(0))) {
      HasArgumentUse = true;
      ArgumentUseIndex = OperandIndex;
    }
    if (Operand.isDef() && Operand.registerValue() == rcc::Register::fromPhysical(returnFPR(0))) {
      HasResultDef = true;
      ResultDefIndex = OperandIndex;
    }
    ++OperandIndex;
  }
  Context.expect(HasArgumentUse && HasResultDef && ArgumentUseIndex && ResultDefIndex &&
                     *ArgumentUseIndex < *ResultDefIndex &&
                     Function->frameInfo().state().HasCalls &&
                     Function->frameInfo().maxCallFrameSize() == 0 &&
                     Function->frameInfo().stackAlignment().bytes() == 16 &&
                     verifyFunction(*Function, *Target),
                 "ordinary call emission must order implicit uses before defs and record frame "
                 "state");

  rcc::MachineBasicBlock *IndirectBlock = nullptr;
  auto IndirectFunction = createFunction(Context, "indirect_call", 2, IndirectBlock);
  if (!IndirectFunction || IndirectBlock == nullptr)
    return;
  const rcc::Register Callee = virtualRegister(*IndirectFunction, pointer());
  auto IndirectTarget = rcc::MachineCallTarget::indirect(Callee);
  if (!IndirectTarget)
    return;
  auto IndirectRequest = callRequest({}, 0);
  IndirectRequest.Callee = std::move(*IndirectTarget);
  rcc::MachineIRBuilder IndirectBuilder(*IndirectFunction, *Target);
  if (!IndirectBuilder.setInsertPoint(*IndirectBlock))
    return;
  auto IndirectLowered = rcc::riscv::callLowering().lowerCall(IndirectBuilder, IndirectRequest);
  rcc::MachineInstr *IndirectCall =
      findInstruction(*IndirectBlock, rcc::riscv::InstructionOpcode::PseudoCALL);
  Context.expect(IndirectLowered && IndirectCall != nullptr &&
                     IndirectCall->operandAt(0)->registerValue() == Callee &&
                     verifyFunction(*IndirectFunction, *Target),
                 "indirect call emission must retain the callee register operand");
}

void testStackAndByValEmission(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  rcc::MachineBasicBlock *Block = nullptr;
  auto Function = createFunction(Context, "stack_byval_call", 3, Block);
  if (!Target || !Function || Block == nullptr)
    return;

  std::vector<rcc::ABIValue> Arguments;
  const rcc::Register ByValSource = virtualRegister(*Function, pointer());
  auto ByVal = abiValue(pointer(), 0, ByValSource);
  ByVal.Flags.set(rcc::ABIValueFlag::ByVal);
  ByVal.ByValSizeBytes = 24;
  ByVal.ByValAlignment = align(16);
  Arguments.push_back(ByVal);
  for (std::uint32_t Index = 1; Index < 9; ++Index) {
    Arguments.push_back(abiValue(integer(64), Index, virtualRegister(*Function, integer(64))));
  }
  auto Request = callRequest(std::move(Arguments), 9);
  rcc::MachineIRBuilder Builder(*Function, *Target);
  if (!Builder.setInsertPoint(*Block))
    return;
  auto Lowered = rcc::riscv::callLowering().lowerCall(Builder, Request);
  rcc::MachineInstr *ByValCopy =
      findInstruction(*Block, rcc::riscv::InstructionOpcode::PseudoBYVAL_COPY);
  rcc::MachineInstr *StackStore =
      findInstruction(*Block, rcc::riscv::InstructionOpcode::PseudoSTORE_STACK_ARG);
  bool HasByValObject = false;
  bool HasOutgoingObject = false;
  for (const rcc::StackObject &Object : Function->frameInfo().objects()) {
    const rcc::StackObjectDesc *Description = Object.description();
    if (Description == nullptr)
      continue;
    HasByValObject |= Description->Kind == rcc::StackObjectKind::Target &&
                      Description->TargetFlags ==
                          static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::ByValCopy);
    HasOutgoingObject |=
        Description->Kind == rcc::StackObjectKind::Target &&
        Description->TargetFlags ==
            static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::OutgoingArgument);
  }
  Context.expect(
      Lowered && ByValCopy != nullptr && ByValCopy->memOperands().size() == 2 &&
          ByValCopy->memOperands()[0]->isLoad() && ByValCopy->memOperands()[1]->isStore() &&
          StackStore != nullptr && StackStore->memOperands().size() == 1 &&
          StackStore->memOperands()[0]->isStore() && HasByValObject && HasOutgoingObject &&
          Function->frameInfo().liveObjectCount() == 2 &&
          Function->frameInfo().maxCallFrameSize() == 48 && verifyFunction(*Function, *Target),
      "stack and byval emission must create precise objects, MMOs, and frame size");
}

void testFormalReturnAndTailEmission(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return;

  rcc::MachineBasicBlock *FormalBlock = nullptr;
  auto FormalFunction = createFunction(Context, "variadic_formal", 4, FormalBlock);
  if (!FormalFunction || FormalBlock == nullptr)
    return;
  const rcc::Register FormalValue = virtualRegister(*FormalFunction, integer(64));
  rcc::FormalArgumentRequest Formal;
  Formal.Convention = convention();
  Formal.Arguments = {abiValue(integer(64), 0, FormalValue)};
  Formal.FixedArgumentCount = 1;
  Formal.IsVariadic = true;
  rcc::MachineIRBuilder FormalBuilder(*FormalFunction, *Target);
  if (!FormalBuilder.setInsertPoint(*FormalBlock))
    return;
  auto FormalLowered = rcc::riscv::callLowering().lowerFormalArguments(FormalBuilder, Formal);
  std::size_t SaveStores = 0;
  for (rcc::MachineInstr *Instruction : instructions(*FormalBlock)) {
    if (Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::PseudoSTORE_STACK_ARG))
      ++SaveStores;
  }
  bool HasSaveArea = false;
  bool HasPadding = false;
  for (const rcc::StackObject &Object : FormalFunction->frameInfo().objects()) {
    const rcc::StackObjectDesc *Description = Object.description();
    if (Description == nullptr)
      continue;
    HasSaveArea |= Description->TargetFlags ==
                   static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::VarArgSaveArea);
    HasPadding |=
        Description->TargetFlags ==
        static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::VarArgAlignmentPadding);
  }
  Context.expect(FormalLowered && SaveStores == 7 && HasSaveArea && HasPadding &&
                     FormalFunction->registerInfo().liveIns().size() == 8 &&
                     FormalBlock->liveIns().size() == 8 && verifyFunction(*FormalFunction, *Target),
                 "variadic formal emission must save remaining GPRs with explicit padding");

  rcc::MachineBasicBlock *ReturnBlock = nullptr;
  auto ReturnFunction = createFunction(Context, "return_value", 5, ReturnBlock);
  if (!ReturnFunction || ReturnBlock == nullptr)
    return;
  const rcc::Register ReturnValue = virtualRegister(*ReturnFunction, integer(64));
  rcc::ReturnLoweringRequest Return;
  Return.Convention = convention();
  Return.Values = {abiValue(integer(64), 0, ReturnValue)};
  rcc::MachineIRBuilder ReturnBuilder(*ReturnFunction, *Target);
  if (!ReturnBuilder.setInsertPoint(*ReturnBlock))
    return;
  auto ReturnLowered = rcc::riscv::callLowering().lowerReturn(ReturnBuilder, Return);
  rcc::MachineInstr *ReturnPseudo =
      findInstruction(*ReturnBlock, rcc::riscv::InstructionOpcode::PseudoRET);
  bool HasReturnRegister = false;
  if (ReturnPseudo != nullptr) {
    HasReturnRegister = std::ranges::any_of(ReturnPseudo->operands(), [](const auto &Operand) {
      return Operand.isRegister() && Operand.isImplicit() && Operand.isUse() &&
             Operand.registerValue() == rcc::Register::fromPhysical(returnGPR(0));
    });
  }
  Context.expect(ReturnLowered && instructions(*ReturnBlock).size() == 2 &&
                     ReturnPseudo != nullptr && HasReturnRegister &&
                     verifyFunction(*ReturnFunction, *Target),
                 "return emission must copy into a0 and record its implicit PseudoRET use");

  rcc::MachineBasicBlock *TailBlock = nullptr;
  auto TailFunction = createFunction(Context, "tail_call", 6, TailBlock);
  if (!TailFunction || TailBlock == nullptr)
    return;
  auto Tail = callRequest({}, 0);
  Tail.TailKind = rcc::TailCallKind::Tail;
  Tail.TailContext = emptyTailContext();
  rcc::MachineIRBuilder TailBuilder(*TailFunction, *Target);
  if (!TailBuilder.setInsertPoint(*TailBlock))
    return;
  auto TailLowered = rcc::riscv::callLowering().lowerCall(TailBuilder, Tail);
  Context.expect(TailLowered && instructions(*TailBlock).size() == 1 &&
                     TailBlock->firstInstruction()->opcode() ==
                         opcode(rcc::riscv::InstructionOpcode::PseudoTAIL) &&
                     TailFunction->frameInfo().state().HasCalls &&
                     TailFunction->frameInfo().state().HasTailCall &&
                     !TailFunction->frameInfo().state().AdjustsStack &&
                     verifyFunction(*TailFunction, *Target),
                 "tail emission must avoid a private call frame and mark function frame state");

  rcc::MachineBasicBlock *StackTailBlock = nullptr;
  auto StackTailFunction = createFunction(Context, "stack_tail_call", 8, StackTailBlock);
  if (!StackTailFunction || StackTailBlock == nullptr)
    return;
  std::vector<rcc::ABIValue> StackTailArguments;
  StackTailArguments.reserve(9);
  for (std::uint32_t Index = 0; Index < 9; ++Index) {
    StackTailArguments.push_back(
        abiValue(integer(64), Index, virtualRegister(*StackTailFunction, integer(64))));
  }
  auto StackTail = callRequest(std::move(StackTailArguments), 9);
  StackTail.TailKind = rcc::TailCallKind::Tail;
  auto StackTailContext = emptyTailContext();
  StackTailContext.CallerFixedArgumentCount = 9;
  StackTailContext.ReusableIncomingArgumentBytes = 16;
  StackTail.TailContext = std::move(StackTailContext);
  rcc::MachineIRBuilder StackTailBuilder(*StackTailFunction, *Target);
  if (!StackTailBuilder.setInsertPoint(*StackTailBlock))
    return;
  auto StackTailLowered = rcc::riscv::callLowering().lowerCall(StackTailBuilder, StackTail);
  Context.expect(
      StackTailLowered &&
          findInstruction(*StackTailBlock, rcc::riscv::InstructionOpcode::PseudoSTORE_STACK_ARG) !=
              nullptr &&
          findInstruction(*StackTailBlock, rcc::riscv::InstructionOpcode::ADJCALLSTACKDOWN) ==
              nullptr &&
          StackTailFunction->frameInfo().state().HasTailCall &&
          !StackTailFunction->frameInfo().state().AdjustsStack &&
          verifyFunction(*StackTailFunction, *Target),
      "a stack-argument tail call must reuse fixed incoming slots without claiming an SP change");
}

void testFormalStackAlignmentAndPreflight(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return;

  rcc::MachineBasicBlock *StackBlock = nullptr;
  auto StackFunction = createFunction(Context, "stack_formal", 9, StackBlock);
  if (!StackFunction || StackBlock == nullptr)
    return;
  std::vector<rcc::ABIValue> StackArguments;
  StackArguments.reserve(9);
  for (std::uint32_t Index = 0; Index < 9; ++Index) {
    StackArguments.push_back(
        abiValue(integer(64), Index, virtualRegister(*StackFunction, integer(64))));
  }
  rcc::FormalArgumentRequest StackRequest;
  StackRequest.Convention = convention();
  StackRequest.Arguments = std::move(StackArguments);
  StackRequest.FixedArgumentCount = 9;
  rcc::MachineIRBuilder StackBuilder(*StackFunction, *Target);
  if (!StackBuilder.setInsertPoint(*StackBlock))
    return;
  auto StackLowered = rcc::riscv::callLowering().lowerFormalArguments(StackBuilder, StackRequest);
  bool HasIncomingObject = false;
  for (const rcc::StackObject &Object : StackFunction->frameInfo().objects()) {
    const rcc::StackObjectDesc *Description = Object.description();
    HasIncomingObject |=
        Description != nullptr && Description->Kind == rcc::StackObjectKind::FixedABI &&
        Description->Align.bytes() == 8 && Description->Offset == 0 &&
        Description->TargetFlags ==
            static_cast<std::uint64_t>(rcc::riscv::CallStackObjectFlag::IncomingArgument);
  }
  Context.expect(
      StackLowered && StackFunction->frameInfo().liveObjectCount() == 1 && HasIncomingObject &&
          StackFunction->frameInfo().stackAlignment().bytes() == 8 &&
          findInstruction(*StackBlock, rcc::riscv::InstructionOpcode::PseudoLOAD_STACK_ARG) !=
              nullptr &&
          verifyFunction(*StackFunction, *Target),
      "non-variadic stack formals must publish fixed objects and maximum frame alignment");

  rcc::MachineBasicBlock *ConflictBlock = nullptr;
  auto ConflictFunction = createFunction(Context, "conflicting_formal", 10, ConflictBlock);
  if (!ConflictFunction || ConflictBlock == nullptr)
    return;
  const rcc::Register Existing = virtualRegister(*ConflictFunction, integer(64));
  const rcc::Register First = virtualRegister(*ConflictFunction, integer(64));
  const rcc::Register Second = virtualRegister(*ConflictFunction, integer(64));
  const rcc::PhysicalRegister ConflictingRegister = argumentGPR(1);
  if (!ConflictFunction->registerInfo().addLiveIn(ConflictingRegister, Existing) ||
      !ConflictBlock->addLiveIn(ConflictingRegister, rcc::LaneMask::all())) {
    Context.expect(false, "formal live-in conflict fixture must be valid");
    return;
  }
  rcc::FormalArgumentRequest ConflictRequest;
  ConflictRequest.Convention = convention();
  ConflictRequest.Arguments = {abiValue(integer(64), 0, First), abiValue(integer(64), 1, Second)};
  ConflictRequest.FixedArgumentCount = 2;
  rcc::MachineIRBuilder ConflictBuilder(*ConflictFunction, *Target);
  if (!ConflictBuilder.setInsertPoint(*ConflictBlock))
    return;
  auto ConflictLowered =
      rcc::riscv::callLowering().lowerFormalArguments(ConflictBuilder, ConflictRequest);
  Context.expect(
      !ConflictLowered && ConflictBlock->instructionCount() == 0 &&
          ConflictFunction->allInstructions().empty() &&
          ConflictFunction->registerInfo().liveIns().size() == 1 &&
          ConflictBlock->liveIns().size() == 1 &&
          ConflictFunction->frameInfo().liveObjectCount() == 0,
      "formal live-in conflicts must fail preflight without partially modifying MachineIR");
}

void testPlanningFailureDoesNotMutate(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  rcc::MachineBasicBlock *Block = nullptr;
  auto Function = createFunction(Context, "failed_call", 7, Block);
  if (!Target || !Function || Block == nullptr)
    return;
  const rcc::Register Value = virtualRegister(*Function, integer(32));
  auto Request = callRequest({abiValue(integer(32), 0, Value)}, 1);
  rcc::MachineIRBuilder Builder(*Function, *Target);
  if (!Builder.setInsertPoint(*Block))
    return;
  auto Lowered = rcc::riscv::callLowering().lowerCall(Builder, Request);
  auto UnknownPhysical = rcc::PhysicalRegister::create(999);
  auto UnknownRequest =
      UnknownPhysical
          ? callRequest({abiValue(integer(64), 0, rcc::Register::fromPhysical(*UnknownPhysical))},
                        1)
          : rcc::CallLoweringRequest{};
  auto UnknownLowered = UnknownPhysical
                            ? rcc::riscv::callLowering().lowerCall(Builder, UnknownRequest)
                            : rcc::Expected<void>{std::unexpected(UnknownPhysical.error())};
  Context.expect(!Lowered && !UnknownLowered && Block->instructionCount() == 0 &&
                     Function->allInstructions().empty() &&
                     Function->frameInfo().liveObjectCount() == 0,
                 "RISC-V planning and preflight failures must leave MachineIR and frame state "
                 "untouched");
}

} // namespace

int main() {
  TestContext Context;
  testIntegerAndFloatingAssignments(Context);
  testVariadicAssignments(Context);
  testSpecialArgumentsAndReturns(Context);
  testTailCalls(Context);
  testOrdinaryAndIndirectEmission(Context);
  testStackAndByValEmission(Context);
  testFormalReturnAndTailEmission(Context);
  testFormalStackAlignmentAndPreflight(Context);
  testPlanningFailureDoesNotMutate(Context);
  return Context.result();
}
