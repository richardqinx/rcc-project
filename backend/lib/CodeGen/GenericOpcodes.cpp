// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GenericOpcodes.h"

#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {
namespace {

using GO = GenericOpcode;

[[nodiscard]] Expected<void> verifyGenericExplicitOperands(Opcode InstructionOpcode,
                                                           std::span<const MachineOperand> Operands,
                                                           const MachineRegisterInfo &RegInfo);
[[nodiscard]] Expected<void> verifyGenericInstructionSemantics(const MachineInstr &Instruction,
                                                               const MachineRegisterInfo &RegInfo);

[[nodiscard]] LLTKindSet anyValueKinds() {
  return {LLT::Kind::AnyScalar,   LLT::Kind::Integer,      LLT::Kind::Float,
          LLT::Kind::Pointer,     LLT::Kind::VectorAny,    LLT::Kind::VectorInteger,
          LLT::Kind::VectorFloat, LLT::Kind::VectorPointer};
}

[[nodiscard]] LLTKindSet integerKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Integer, LLT::Kind::VectorAny, LLT::Kind::VectorInteger};
}

[[nodiscard]] LLTKindSet integerOrPointerKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Integer,       LLT::Kind::Pointer,
          LLT::Kind::VectorAny, LLT::Kind::VectorInteger, LLT::Kind::VectorPointer};
}

[[nodiscard]] LLTKindSet scalarIntegerKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Integer};
}

[[nodiscard]] LLTKindSet scalarIntegerOrPointerKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Integer, LLT::Kind::Pointer};
}

[[nodiscard]] LLTKindSet scalarValueKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Integer, LLT::Kind::Float, LLT::Kind::Pointer};
}

[[nodiscard]] LLTKindSet floatingKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Float, LLT::Kind::VectorAny, LLT::Kind::VectorFloat};
}

[[nodiscard]] LLTKindSet scalarFloatingKinds() {
  return {LLT::Kind::AnyScalar, LLT::Kind::Float};
}

[[nodiscard]] LLTKindSet pointerKinds() {
  return {LLT::Kind::Pointer, LLT::Kind::VectorPointer};
}

[[nodiscard]] LLTKindSet scalarPointerKinds() {
  return {LLT::Kind::Pointer};
}

[[nodiscard]] LLTKindSet vectorKinds() {
  return {LLT::Kind::VectorAny, LLT::Kind::VectorInteger, LLT::Kind::VectorFloat,
          LLT::Kind::VectorPointer};
}

[[nodiscard]] OperandKindSet allOperandKinds() {
  return {MachineOperand::Kind::Register,
          MachineOperand::Kind::Immediate,
          MachineOperand::Kind::IntegerImmediate,
          MachineOperand::Kind::FloatingImmediate,
          MachineOperand::Kind::BasicBlock,
          MachineOperand::Kind::FrameIndex,
          MachineOperand::Kind::ConstantPoolIndex,
          MachineOperand::Kind::JumpTableIndex,
          MachineOperand::Kind::GlobalSymbol,
          MachineOperand::Kind::ExternalSymbol,
          MachineOperand::Kind::BlockAddress,
          MachineOperand::Kind::RegisterMask,
          MachineOperand::Kind::LiveOutRegisterMask,
          MachineOperand::Kind::Metadata,
          MachineOperand::Kind::TargetIndex,
          MachineOperand::Kind::MCSymbol,
          MachineOperand::Kind::CFIIndex,
          MachineOperand::Kind::Intrinsic,
          MachineOperand::Kind::Predicate,
          MachineOperand::Kind::ShuffleMask,
          MachineOperand::Kind::DebugInstrRef,
          MachineOperand::Kind::LaneMask};
}

[[nodiscard]] OperandDescriptor typedRegister(RegisterOperandRole Role, LLTKindSet Kinds,
                                              std::uint16_t Group) {
  OperandDescriptor Result;
  Result.Kinds = {MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.TypeConstraint = OperandTypeConstraint{Kinds, Group};
  return Result;
}

[[nodiscard]] OperandDescriptor untypedRegister(RegisterOperandRole Role, bool AllowSubReg = true) {
  OperandDescriptor Result;
  Result.Kinds = {MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.AllowSubReg = AllowSubReg;
  return Result;
}

[[nodiscard]] OperandDescriptor operand(OperandKindSet Kinds) {
  OperandDescriptor Result;
  Result.Kinds = Kinds;
  if (Kinds.contains(MachineOperand::Kind::Register)) {
    Result.RegisterRole = RegisterOperandRole::Any;
    Result.AllowSubReg = true;
  }
  return Result;
}

[[nodiscard]] OperandDescriptor kind(MachineOperand::Kind Kind) {
  return operand(OperandKindSet{Kind});
}

[[nodiscard]] OperandDescriptor immediate() {
  return operand({MachineOperand::Kind::Immediate, MachineOperand::Kind::IntegerImmediate});
}

[[nodiscard]] OperandDescriptor symbol() {
  return operand({MachineOperand::Kind::GlobalSymbol, MachineOperand::Kind::ExternalSymbol,
                  MachineOperand::Kind::MCSymbol});
}

[[nodiscard]] OperandDescriptor anyOperand() {
  return operand(allOperandKinds());
}

[[nodiscard]] OperandDescriptor repeated(OperandDescriptor Descriptor) {
  Descriptor.VariadicGroup = true;
  return Descriptor;
}

[[nodiscard]] OperandDescriptor registeredTargetFlags(OperandDescriptor Descriptor) {
  Descriptor.AllowAnyTargetFlags = true;
  return Descriptor;
}

class DescriptorRegistry final {
public:
  DescriptorRegistry() {
    Descriptors.reserve(CorePseudoOpcodeCount + GenericOpcodeCount);
    Configured.reserve(CorePseudoOpcodeCount + GenericOpcodeCount);
#define RCC_CORE_OPCODE(Name) add(GO::Name, #Name, false);
#define RCC_GENERIC_OPCODE(Name) add(GO::Name, #Name, true);
#include "rcc/CodeGen/GenericOpcode.def"
#undef RCC_GENERIC_OPCODE
#undef RCC_CORE_OPCODE
  }

  void configure(std::initializer_list<GO> Opcodes, const std::vector<OperandDescriptor> &Operands,
                 std::size_t NumDefs, std::initializer_list<InstructionFlag> Flags = {},
                 bool Variadic = false, std::size_t MinimumRepetitions = 0,
                 bool RequiresMemoryOperands = false, PseudoOwner Owner = PseudoOwner::None,
                 PipelineStage Deadline = PipelineStage::Emission,
                 bool AllowExtraImplicitOperands = false) {
    for (const GO OpcodeValue : Opcodes) {
      const std::size_t Index = index(OpcodeValue);
      if (Index >= Descriptors.size() || Configured[Index])
        std::terminate();
      Configured[Index] = true;
      InstructionDescriptor &Descriptor = Descriptors[Index];
      Descriptor.Operands = Operands;
      Descriptor.NumExplicitDefs = NumDefs;
      Descriptor.Variadic = Variadic;
      Descriptor.MinimumVariadicRepetitions = MinimumRepetitions;
      Descriptor.Flags = {};
      Descriptor.Flags.set(isGenericOpcode(Descriptor.InstructionOpcode) ? InstructionFlag::Generic
                                                                         : InstructionFlag::Pseudo);
      Descriptor.Flags.set(InstructionFlag::AllowedPreLegalizer);
      for (const InstructionFlag Flag : Flags)
        Descriptor.Flags.set(Flag);
      Descriptor.RequiresMemoryOperands = RequiresMemoryOperands;
      Descriptor.Owner = Owner;
      Descriptor.Deadline = Deadline;
      Descriptor.AllowExtraImplicitOperands = AllowExtraImplicitOperands;
      if (isGenericOpcode(Descriptor.InstructionOpcode)) {
        Descriptor.VerifyExplicitOperands = &verifyGenericExplicitOperands;
        Descriptor.VerifyInstructionSemantics = &verifyGenericInstructionSemantics;
      }
    }
  }

  void addFlag(std::initializer_list<GO> Opcodes, InstructionFlag Flag) {
    for (const GO OpcodeValue : Opcodes) {
      const std::size_t Index = index(OpcodeValue);
      if (Index >= Descriptors.size() || !Configured[Index])
        std::terminate();
      Descriptors[Index].Flags.set(Flag);
    }
  }

  void tie(GO OpcodeValue, std::size_t Left, std::size_t Right) {
    InstructionDescriptor &Descriptor = Descriptors[index(OpcodeValue)];
    if (!Configured[index(OpcodeValue)] || Left >= Descriptor.Operands.size() ||
        Right >= Descriptor.Operands.size()) {
      std::terminate();
    }
    Descriptor.Operands[Left].DefaultTiedOperand = Right;
    Descriptor.Operands[Right].DefaultTiedOperand = Left;
  }

  [[nodiscard]] std::vector<InstructionDescriptor> take() {
    if (!std::ranges::all_of(Configured, [](bool Value) { return Value; }))
      std::terminate();
    return std::move(Descriptors);
  }

private:
  static std::size_t index(GO OpcodeValue) {
    const std::size_t Value = static_cast<std::size_t>(OpcodeValue);
    if (Value < static_cast<std::size_t>(GO::CORE_PSEUDO_END))
      return Value - static_cast<std::size_t>(GO::PHI);
    return CorePseudoOpcodeCount + Value - static_cast<std::size_t>(GO::G_ASSERT_SEXT);
  }

  void add(GO OpcodeValue, std::string_view Name, bool IsGeneric) {
    InstructionDescriptor Descriptor;
    Descriptor.InstructionOpcode = opcode(OpcodeValue);
    Descriptor.Name = Name;
    if (IsGeneric != isGenericOpcode(Descriptor.InstructionOpcode))
      std::terminate();
    Descriptors.push_back(std::move(Descriptor));
    Configured.push_back(false);
  }

  std::vector<InstructionDescriptor> Descriptors;
  std::vector<bool> Configured;
};

[[nodiscard]] Expected<void> semanticError(std::string_view Message) {
  return makeUnexpected(ErrorCode::VerificationError, std::string(Message));
}

[[nodiscard]] std::optional<std::int64_t> plainImmediate(const MachineOperand &Operand) noexcept {
  const auto *Value = Operand.getIf<std::int64_t>();
  if (Operand.kind() != MachineOperand::Kind::Immediate || Value == nullptr)
    return std::nullopt;
  return *Value;
}

[[nodiscard]] std::uint64_t minimumBitWidth(LLT Type) noexcept {
  const std::uint64_t Elements = Type.isVector() ? Type.minimumElementCount() : 1;
  return Elements * Type.scalarBitWidth();
}

[[nodiscard]] std::uint64_t minimumStoreBytes(LLT Type) noexcept {
  return (minimumBitWidth(Type) + 7) / 8;
}

[[nodiscard]] LLT scalarElementType(LLT Type) noexcept {
  if (const auto Element = Type.scalarElementType())
    return *Element;
  return Type;
}

[[nodiscard]] bool hasSameShape(LLT Left, LLT Right) noexcept {
  if (Left.isVector() != Right.isVector())
    return false;
  if (!Left.isVector())
    return true;
  return Left.isScalableVector() == Right.isScalableVector() &&
         Left.minimumElementCount() == Right.minimumElementCount();
}

[[nodiscard]] Expected<void> requireSameShape(LLT Left, LLT Right, std::string_view Message) {
  if (!hasSameShape(Left, Right))
    return semanticError(Message);
  return {};
}

[[nodiscard]] Expected<void> requireI1WithShape(LLT Condition, LLT Value,
                                                bool AllowScalarCondition) {
  const bool IsScalarInteger = Condition.isAnyScalar() || Condition.isInteger();
  const bool IsVectorInteger = Condition.isAnyVector() || Condition.isIntegerVector();
  if ((!IsScalarInteger && !IsVectorInteger) || Condition.scalarBitWidth() == 0)
    return semanticError("generic condition or status result must use an integer LLT");
  if (AllowScalarCondition && !Condition.isVector())
    return {};
  return requireSameShape(Condition, Value,
                          "generic condition or status vector shape does not match its value");
}

[[nodiscard]] Expected<void> verifyGenericExplicitOperands(Opcode InstructionOpcode,
                                                           std::span<const MachineOperand> Operands,
                                                           const MachineRegisterInfo &RegInfo) {
  std::vector<std::optional<LLT>> Types(Operands.size());
  bool SawNonDefinition = false;
  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    const MachineOperand &Operand = Operands[Index];
    if (!Operand.isRegister()) {
      SawNonDefinition = true;
      continue;
    }

    if (Operand.isDef() == Operand.isUse())
      return semanticError("generic register operand must be either a pure definition or use");
    if (Operand.isDef()) {
      if (SawNonDefinition)
        return semanticError("generic register definitions must form the explicit operand prefix");
    } else {
      SawNonDefinition = true;
      if (Operand.registerFlags().has(RegisterOperandFlag::Undef))
        return semanticError("generic virtual-register uses cannot carry the undef flag");
    }

    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      return semanticError("generic instruction register operands must be virtual registers");
    auto Info = RegInfo.virtualRegisterInfo(*Reg);
    if (!Info)
      return std::unexpected(Info.error());
    const std::optional<LLT> &Type = Info->get().Type;
    if (!Type)
      return semanticError("generic virtual register is missing its LLT");
    Types[Index] = Type.value();
  }

  const auto TypeAt = [&Types](std::size_t Index) -> LLT { return *Types[Index]; };
  const GO Generic = static_cast<GO>(InstructionOpcode.value());

  const auto VerifyWidthChange = [&](bool IsExtension) -> Expected<void> {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    if (auto Shape =
            requireSameShape(Dst, Src, "generic width conversion must preserve vector shape");
        !Shape) {
      return Shape;
    }
    if ((IsExtension && Dst.scalarBitWidth() <= Src.scalarBitWidth()) ||
        (!IsExtension && Dst.scalarBitWidth() >= Src.scalarBitWidth())) {
      return semanticError(IsExtension
                               ? "generic extension destination must be wider than its source"
                               : "generic truncation destination must be narrower than its source");
    }
    return {};
  };

  const auto VerifyImmediateBitWidth = [&](std::size_t ImmediateIndex, std::size_t TypeIndex,
                                           bool StrictlySmaller) -> Expected<void> {
    const auto Value = plainImmediate(Operands[ImmediateIndex]);
    if (!Value || *Value <= 0)
      return semanticError("generic bit-width immediate must be a positive machine immediate");
    const std::uint64_t Width = TypeAt(TypeIndex).scalarBitWidth();
    if ((StrictlySmaller && static_cast<std::uint64_t>(*Value) >= Width) ||
        (!StrictlySmaller && static_cast<std::uint64_t>(*Value) > Width)) {
      return semanticError("generic bit-width immediate exceeds its register element width");
    }
    return {};
  };

  switch (Generic) {
  case GO::G_ASSERT_SEXT:
  case GO::G_ASSERT_ZEXT:
  case GO::G_SEXT_INREG:
    return VerifyImmediateBitWidth(2, 1, true);
  case GO::G_ASSERT_ALIGN: {
    const auto AlignmentValue = plainImmediate(Operands[2]);
    if (!AlignmentValue || *AlignmentValue <= 0 ||
        !std::has_single_bit(static_cast<std::uint64_t>(*AlignmentValue))) {
      return semanticError("G_ASSERT_ALIGN requires a positive power-of-two alignment");
    }
    return {};
  }
  case GO::G_CONSTANT: {
    if (const auto *Payload = Operands[1].getIf<IntegerImmediateOperand>();
        Payload != nullptr && Payload->Value.bitWidth() != TypeAt(0).scalarBitWidth()) {
      return semanticError("G_CONSTANT integer width must match its destination LLT");
    }
    return {};
  }
  case GO::G_FCONSTANT: {
    const auto *Payload = Operands[1].getIf<FloatingImmediateOperand>();
    if (Payload == nullptr)
      return semanticError("G_FCONSTANT requires a floating-point immediate");
    const LLT DestinationType = TypeAt(0);
    const std::optional<FloatSemantics> DestinationSemantics = DestinationType.floatSemantics();
    if ((DestinationSemantics && Payload->Bits.semantics() != *DestinationSemantics) ||
        (!DestinationSemantics && Payload->Bits.bitWidth() != DestinationType.scalarBitWidth())) {
      return semanticError("G_FCONSTANT semantics must match its destination LLT");
    }
    return {};
  }
  case GO::G_BITCAST: {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    if (Dst.isPointerOrPointerVector() != Src.isPointerOrPointerVector())
      return semanticError("G_BITCAST cannot cross the pointer/non-pointer boundary");
    if (minimumBitWidth(Dst) != minimumBitWidth(Src) ||
        Dst.isScalableVector() != Src.isScalableVector()) {
      return semanticError("G_BITCAST source and destination sizes must match");
    }
    if (Dst == Src)
      return semanticError("G_BITCAST must change the LLT");
    return {};
  }
  case GO::G_INTTOPTR:
  case GO::G_PTRTOINT:
    return requireSameShape(TypeAt(0), TypeAt(1),
                            "pointer/integer conversion must preserve vector shape");
  case GO::G_ADDRSPACE_CAST: {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    if (auto Shape = requireSameShape(Dst, Src, "G_ADDRSPACE_CAST must preserve vector shape");
        !Shape) {
      return Shape;
    }
    if (Dst.addressSpace() == Src.addressSpace())
      return semanticError("G_ADDRSPACE_CAST must change the address space");
    return {};
  }
  case GO::G_ANYEXT:
  case GO::G_SEXT:
  case GO::G_ZEXT:
  case GO::G_FPEXT:
    return VerifyWidthChange(true);
  case GO::G_TRUNC:
  case GO::G_TRUNC_SSAT_S:
  case GO::G_TRUNC_SSAT_U:
  case GO::G_TRUNC_USAT_U:
  case GO::G_FPTRUNC:
    return VerifyWidthChange(false);
  case GO::G_PTR_ADD:
  case GO::G_PTRMASK:
    return requireSameShape(TypeAt(1), TypeAt(2),
                            "pointer arithmetic operands must have matching scalar/vector shape");
  case GO::G_ICMP:
  case GO::G_FCMP:
  case GO::G_STRICT_FCMP:
  case GO::G_STRICT_FCMPS: {
    const PredicateOperand *Predicate = Operands[1].getIf<PredicateOperand>();
    if (Predicate == nullptr || ((Generic == GO::G_ICMP) != Predicate->isInteger())) {
      return semanticError("generic comparison predicate kind does not match its opcode");
    }
    return requireI1WithShape(TypeAt(0), TypeAt(2), false);
  }
  case GO::G_SCMP:
  case GO::G_UCMP:
    if (TypeAt(0).scalarBitWidth() < 2)
      return semanticError("G_SCMP/G_UCMP result elements must be at least two bits wide");
    return requireSameShape(TypeAt(0), TypeAt(1),
                            "G_SCMP/G_UCMP must preserve scalar/vector shape");
  case GO::G_SELECT:
    return requireI1WithShape(TypeAt(1), TypeAt(0), true);
  case GO::G_UADDO:
  case GO::G_USUBO:
  case GO::G_SADDO:
  case GO::G_SSUBO:
  case GO::G_UMULO:
  case GO::G_SMULO:
  case GO::G_UADDE:
  case GO::G_USUBE:
  case GO::G_SADDE:
  case GO::G_SSUBE:
    return requireI1WithShape(TypeAt(1), TypeAt(0), false);
  case GO::G_CTTZ:
  case GO::G_CTTZ_ZERO_POISON:
  case GO::G_CTLZ:
  case GO::G_CTLZ_ZERO_POISON:
  case GO::G_CTLS:
  case GO::G_CTPOP:
    return requireSameShape(TypeAt(0), TypeAt(1),
                            "generic bit-count operation must preserve scalar/vector shape");
  case GO::G_BSWAP:
    if ((TypeAt(0).scalarBitWidth() % 16) != 0)
      return semanticError("G_BSWAP element width must be a multiple of 16 bits");
    return {};
  case GO::G_FPTOSI:
  case GO::G_FPTOUI:
  case GO::G_FPTOSI_SAT:
  case GO::G_FPTOUI_SAT:
  case GO::G_SITOFP:
  case GO::G_UITOFP:
  case GO::G_LROUND:
  case GO::G_LLROUND:
    return requireSameShape(TypeAt(0), TypeAt(1),
                            "generic numeric conversion must preserve scalar/vector shape");
  case GO::G_FCOPYSIGN:
    return requireSameShape(TypeAt(0), TypeAt(2),
                            "G_FCOPYSIGN operands must have matching scalar/vector shape");
  case GO::G_IS_FPCLASS: {
    if (auto Shape = requireI1WithShape(TypeAt(0), TypeAt(1), false); !Shape)
      return Shape;
    const auto Classes = plainImmediate(Operands[2]);
    if (!Classes || *Classes < 0 || *Classes > 0x3ff)
      return semanticError("G_IS_FPCLASS class mask must fit the supported ten-bit mask");
    return {};
  }
  case GO::G_FPOWI:
  case GO::G_FLDEXP:
  case GO::G_STRICT_FLDEXP:
    return requireSameShape(TypeAt(0), TypeAt(2),
                            "floating operation exponent must match the result vector shape");
  case GO::G_FFREXP:
    return requireSameShape(TypeAt(0), TypeAt(1),
                            "G_FFREXP mantissa and exponent results must have matching shape");
  case GO::G_MERGE_VALUES: {
    if (TypeAt(0).isVector() || TypeAt(1).isVector())
      return semanticError("G_MERGE_VALUES only merges scalar values");
    if (minimumBitWidth(TypeAt(0)) != minimumBitWidth(TypeAt(1)) * (Operands.size() - 1))
      return semanticError("G_MERGE_VALUES source widths do not fill the destination");
    return {};
  }
  case GO::G_UNMERGE_VALUES: {
    const std::size_t DestinationCount = Operands.size() - 1;
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(DestinationCount);
    if (minimumBitWidth(Src) != minimumBitWidth(Dst) * DestinationCount)
      return semanticError("G_UNMERGE_VALUES destination widths do not cover the source");
    if (Dst.isVector() && (!Src.isVector() || Dst.isScalableVector() != Src.isScalableVector() ||
                           scalarElementType(Dst) != scalarElementType(Src))) {
      return semanticError("vector G_UNMERGE_VALUES operands have incompatible element types");
    }
    return {};
  }
  case GO::G_EXTRACT: {
    const auto Offset = plainImmediate(Operands[2]);
    const std::uint64_t DstBits = minimumBitWidth(TypeAt(0));
    const std::uint64_t SrcBits = minimumBitWidth(TypeAt(1));
    if (!Offset || *Offset < 0 || DstBits >= SrcBits ||
        static_cast<std::uint64_t>(*Offset) > SrcBits - DstBits) {
      return semanticError("G_EXTRACT offset and result width must stay inside its source");
    }
    return {};
  }
  case GO::G_INSERT: {
    const auto Offset = plainImmediate(Operands[3]);
    const std::uint64_t DstBits = minimumBitWidth(TypeAt(0));
    const std::uint64_t InsertBits = minimumBitWidth(TypeAt(2));
    if (!Offset || *Offset < 0 || InsertBits >= DstBits ||
        static_cast<std::uint64_t>(*Offset) > DstBits - InsertBits) {
      return semanticError("G_INSERT offset and inserted width must stay inside its result");
    }
    return {};
  }
  case GO::G_BUILD_VECTOR:
  case GO::G_BUILD_VECTOR_TRUNC: {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    if (!Dst.isVector() || Dst.isScalableVector() || Src.isVector())
      return semanticError("G_BUILD_VECTOR requires a fixed vector result and scalar sources");
    if (Dst.minimumElementCount() != Operands.size() - 1)
      return semanticError("G_BUILD_VECTOR source count must equal the result lane count");
    const LLT Element = scalarElementType(Dst);
    if (Generic == GO::G_BUILD_VECTOR && Element != Src)
      return semanticError("G_BUILD_VECTOR source LLT must equal the result element LLT");
    if (Generic == GO::G_BUILD_VECTOR_TRUNC && Src.scalarBitWidth() <= Element.scalarBitWidth())
      return semanticError("G_BUILD_VECTOR_TRUNC sources must be wider than result elements");
    return {};
  }
  case GO::G_CONCAT_VECTORS: {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    if (!Dst.isVector() || !Src.isVector() || Dst.isScalableVector() != Src.isScalableVector() ||
        scalarElementType(Dst) != scalarElementType(Src) ||
        Dst.minimumElementCount() != Src.minimumElementCount() * (Operands.size() - 1)) {
      return semanticError("G_CONCAT_VECTORS operands do not exactly tile the result vector");
    }
    return {};
  }
  case GO::G_VSCALE: {
    const auto *Scale = Operands[1].getIf<IntegerImmediateOperand>();
    if (TypeAt(0).isVector() || Scale == nullptr || Scale->Value.isZero() ||
        Scale->Value.bitWidth() != TypeAt(0).scalarBitWidth()) {
      return semanticError("G_VSCALE requires a nonzero typed constant matching a scalar result");
    }
    return {};
  }
  case GO::G_SHL:
  case GO::G_LSHR:
  case GO::G_ASHR:
  case GO::G_ROTR:
  case GO::G_ROTL:
  case GO::G_USHLSAT:
  case GO::G_SSHLSAT:
  case GO::G_FSHL:
  case GO::G_FSHR:
    return requireSameShape(TypeAt(1), TypeAt(Operands.size() - 1),
                            "shift amount must match the shifted value's scalar/vector shape");
  case GO::G_INSERT_SUBVECTOR:
  case GO::G_EXTRACT_SUBVECTOR: {
    const bool IsInsert = Generic == GO::G_INSERT_SUBVECTOR;
    const LLT Outer = TypeAt(IsInsert ? 0 : 1);
    const LLT Inner = TypeAt(IsInsert ? 2 : 0);
    const auto Index = plainImmediate(Operands[IsInsert ? 3 : 2]);
    if (!Outer.isVector() || !Inner.isVector() ||
        scalarElementType(Outer) != scalarElementType(Inner) || !Index || *Index < 0) {
      return semanticError("subvector operation has incompatible vector types or index");
    }
    if ((!Outer.isScalableVector() && Inner.isScalableVector()) ||
        Inner.minimumElementCount() > Outer.minimumElementCount()) {
      return semanticError("subvector operation cannot place a larger vector inside a smaller one");
    }
    const std::uint64_t Start = static_cast<std::uint64_t>(*Index);
    const std::uint64_t InnerLength = Inner.minimumElementCount();
    const std::uint64_t OuterLength = Outer.minimumElementCount();
    const bool MixedScalability = Outer.isScalableVector() && !Inner.isScalableVector();
    if ((Start % InnerLength) != 0 || Start >= OuterLength ||
        (!MixedScalability && Start + InnerLength > OuterLength)) {
      return semanticError("subvector index is misaligned or outside the containing vector");
    }
    return {};
  }
  case GO::G_SHUFFLE_VECTOR: {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    const auto *Mask = Operands[3].getIf<ShuffleMaskOperand>();
    if (!Dst.isVector() || !Src.isVector() || Dst.isScalableVector() || Src.isScalableVector() ||
        scalarElementType(Dst) != scalarElementType(Src) || Mask == nullptr ||
        Mask->Elements.size() != Dst.minimumElementCount()) {
      return semanticError("G_SHUFFLE_VECTOR types and mask length are inconsistent");
    }
    const std::uint64_t SourceLanes = Src.minimumElementCount();
    for (const std::int32_t Element : Mask->Elements) {
      if (Element < ShuffleMaskOperand::Undef ||
          (Element >= 0 && static_cast<std::uint64_t>(Element) >= 2 * SourceLanes)) {
        return semanticError("G_SHUFFLE_VECTOR mask contains an invalid lane index");
      }
    }
    return {};
  }
  case GO::G_SPLAT_VECTOR: {
    const LLT Dst = TypeAt(0);
    const LLT Src = TypeAt(1);
    if (!Dst.isScalableVector() || Src.isVector() ||
        scalarElementType(Dst).scalarBitWidth() > Src.scalarBitWidth()) {
      return semanticError("G_SPLAT_VECTOR requires a scalable result with no wider elements");
    }
    return {};
  }
  case GO::G_STEP_VECTOR: {
    const LLT Dst = TypeAt(0);
    const auto *Step = Operands[1].getIf<IntegerImmediateOperand>();
    if (!Dst.isScalableVector() || Step == nullptr || Step->Value.isZero() ||
        Step->Value.isNegative() || Step->Value.bitWidth() != Dst.scalarBitWidth()) {
      return semanticError("G_STEP_VECTOR step must be a positive typed element-width constant");
    }
    return {};
  }
  case GO::G_INSERT_VECTOR_ELT: {
    const LLT Vector = TypeAt(0);
    if (scalarElementType(Vector) != TypeAt(2) || TypeAt(3).isVector())
      return semanticError("G_INSERT_VECTOR_ELT element or index LLT is incompatible");
    return {};
  }
  case GO::G_EXTRACT_VECTOR_ELT: {
    const LLT Vector = TypeAt(1);
    if (scalarElementType(Vector) != TypeAt(0) || TypeAt(2).isVector())
      return semanticError("G_EXTRACT_VECTOR_ELT result or index LLT is incompatible");
    return {};
  }
  case GO::G_VECTOR_COMPRESS:
    return requireI1WithShape(TypeAt(2), TypeAt(0), false);
  case GO::G_VECREDUCE_SEQ_FADD:
  case GO::G_VECREDUCE_SEQ_FMUL:
    if (TypeAt(0).isVector() || TypeAt(1).isVector() || !TypeAt(2).isVector() ||
        scalarElementType(TypeAt(2)) != TypeAt(0) || TypeAt(0) != TypeAt(1)) {
      return semanticError("sequential vector reduction accumulator and element LLTs must match");
    }
    return {};
  case GO::G_VECREDUCE_FADD:
  case GO::G_VECREDUCE_FMUL:
  case GO::G_VECREDUCE_FMAX:
  case GO::G_VECREDUCE_FMIN:
  case GO::G_VECREDUCE_FMAXIMUM:
  case GO::G_VECREDUCE_FMINIMUM:
  case GO::G_VECREDUCE_ADD:
  case GO::G_VECREDUCE_MUL:
  case GO::G_VECREDUCE_AND:
  case GO::G_VECREDUCE_OR:
  case GO::G_VECREDUCE_XOR:
  case GO::G_VECREDUCE_SMAX:
  case GO::G_VECREDUCE_SMIN:
  case GO::G_VECREDUCE_UMAX:
  case GO::G_VECREDUCE_UMIN:
    if (TypeAt(0).isVector() || !TypeAt(1).isVector() ||
        scalarElementType(TypeAt(1)) != TypeAt(0)) {
      return semanticError("vector reduction result must equal the source element LLT");
    }
    return {};
  case GO::G_SBFX:
  case GO::G_UBFX:
    if (TypeAt(0).isVector() || TypeAt(2).isVector() || TypeAt(3).isVector())
      return semanticError("generic bit-field extraction only supports scalar operands");
    return {};
  case GO::G_BRCOND:
    if (!TypeAt(0).isScalarValue())
      return semanticError("G_BRCOND condition must be a scalar value");
    return {};
  case GO::G_BRJT:
    if (TypeAt(0).isVector() || TypeAt(2).isVector())
      return semanticError("G_BRJT table pointer and index must be scalar values");
    return {};
  case GO::G_INTRINSIC:
  case GO::G_INTRINSIC_W_SIDE_EFFECTS:
  case GO::G_INTRINSIC_CONVERGENT:
  case GO::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS: {
    const auto FirstUse = std::ranges::find_if_not(Operands, [](const MachineOperand &Operand) {
      return Operand.isRegister() && Operand.isDef();
    });
    if (FirstUse == Operands.end() || FirstUse->kind() != MachineOperand::Kind::Intrinsic)
      return semanticError("generic intrinsic must place its intrinsic ID after all definitions");
    return {};
  }
  case GO::G_INDEXED_LOAD:
  case GO::G_INDEXED_SEXTLOAD:
  case GO::G_INDEXED_ZEXTLOAD:
    if (const auto Mode = plainImmediate(Operands[4]); !Mode || (*Mode != 0 && *Mode != 1))
      return semanticError("generic indexed-load address mode must be immediate 0 or 1");
    return {};
  case GO::G_INDEXED_STORE:
    if (const auto Mode = plainImmediate(Operands[4]); !Mode || (*Mode != 0 && *Mode != 1))
      return semanticError("G_INDEXED_STORE address mode must be immediate 0 or 1");
    return {};
  case GO::G_ATOMIC_CMPXCHG_WITH_SUCCESS:
    return requireI1WithShape(TypeAt(1), TypeAt(0), false);
  case GO::G_FENCE: {
    const auto Ordering = plainImmediate(Operands[0]);
    const auto Scope = plainImmediate(Operands[1]);
    if (!Ordering || *Ordering < 4 || *Ordering > 7 || !Scope || *Scope < 0 ||
        static_cast<std::uint64_t>(*Scope) > std::numeric_limits<std::uint32_t>::max()) {
      return semanticError("G_FENCE requires acquire-or-stronger ordering and a uint32 scope");
    }
    return {};
  }
  case GO::G_PREFETCH: {
    const auto ReadWrite = plainImmediate(Operands[1]);
    const auto Locality = plainImmediate(Operands[2]);
    const auto CacheType = plainImmediate(Operands[3]);
    if (!ReadWrite || *ReadWrite < 0 || *ReadWrite > 1 || !Locality || *Locality < 0 ||
        *Locality > 3 || !CacheType || *CacheType < 0 || *CacheType > 1) {
      return semanticError("G_PREFETCH control operands are outside their defined ranges");
    }
    return {};
  }
  case GO::G_VAARG: {
    const auto Align = plainImmediate(Operands[2]);
    if (!Align || *Align <= 0 || !std::has_single_bit(static_cast<std::uint64_t>(*Align)))
      return semanticError("G_VAARG alignment must be a positive power of two");
    return {};
  }
  case GO::G_DYN_STACKALLOC: {
    const auto Align = plainImmediate(Operands[2]);
    if (!Align || *Align <= 0 || !std::has_single_bit(static_cast<std::uint64_t>(*Align)))
      return semanticError("G_DYN_STACKALLOC alignment must be a positive power of two");
    return {};
  }
  case GO::G_MEMCPY:
  case GO::G_MEMMOVE:
    if (const auto Tail = plainImmediate(Operands[3]); !Tail || (*Tail != 0 && *Tail != 1))
      return semanticError("generic memory-transfer tail flag must be immediate 0 or 1");
    return {};
  case GO::G_MEMSET:
    if (TypeAt(1).isVector() || TypeAt(1).scalarBitWidth() != 8)
      return semanticError("G_MEMSET fill value must be a scalar i8 value");
    if (const auto Tail = plainImmediate(Operands[3]); !Tail || (*Tail != 0 && *Tail != 1))
      return semanticError("G_MEMSET tail flag must be immediate 0 or 1");
    return {};
  case GO::G_BZERO:
    if (const auto Tail = plainImmediate(Operands[2]); !Tail || (*Tail != 0 && *Tail != 1))
      return semanticError("G_BZERO tail flag must be immediate 0 or 1");
    return {};
  case GO::G_UBSANTRAP: {
    const auto Kind = plainImmediate(Operands[0]);
    if (!Kind || *Kind < -128 || *Kind > 127)
      return semanticError("G_UBSANTRAP kind must fit in a signed eight-bit immediate");
    return {};
  }
  case GO::G_INTRINSIC_FPTRUNC_ROUND: {
    const auto Rounding = plainImmediate(Operands[2]);
    if (!Rounding || *Rounding < 0 || (*Rounding > 4 && *Rounding != 7))
      return semanticError("floating truncation rounding mode is invalid");
    return VerifyWidthChange(false);
  }
  default:
    return {};
  }
}

[[nodiscard]] Expected<void> verifyGenericInstructionSemantics(const MachineInstr &Instruction,
                                                               const MachineRegisterInfo &RegInfo) {
  const auto Operands = Instruction.operands();
  const auto MemoryOperands = Instruction.memOperands();
  const GO Generic = static_cast<GO>(Instruction.opcode().value());

  const auto TypeAt = [&](std::size_t Index) -> Expected<LLT> {
    if (Index >= Operands.size())
      return makeUnexpected(ErrorCode::InternalError,
                            "generic semantic verifier operand index is out of range");
    const Register *Reg = Operands[Index].getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      return makeUnexpected(ErrorCode::InternalError,
                            "generic semantic verifier expected a virtual register");
    auto Info = RegInfo.virtualRegisterInfo(*Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (!Info->get().Type)
      return makeUnexpected(ErrorCode::InternalError,
                            "generic semantic verifier found a register without an LLT");
    return *Info->get().Type;
  };

  const auto VerifyAddress = [&](std::size_t OperandIndex,
                                 const MachineMemOperand &Memory) -> Expected<void> {
    auto Type = TypeAt(OperandIndex);
    if (!Type)
      return std::unexpected(Type.error());
    if (!Type->isPointer() || Type->addressSpace() != Memory.description().AddressSpace)
      return semanticError("generic memory address LLT and MMO address space do not match");
    return {};
  };

  enum class SizeRelation : std::uint8_t {
    Equal,
    NoLarger,
    StrictlySmaller,
  };
  const auto VerifySize = [&](LLT ValueType, const MachineMemOperand &Memory,
                              SizeRelation Relation) -> Expected<void> {
    const MemorySize Size = Memory.description().Size;
    if (!Size.isKnown())
      return semanticError("typed generic memory access requires a known MMO size");
    if (Size.isScalable() != ValueType.isScalableVector())
      return semanticError("generic value and MMO scalability do not match");
    const std::uint64_t MemoryBytes = Size.minimumBytes();
    const std::uint64_t ValueBytes = minimumStoreBytes(ValueType);
    const bool Valid = Relation == SizeRelation::Equal
                           ? MemoryBytes == ValueBytes
                           : (Relation == SizeRelation::NoLarger ? MemoryBytes <= ValueBytes
                                                                 : MemoryBytes < ValueBytes);
    if (!Valid)
      return semanticError("generic memory access size is incompatible with its value LLT");
    return {};
  };

  const auto VerifySingleAccess = [&](std::size_t AddressIndex, std::size_t ValueIndex,
                                      bool MustLoad, bool MustStore,
                                      SizeRelation Relation) -> Expected<void> {
    if (MemoryOperands.size() != 1)
      return semanticError("generic scalar memory instruction requires exactly one MMO");
    const MachineMemOperand &Memory = *MemoryOperands.front();
    if (Memory.isLoad() != MustLoad || Memory.isStore() != MustStore)
      return semanticError("generic instruction and MMO access directions do not match");
    if (auto Address = VerifyAddress(AddressIndex, Memory); !Address)
      return Address;
    auto ValueType = TypeAt(ValueIndex);
    if (!ValueType)
      return std::unexpected(ValueType.error());
    return VerifySize(*ValueType, Memory, Relation);
  };

  switch (Generic) {
  case GO::G_LOAD:
    return VerifySingleAccess(1, 0, true, false, SizeRelation::NoLarger);
  case GO::G_SEXTLOAD:
  case GO::G_ZEXTLOAD:
  case GO::G_FPEXTLOAD:
    return VerifySingleAccess(1, 0, true, false, SizeRelation::StrictlySmaller);
  case GO::G_STORE:
    return VerifySingleAccess(1, 0, false, true, SizeRelation::NoLarger);
  case GO::G_FPTRUNCSTORE:
    return VerifySingleAccess(1, 0, false, true, SizeRelation::StrictlySmaller);
  case GO::G_INDEXED_LOAD:
    return VerifySingleAccess(2, 0, true, false, SizeRelation::NoLarger);
  case GO::G_INDEXED_SEXTLOAD:
  case GO::G_INDEXED_ZEXTLOAD:
    return VerifySingleAccess(2, 0, true, false, SizeRelation::StrictlySmaller);
  case GO::G_INDEXED_STORE:
    return VerifySingleAccess(2, 1, false, true, SizeRelation::NoLarger);
  case GO::G_ATOMIC_CMPXCHG_WITH_SUCCESS:
    if (MemoryOperands.size() != 1 ||
        MemoryOperands.front()->description().AtomicKind != AtomicOperation::CompareExchange)
      return semanticError("G_ATOMIC_CMPXCHG_WITH_SUCCESS requires one compare-exchange MMO");
    return VerifySingleAccess(2, 0, true, true, SizeRelation::NoLarger);
  case GO::G_ATOMIC_CMPXCHG:
    if (MemoryOperands.size() != 1 ||
        MemoryOperands.front()->description().AtomicKind != AtomicOperation::CompareExchange)
      return semanticError("G_ATOMIC_CMPXCHG requires one compare-exchange MMO");
    return VerifySingleAccess(1, 0, true, true, SizeRelation::NoLarger);
  case GO::G_ATOMICRMW_XCHG:
  case GO::G_ATOMICRMW_ADD:
  case GO::G_ATOMICRMW_SUB:
  case GO::G_ATOMICRMW_AND:
  case GO::G_ATOMICRMW_NAND:
  case GO::G_ATOMICRMW_OR:
  case GO::G_ATOMICRMW_XOR:
  case GO::G_ATOMICRMW_MAX:
  case GO::G_ATOMICRMW_MIN:
  case GO::G_ATOMICRMW_UMAX:
  case GO::G_ATOMICRMW_UMIN:
  case GO::G_ATOMICRMW_FADD:
  case GO::G_ATOMICRMW_FSUB:
  case GO::G_ATOMICRMW_FMAX:
  case GO::G_ATOMICRMW_FMIN:
  case GO::G_ATOMICRMW_FMAXIMUM:
  case GO::G_ATOMICRMW_FMINIMUM:
  case GO::G_ATOMICRMW_FMAXIMUMNUM:
  case GO::G_ATOMICRMW_FMINIMUMNUM:
  case GO::G_ATOMICRMW_UINC_WRAP:
  case GO::G_ATOMICRMW_UDEC_WRAP:
  case GO::G_ATOMICRMW_USUB_COND:
  case GO::G_ATOMICRMW_USUB_SAT:
    if (MemoryOperands.size() != 1 ||
        MemoryOperands.front()->description().AtomicKind != AtomicOperation::ReadModifyWrite)
      return semanticError("generic atomic RMW requires one read-modify-write MMO");
    return VerifySingleAccess(1, 0, true, true, SizeRelation::NoLarger);
  case GO::G_PREFETCH: {
    if (MemoryOperands.size() != 1)
      return semanticError("G_PREFETCH requires exactly one MMO");
    const auto ReadWrite = plainImmediate(Operands[1]);
    if (!ReadWrite)
      return semanticError("G_PREFETCH is missing its validated read/write selector");
    const MachineMemOperand &Memory = *MemoryOperands.front();
    if ((*ReadWrite == 0 && (!Memory.isLoad() || Memory.isStore())) ||
        (*ReadWrite == 1 && (Memory.isLoad() || !Memory.isStore()))) {
      return semanticError("G_PREFETCH selector and MMO access direction do not match");
    }
    return VerifyAddress(0, Memory);
  }
  case GO::G_VASTART:
    if (MemoryOperands.size() != 1 || MemoryOperands.front()->isLoad() ||
        !MemoryOperands.front()->isStore()) {
      return semanticError("G_VASTART requires exactly one store-only MMO");
    }
    return VerifyAddress(0, *MemoryOperands.front());
  case GO::G_VAARG: {
    return MemoryOperands.empty()
               ? Expected<void>{}
               : semanticError("G_VAARG must not carry MMOs before legalization");
  }
  case GO::G_MEMCPY:
  case GO::G_MEMCPY_INLINE:
  case GO::G_MEMMOVE: {
    if (MemoryOperands.size() != 2 || MemoryOperands[0]->isLoad() ||
        !MemoryOperands[0]->isStore() || !MemoryOperands[1]->isLoad() ||
        MemoryOperands[1]->isStore()) {
      return semanticError("generic memory transfer requires store and load MMOs in that order");
    }
    if (MemoryOperands[0]->description().Size != MemoryOperands[1]->description().Size)
      return semanticError("generic memory-transfer MMOs must have identical sizes");
    if (auto Destination = VerifyAddress(0, *MemoryOperands[0]); !Destination)
      return Destination;
    return VerifyAddress(1, *MemoryOperands[1]);
  }
  case GO::G_MEMSET:
  case GO::G_BZERO:
    if (MemoryOperands.size() != 1 || MemoryOperands.front()->isLoad() ||
        !MemoryOperands.front()->isStore()) {
      return semanticError("generic memory fill requires exactly one store-only MMO");
    }
    return VerifyAddress(0, *MemoryOperands.front());
  default:
    return {};
  }
}

void configureCoreOpcodes(DescriptorRegistry &Registry) {
  const auto RegDef = untypedRegister(RegisterOperandRole::Def);
  const auto RegUse = untypedRegister(RegisterOperandRole::Use);
  const auto TransferDef = registeredTargetFlags(RegDef);
  const auto TransferUse = registeredTargetFlags(RegUse);
  const auto Any = anyOperand();
  const auto VariadicAny = repeated(Any);
  const auto VariadicUse = repeated(RegUse);

  Registry.configure(
      {GO::PHI},
      {TransferDef, repeated(TransferUse), repeated(kind(MachineOperand::Kind::BasicBlock))}, 1,
      {InstructionFlag::Phi}, true, 1, false, PseudoOwner::PhiElimination,
      PipelineStage::SSAEliminated);
  Registry.configure({GO::INLINEASM}, {VariadicAny}, 0,
                     {InstructionFlag::ProducesCode, InstructionFlag::TerminalEmission,
                      InstructionFlag::UnmodeledSideEffects},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::INLINEASM_BR}, {VariadicAny}, 0,
                     {InstructionFlag::ProducesCode, InstructionFlag::TerminalEmission,
                      InstructionFlag::UnmodeledSideEffects, InstructionFlag::HasSideEffects},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::CFI_INSTRUCTION}, {kind(MachineOperand::Kind::CFIIndex)}, 0,
                     {InstructionFlag::Meta, InstructionFlag::NotDuplicable});
  Registry.configure({GO::EH_LABEL, GO::GC_LABEL, GO::ANNOTATION_LABEL},
                     {kind(MachineOperand::Kind::MCSymbol)}, 0,
                     {InstructionFlag::Meta, InstructionFlag::NotDuplicable});
  Registry.configure({GO::KILL}, {VariadicUse}, 0, {InstructionFlag::Meta}, true);
  Registry.configure({GO::EXTRACT_SUBREG}, {RegDef, RegUse, immediate()}, 1, {}, false, 0, false,
                     PseudoOwner::PostRAExpansion, PipelineStage::PostRAFinalized);
  Registry.configure({GO::INSERT_SUBREG}, {TransferDef, TransferUse, TransferUse, immediate()}, 1,
                     {}, false, 0, false, PseudoOwner::TwoAddressRewrite,
                     PipelineStage::TiedOpsRewritten);
  Registry.tie(GO::INSERT_SUBREG, 0, 1);
  Registry.configure({GO::IMPLICIT_DEF}, {TransferDef}, 1,
                     {InstructionFlag::Meta, InstructionFlag::Rematerializable});
  Registry.configure({GO::INIT_UNDEF}, {RegDef}, 1, {}, false, 0, false,
                     PseudoOwner::RegisterAllocator, PipelineStage::RegisterAllocated);
  Registry.configure({GO::SUBREG_TO_REG}, {RegDef, RegUse, immediate()}, 1, {}, false, 0, false,
                     PseudoOwner::PostRAExpansion, PipelineStage::PostRAFinalized);
  Registry.configure({GO::COPY_TO_REGCLASS}, {RegDef, RegUse, immediate()}, 1, {}, false, 0, false,
                     PseudoOwner::RegisterAllocator, PipelineStage::RegisterAllocated);
  Registry.configure({GO::DBG_VALUE, GO::DBG_VALUE_LIST, GO::DBG_INSTR_REF, GO::DBG_PHI},
                     {VariadicAny}, 0, {InstructionFlag::Meta}, true);
  Registry.configure({GO::DBG_LABEL}, {kind(MachineOperand::Kind::Metadata)}, 0,
                     {InstructionFlag::Meta});
  Registry.configure({GO::REG_SEQUENCE},
                     {TransferDef, repeated(TransferUse), repeated(immediate())}, 1, {}, true, 1,
                     false, PseudoOwner::TwoAddressRewrite, PipelineStage::TiedOpsRewritten);
  Registry.configure({GO::COPY}, {TransferDef, TransferUse}, 1, {}, false, 0, false,
                     PseudoOwner::PostRAExpansion, PipelineStage::PostRAFinalized);
  Registry.configure({GO::COPY_LANEMASK}, {RegDef, RegUse, kind(MachineOperand::Kind::LaneMask)}, 1,
                     {}, false, 0, false, PseudoOwner::PostRAExpansion,
                     PipelineStage::PostRAFinalized);
  Registry.configure({GO::BUNDLE}, {VariadicAny}, 0, {InstructionFlag::Meta}, true, 0, false,
                     PseudoOwner::None, PipelineStage::Emission, true);
  Registry.configure({GO::LIFETIME_START, GO::LIFETIME_END}, {immediate()}, 0,
                     {InstructionFlag::Meta});
  Registry.configure({GO::PSEUDO_PROBE}, {immediate(), immediate(), immediate(), immediate()}, 0,
                     {InstructionFlag::Meta, InstructionFlag::HasSideEffects});
  Registry.configure({GO::ARITH_FENCE}, {RegDef, RegUse}, 1, {InstructionFlag::Meta});
  Registry.tie(GO::ARITH_FENCE, 0, 1);
  Registry.configure({GO::STACKMAP}, {immediate(), immediate(), VariadicAny}, 0,
                     {InstructionFlag::Call, InstructionFlag::MayLoad,
                      InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::FENTRY_CALL}, {}, 0,
                     {InstructionFlag::Call, InstructionFlag::MayLoad, InstructionFlag::MayStore,
                      InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     false, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure(
      {GO::PATCHPOINT},
      {RegDef, immediate(), immediate(), Any, immediate(), immediate(), VariadicAny}, 1,
      {InstructionFlag::Call, InstructionFlag::MayLoad, InstructionFlag::HasSideEffects,
       InstructionFlag::ProducesCode, InstructionFlag::TerminalEmission},
      true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::LOAD_STACK_GUARD}, {RegDef}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::Rematerializable}, false, 0, false,
                     PseudoOwner::InstructionSelect, PipelineStage::Selected);
  Registry.configure({GO::PREALLOCATED_SETUP}, {immediate()}, 0, {InstructionFlag::HasSideEffects},
                     false, 0, false, PseudoOwner::InstructionSelect, PipelineStage::Selected);
  Registry.configure({GO::PREALLOCATED_ARG}, {RegDef, immediate(), immediate()}, 1,
                     {InstructionFlag::HasSideEffects}, false, 0, false,
                     PseudoOwner::InstructionSelect, PipelineStage::Selected);
  Registry.configure({GO::STATEPOINT}, {VariadicAny}, 0,
                     {InstructionFlag::Call, InstructionFlag::MayLoad, InstructionFlag::MayStore,
                      InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::LOCAL_ESCAPE}, {symbol(), kind(MachineOperand::Kind::FrameIndex)}, 0,
                     {InstructionFlag::Meta});
  Registry.configure({GO::FAULTING_OP}, {RegDef, VariadicAny}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore,
                      InstructionFlag::HasSideEffects, InstructionFlag::Terminator,
                      InstructionFlag::Branch, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::PATCHABLE_OP}, {VariadicAny}, 0,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore,
                      InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::PATCHABLE_FUNCTION_ENTER, GO::PATCHABLE_FUNCTION_EXIT}, {}, 0,
                     {InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     false, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission);
  Registry.configure({GO::PATCHABLE_RET, GO::PATCHABLE_TAIL_CALL}, {VariadicAny}, 0,
                     {InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission, InstructionFlag::Terminator,
                      InstructionFlag::Return},
                     true, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::PATCHABLE_EVENT_CALL}, {RegUse, Any}, 0,
                     {InstructionFlag::Call, InstructionFlag::MayLoad, InstructionFlag::MayStore,
                      InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     false, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::PATCHABLE_TYPED_EVENT_CALL}, {Any, RegUse, Any}, 0,
                     {InstructionFlag::Call, InstructionFlag::MayLoad, InstructionFlag::MayStore,
                      InstructionFlag::HasSideEffects, InstructionFlag::ProducesCode,
                      InstructionFlag::TerminalEmission},
                     false, 0, false, PseudoOwner::AsmPrinter, PipelineStage::Emission, true);
  Registry.configure({GO::ICALL_BRANCH_FUNNEL}, {VariadicAny}, 0, {InstructionFlag::HasSideEffects},
                     true, 0, false, PseudoOwner::PostRAExpansion, PipelineStage::PostRAFinalized,
                     true);
  Registry.configure({GO::FAKE_USE}, {VariadicUse}, 0,
                     {InstructionFlag::Meta, InstructionFlag::FakeUse}, true);
  Registry.configure({GO::MEMBARRIER}, {}, 0,
                     {InstructionFlag::Meta, InstructionFlag::HasSideEffects});
  Registry.configure({GO::JUMP_TABLE_DEBUG_INFO}, {kind(MachineOperand::Kind::JumpTableIndex)}, 0,
                     {InstructionFlag::Meta});
  Registry.configure({GO::RELOC_NONE}, {symbol()}, 0,
                     {InstructionFlag::Meta, InstructionFlag::HasSideEffects});
  Registry.configure({GO::CONVERGENCECTRL_ENTRY, GO::CONVERGENCECTRL_ANCHOR}, {RegDef}, 1,
                     {InstructionFlag::Meta, InstructionFlag::Convergent});
  Registry.configure({GO::CONVERGENCECTRL_LOOP}, {RegDef, RegUse}, 1,
                     {InstructionFlag::Meta, InstructionFlag::Convergent});
  Registry.configure({GO::CONVERGENCECTRL_GLUE}, {RegUse}, 0,
                     {InstructionFlag::Meta, InstructionFlag::Convergent});
}

void configureGenericOpcodes(DescriptorRegistry &Registry) {
  const LLTKindSet AnyTypes = anyValueKinds();
  const LLTKindSet IntTypes = integerKinds();
  const LLTKindSet IntegerOrPointers = integerOrPointerKinds();
  const LLTKindSet ScalarValues = scalarValueKinds();
  const LLTKindSet ScalarInts = scalarIntegerKinds();
  const LLTKindSet ScalarIntegerOrPointers = scalarIntegerOrPointerKinds();
  const LLTKindSet FloatTypes = floatingKinds();
  const LLTKindSet ScalarFloats = scalarFloatingKinds();
  const LLTKindSet PointerTypes = pointerKinds();
  const LLTKindSet ScalarPointers = scalarPointerKinds();
  const LLTKindSet VectorTypes = vectorKinds();

  const auto AnyDef0 = typedRegister(RegisterOperandRole::Def, AnyTypes, 0);
  const auto AnyUse0 = typedRegister(RegisterOperandRole::Use, AnyTypes, 0);
  const auto AnyUse1 = typedRegister(RegisterOperandRole::Use, AnyTypes, 1);
  const auto ScalarDef0 = typedRegister(RegisterOperandRole::Def, ScalarValues, 0);
  const auto ScalarUse0 = typedRegister(RegisterOperandRole::Use, ScalarValues, 0);
  const auto IntDef0 = typedRegister(RegisterOperandRole::Def, IntTypes, 0);
  const auto IntDef1 = typedRegister(RegisterOperandRole::Def, IntTypes, 1);
  const auto IntUse0 = typedRegister(RegisterOperandRole::Use, IntTypes, 0);
  const auto IntUse1 = typedRegister(RegisterOperandRole::Use, IntTypes, 1);
  const auto IntegerOrPointerUse1 = typedRegister(RegisterOperandRole::Use, IntegerOrPointers, 1);
  const auto ScalarIntDef0 = typedRegister(RegisterOperandRole::Def, ScalarInts, 0);
  const auto ScalarIntDef1 = typedRegister(RegisterOperandRole::Def, ScalarInts, 1);
  const auto ScalarIntUse0 = typedRegister(RegisterOperandRole::Use, ScalarInts, 0);
  const auto ScalarIntUse1 = typedRegister(RegisterOperandRole::Use, ScalarInts, 1);
  const auto ScalarIntUse2 = typedRegister(RegisterOperandRole::Use, ScalarInts, 2);
  const auto FloatDef0 = typedRegister(RegisterOperandRole::Def, FloatTypes, 0);
  const auto FloatUse0 = typedRegister(RegisterOperandRole::Use, FloatTypes, 0);
  const auto FloatUse1 = typedRegister(RegisterOperandRole::Use, FloatTypes, 1);
  const auto ScalarFloatDef0 = typedRegister(RegisterOperandRole::Def, ScalarFloats, 0);
  const auto ScalarFloatUse0 = typedRegister(RegisterOperandRole::Use, ScalarFloats, 0);
  const auto PointerDef0 = typedRegister(RegisterOperandRole::Def, PointerTypes, 0);
  const auto PointerUse0 = typedRegister(RegisterOperandRole::Use, PointerTypes, 0);
  const auto PointerUse1 = typedRegister(RegisterOperandRole::Use, PointerTypes, 1);
  const auto ScalarPointerDef0 = typedRegister(RegisterOperandRole::Def, ScalarPointers, 0);
  const auto ScalarPointerDef1 = typedRegister(RegisterOperandRole::Def, ScalarPointers, 1);
  const auto ScalarPointerUse0 = typedRegister(RegisterOperandRole::Use, ScalarPointers, 0);
  const auto ScalarPointerUse1 = typedRegister(RegisterOperandRole::Use, ScalarPointers, 1);
  const auto ScalarPointerUse2 = typedRegister(RegisterOperandRole::Use, ScalarPointers, 2);

  Registry.configure({GO::G_ASSERT_SEXT, GO::G_ASSERT_ZEXT}, {IntDef0, IntUse0, immediate()}, 1);
  Registry.configure({GO::G_ASSERT_ALIGN}, {AnyDef0, AnyUse0, immediate()}, 1);

  Registry.configure(
      {GO::G_ADD,       GO::G_SUB,      GO::G_MUL,       GO::G_SDIV,     GO::G_UDIV,  GO::G_SREM,
       GO::G_UREM,      GO::G_AND,      GO::G_OR,        GO::G_XOR,      GO::G_ABDS,  GO::G_ABDU,
       GO::G_UAVGFLOOR, GO::G_UAVGCEIL, GO::G_SAVGFLOOR, GO::G_SAVGCEIL, GO::G_UMULH, GO::G_SMULH,
       GO::G_UADDSAT,   GO::G_SADDSAT,  GO::G_USUBSAT,   GO::G_SSUBSAT,  GO::G_SMIN,  GO::G_SMAX,
       GO::G_UMIN,      GO::G_UMAX,     GO::G_CLMUL},
      {IntDef0, IntUse0, IntUse0}, 1);
  Registry.addFlag({GO::G_ADD, GO::G_MUL, GO::G_AND, GO::G_OR, GO::G_XOR, GO::G_ABDS, GO::G_ABDU,
                    GO::G_UMULH, GO::G_SMULH, GO::G_UADDSAT, GO::G_SADDSAT, GO::G_SMIN, GO::G_SMAX,
                    GO::G_UMIN, GO::G_UMAX, GO::G_CLMUL},
                   InstructionFlag::Commutable);

  Registry.configure({GO::G_SDIVREM, GO::G_UDIVREM}, {IntDef0, IntDef0, IntUse0, IntUse0}, 2);
  const auto TransferDef0 = registeredTargetFlags(AnyDef0);
  const auto TransferUse0 = registeredTargetFlags(AnyUse0);
  Registry.configure({GO::G_IMPLICIT_DEF}, {TransferDef0}, 1);
  Registry.configure(
      {GO::G_PHI},
      {TransferDef0, repeated(TransferUse0), repeated(kind(MachineOperand::Kind::BasicBlock))}, 1,
      {InstructionFlag::Phi}, true, 1);
  Registry.configure({GO::G_FRAME_INDEX},
                     {ScalarPointerDef0, kind(MachineOperand::Kind::FrameIndex)}, 1);
  Registry.configure({GO::G_GLOBAL_VALUE},
                     {ScalarPointerDef0, kind(MachineOperand::Kind::GlobalSymbol)}, 1);
  Registry.configure(
      {GO::G_PTRAUTH_GLOBAL_VALUE},
      {ScalarPointerDef0, ScalarPointerUse1, immediate(), ScalarIntUse2, immediate()}, 1);
  Registry.configure({GO::G_CONSTANT_POOL},
                     {ScalarPointerDef0, kind(MachineOperand::Kind::ConstantPoolIndex)}, 1);
  Registry.configure({GO::G_EXTRACT}, {AnyDef0, AnyUse1, immediate()}, 1);
  Registry.configure({GO::G_UNMERGE_VALUES}, {AnyDef0, repeated(AnyDef0), AnyUse1}, 2, {}, true, 1);
  Registry.configure({GO::G_INSERT}, {AnyDef0, AnyUse0, AnyUse1, immediate()}, 1);
  Registry.configure({GO::G_MERGE_VALUES}, {AnyDef0, AnyUse1, repeated(AnyUse1)}, 1, {}, true, 1);

  const auto VectorDef0 = typedRegister(RegisterOperandRole::Def, VectorTypes, 0);
  Registry.configure({GO::G_BUILD_VECTOR, GO::G_BUILD_VECTOR_TRUNC},
                     {VectorDef0, AnyUse1, repeated(AnyUse1)}, 1, {}, true);
  Registry.configure({GO::G_CONCAT_VECTORS},
                     {VectorDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 1),
                      repeated(typedRegister(RegisterOperandRole::Use, VectorTypes, 1))},
                     1, {}, true, 1);

  Registry.configure({GO::G_PTRTOINT}, {IntDef0, PointerUse1}, 1);
  Registry.configure({GO::G_INTTOPTR}, {PointerDef0, IntUse1}, 1);
  Registry.configure({GO::G_BITCAST}, {AnyDef0, AnyUse1}, 1);
  Registry.configure({GO::G_FREEZE, GO::G_CONSTANT_FOLD_BARRIER}, {AnyDef0, AnyUse0}, 1);
  Registry.configure({GO::G_INTRINSIC_FPTRUNC_ROUND}, {FloatDef0, FloatUse1, immediate()}, 1);
  Registry.configure({GO::G_INTRINSIC_TRUNC, GO::G_INTRINSIC_ROUND, GO::G_INTRINSIC_ROUNDEVEN},
                     {FloatDef0, FloatUse0}, 1);
  Registry.configure({GO::G_INTRINSIC_LRINT, GO::G_INTRINSIC_LLRINT}, {IntDef0, FloatUse1}, 1);
  Registry.configure({GO::G_READCYCLECOUNTER, GO::G_READSTEADYCOUNTER},
                     {typedRegister(RegisterOperandRole::Def, ScalarInts, 0)}, 1,
                     {InstructionFlag::HasSideEffects});

  Registry.configure({GO::G_LOAD}, {AnyDef0, ScalarPointerUse1}, 1, {InstructionFlag::MayLoad},
                     false, 0, true);
  Registry.configure({GO::G_SEXTLOAD, GO::G_ZEXTLOAD}, {IntDef0, ScalarPointerUse1}, 1,
                     {InstructionFlag::MayLoad}, false, 0, true);
  Registry.configure({GO::G_FPEXTLOAD}, {FloatDef0, ScalarPointerUse1}, 1,
                     {InstructionFlag::MayLoad}, false, 0, true);
  Registry.configure({GO::G_INDEXED_LOAD},
                     {AnyDef0, ScalarPointerDef1, ScalarPointerUse1, ScalarIntUse2, immediate()}, 2,
                     {InstructionFlag::MayLoad}, false, 0, true);
  Registry.configure({GO::G_INDEXED_SEXTLOAD, GO::G_INDEXED_ZEXTLOAD},
                     {IntDef0, ScalarPointerDef1, ScalarPointerUse1, ScalarIntUse2, immediate()}, 2,
                     {InstructionFlag::MayLoad}, false, 0, true);
  Registry.configure({GO::G_STORE}, {AnyUse0, ScalarPointerUse1}, 0, {InstructionFlag::MayStore},
                     false, 0, true);
  Registry.configure({GO::G_FPTRUNCSTORE}, {FloatUse0, ScalarPointerUse1}, 0,
                     {InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_INDEXED_STORE},
                     {ScalarPointerDef0, AnyUse1, ScalarPointerUse0, ScalarIntUse2, immediate()}, 1,
                     {InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_ATOMIC_CMPXCHG_WITH_SUCCESS},
                     {ScalarDef0, ScalarIntDef1, ScalarPointerUse2, ScalarUse0, ScalarUse0}, 2,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_ATOMIC_CMPXCHG},
                     {ScalarDef0, ScalarPointerUse1, ScalarUse0, ScalarUse0}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_ATOMICRMW_XCHG}, {ScalarDef0, ScalarPointerUse1, ScalarUse0}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_ATOMICRMW_ADD, GO::G_ATOMICRMW_SUB, GO::G_ATOMICRMW_AND,
                      GO::G_ATOMICRMW_NAND, GO::G_ATOMICRMW_OR, GO::G_ATOMICRMW_XOR,
                      GO::G_ATOMICRMW_MAX, GO::G_ATOMICRMW_MIN, GO::G_ATOMICRMW_UMAX,
                      GO::G_ATOMICRMW_UMIN, GO::G_ATOMICRMW_UINC_WRAP, GO::G_ATOMICRMW_UDEC_WRAP,
                      GO::G_ATOMICRMW_USUB_COND, GO::G_ATOMICRMW_USUB_SAT},
                     {ScalarIntDef0, ScalarPointerUse1, ScalarIntUse0}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_ATOMICRMW_FADD, GO::G_ATOMICRMW_FSUB, GO::G_ATOMICRMW_FMAX,
                      GO::G_ATOMICRMW_FMIN, GO::G_ATOMICRMW_FMAXIMUM, GO::G_ATOMICRMW_FMINIMUM,
                      GO::G_ATOMICRMW_FMAXIMUMNUM, GO::G_ATOMICRMW_FMINIMUMNUM},
                     {ScalarFloatDef0, ScalarPointerUse1, ScalarFloatUse0}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_FENCE}, {immediate(), immediate()}, 0,
                     {InstructionFlag::HasSideEffects});
  Registry.configure(
      {GO::G_PREFETCH}, {ScalarPointerUse0, immediate(), immediate(), immediate()}, 0,
      {InstructionFlag::MayLoad, InstructionFlag::MayStore, InstructionFlag::HasSideEffects}, false,
      0, true);
  Registry.configure({GO::G_BRCOND}, {ScalarIntUse0, kind(MachineOperand::Kind::BasicBlock)}, 0,
                     {InstructionFlag::Terminator, InstructionFlag::Branch});
  Registry.configure({GO::G_BRINDIRECT}, {ScalarPointerUse0}, 0,
                     {InstructionFlag::Terminator, InstructionFlag::Branch,
                      InstructionFlag::IndirectBranch, InstructionFlag::Barrier});
  Registry.configure({GO::G_INVOKE_REGION_START}, {}, 0, {InstructionFlag::Terminator});
  Registry.configure({GO::G_INTRINSIC, GO::G_INTRINSIC_W_SIDE_EFFECTS, GO::G_INTRINSIC_CONVERGENT,
                      GO::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS},
                     {repeated(anyOperand())}, 0, {}, true, 1, false, PseudoOwner::None,
                     PipelineStage::Emission, true);
  Registry.addFlag({GO::G_INTRINSIC_W_SIDE_EFFECTS, GO::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS},
                   InstructionFlag::HasSideEffects);
  Registry.addFlag({GO::G_INTRINSIC_W_SIDE_EFFECTS, GO::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS},
                   InstructionFlag::MayLoad);
  Registry.addFlag({GO::G_INTRINSIC_W_SIDE_EFFECTS, GO::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS},
                   InstructionFlag::MayStore);
  Registry.addFlag({GO::G_INTRINSIC_CONVERGENT, GO::G_INTRINSIC_CONVERGENT_W_SIDE_EFFECTS},
                   InstructionFlag::Convergent);

  Registry.configure({GO::G_ANYEXT, GO::G_SEXT, GO::G_ZEXT}, {IntDef0, IntUse1}, 1);
  Registry.configure({GO::G_TRUNC, GO::G_TRUNC_SSAT_S, GO::G_TRUNC_SSAT_U, GO::G_TRUNC_USAT_U},
                     {IntDef0, IntUse1}, 1);
  Registry.configure(
      {GO::G_CONSTANT},
      {typedRegister(RegisterOperandRole::Def, ScalarIntegerOrPointers, 0),
       operand({MachineOperand::Kind::Immediate, MachineOperand::Kind::IntegerImmediate})},
      1);
  Registry.configure({GO::G_FCONSTANT},
                     {ScalarFloatDef0, kind(MachineOperand::Kind::FloatingImmediate)}, 1);
  Registry.configure({GO::G_VASTART}, {ScalarPointerUse0}, 0, {InstructionFlag::MayStore}, false, 0,
                     true);
  Registry.configure({GO::G_VAARG}, {AnyDef0, ScalarPointerUse1, immediate()}, 1,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, false);
  Registry.configure({GO::G_SEXT_INREG}, {IntDef0, IntUse0, immediate()}, 1);
  Registry.configure(
      {GO::G_SHL, GO::G_LSHR, GO::G_ASHR, GO::G_ROTR, GO::G_ROTL, GO::G_USHLSAT, GO::G_SSHLSAT},
      {IntDef0, IntUse0, IntUse1}, 1);
  Registry.configure({GO::G_FSHL, GO::G_FSHR}, {IntDef0, IntUse0, IntUse0, IntUse1}, 1);
  Registry.configure(
      {GO::G_ICMP},
      {IntDef0, kind(MachineOperand::Kind::Predicate), IntegerOrPointerUse1, IntegerOrPointerUse1},
      1);
  Registry.configure({GO::G_FCMP},
                     {IntDef0, kind(MachineOperand::Kind::Predicate), FloatUse1, FloatUse1}, 1);
  Registry.configure({GO::G_SCMP, GO::G_UCMP}, {IntDef0, IntUse1, IntUse1}, 1);
  Registry.configure({GO::G_SELECT}, {AnyDef0, IntUse1, AnyUse0, AnyUse0}, 1);

  Registry.configure({GO::G_UADDO, GO::G_USUBO, GO::G_SADDO, GO::G_SSUBO, GO::G_UMULO, GO::G_SMULO},
                     {IntDef0, IntDef1, IntUse0, IntUse0}, 2);
  Registry.configure({GO::G_UADDE, GO::G_USUBE, GO::G_SADDE, GO::G_SSUBE},
                     {IntDef0, IntDef1, IntUse0, IntUse0, IntUse1}, 2);
  Registry.addFlag({GO::G_UADDO, GO::G_SADDO, GO::G_UMULO, GO::G_SMULO},
                   InstructionFlag::Commutable);
  Registry.configure({GO::G_SMULFIX, GO::G_UMULFIX, GO::G_SMULFIXSAT, GO::G_UMULFIXSAT,
                      GO::G_SDIVFIX, GO::G_UDIVFIX, GO::G_SDIVFIXSAT, GO::G_UDIVFIXSAT},
                     {IntDef0, IntUse0, IntUse0, immediate()}, 1);
  Registry.addFlag({GO::G_SMULFIX, GO::G_UMULFIX, GO::G_SMULFIXSAT, GO::G_UMULFIXSAT},
                   InstructionFlag::Commutable);

  Registry.configure({GO::G_FNEG,   GO::G_FABS,  GO::G_FCANONICALIZE, GO::G_FEXP,   GO::G_FEXP2,
                      GO::G_FEXP10, GO::G_FLOG,  GO::G_FLOG2,         GO::G_FLOG10, GO::G_FCEIL,
                      GO::G_FCOS,   GO::G_FSIN,  GO::G_FTAN,          GO::G_FACOS,  GO::G_FASIN,
                      GO::G_FATAN,  GO::G_FCOSH, GO::G_FSINH,         GO::G_FTANH,  GO::G_FSQRT,
                      GO::G_FFLOOR, GO::G_FRINT, GO::G_FNEARBYINT},
                     {FloatDef0, FloatUse0}, 1);
  Registry.configure({GO::G_FPEXT, GO::G_FPTRUNC}, {FloatDef0, FloatUse1}, 1);
  Registry.configure({GO::G_FPTOSI, GO::G_FPTOUI, GO::G_FPTOSI_SAT, GO::G_FPTOUI_SAT},
                     {IntDef0, FloatUse1}, 1);
  Registry.configure({GO::G_SITOFP, GO::G_UITOFP}, {FloatDef0, IntUse1}, 1);
  Registry.configure({GO::G_FCOPYSIGN}, {FloatDef0, FloatUse0, FloatUse1}, 1);
  Registry.configure({GO::G_IS_FPCLASS}, {IntDef0, FloatUse1, immediate()}, 1);
  Registry.configure({GO::G_FADD, GO::G_FSUB, GO::G_FMUL, GO::G_FDIV, GO::G_FREM, GO::G_FPOW,
                      GO::G_FATAN2, GO::G_FMINNUM, GO::G_FMAXNUM, GO::G_FMINNUM_IEEE,
                      GO::G_FMAXNUM_IEEE, GO::G_FMINIMUM, GO::G_FMAXIMUM, GO::G_FMINIMUMNUM,
                      GO::G_FMAXIMUMNUM},
                     {FloatDef0, FloatUse0, FloatUse0}, 1);
  Registry.addFlag({GO::G_FADD, GO::G_FMUL, GO::G_FMINNUM, GO::G_FMAXNUM, GO::G_FMINNUM_IEEE,
                    GO::G_FMAXNUM_IEEE, GO::G_FMINIMUM, GO::G_FMAXIMUM, GO::G_FMINIMUMNUM,
                    GO::G_FMAXIMUMNUM},
                   InstructionFlag::Commutable);
  Registry.configure({GO::G_FMA, GO::G_FMAD}, {FloatDef0, FloatUse0, FloatUse0, FloatUse0}, 1);
  Registry.configure({GO::G_FMODF, GO::G_FSINCOS}, {FloatDef0, FloatDef0, FloatUse0}, 2);
  Registry.configure({GO::G_FPOWI, GO::G_FLDEXP}, {FloatDef0, FloatUse0, IntUse1}, 1);
  Registry.configure({GO::G_FFREXP}, {FloatDef0, IntDef1, FloatUse0}, 2);

  Registry.configure({GO::G_GET_FPENV, GO::G_GET_FPMODE}, {AnyDef0}, 1,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_SET_FPENV, GO::G_SET_FPMODE}, {AnyUse0}, 0,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_RESET_FPENV, GO::G_RESET_FPMODE}, {}, 0,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_GET_ROUNDING}, {IntDef0}, 1, {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_SET_ROUNDING}, {IntUse0}, 0, {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_PTR_ADD}, {PointerDef0, PointerUse0, IntUse1}, 1);
  Registry.configure({GO::G_PTRMASK}, {PointerDef0, PointerUse0, IntUse1}, 1);
  Registry.configure({GO::G_ABS}, {IntDef0, IntUse0}, 1);
  Registry.configure({GO::G_LROUND, GO::G_LLROUND}, {IntDef0, FloatUse1}, 1);
  Registry.configure(
      {GO::G_BR}, {kind(MachineOperand::Kind::BasicBlock)}, 0,
      {InstructionFlag::Terminator, InstructionFlag::Branch, InstructionFlag::Barrier});
  Registry.configure({GO::G_BRJT},
                     {ScalarPointerUse0, kind(MachineOperand::Kind::JumpTableIndex), ScalarIntUse1},
                     0,
                     {InstructionFlag::Terminator, InstructionFlag::Branch,
                      InstructionFlag::IndirectBranch, InstructionFlag::Barrier});
  Registry.configure({GO::G_VSCALE}, {ScalarIntDef0, immediate()}, 1);

  Registry.configure({GO::G_INSERT_SUBVECTOR},
                     {VectorDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 0),
                      typedRegister(RegisterOperandRole::Use, VectorTypes, 1), immediate()},
                     1);
  Registry.configure(
      {GO::G_EXTRACT_SUBVECTOR},
      {VectorDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 1), immediate()}, 1);
  Registry.configure(
      {GO::G_INSERT_VECTOR_ELT},
      {VectorDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 0), AnyUse1, ScalarIntUse2},
      1);
  Registry.configure(
      {GO::G_EXTRACT_VECTOR_ELT},
      {AnyDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 1), ScalarIntUse2}, 1);
  Registry.configure({GO::G_SHUFFLE_VECTOR},
                     {VectorDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 1),
                      typedRegister(RegisterOperandRole::Use, VectorTypes, 1),
                      kind(MachineOperand::Kind::ShuffleMask)},
                     1);
  Registry.configure({GO::G_SPLAT_VECTOR}, {VectorDef0, AnyUse1}, 1);
  Registry.configure({GO::G_STEP_VECTOR}, {VectorDef0, immediate()}, 1);
  Registry.configure({GO::G_VECTOR_COMPRESS},
                     {VectorDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 0),
                      typedRegister(RegisterOperandRole::Use, VectorTypes, 1),
                      typedRegister(RegisterOperandRole::Use, VectorTypes, 0)},
                     1);

  Registry.configure({GO::G_CTTZ, GO::G_CTTZ_ZERO_POISON, GO::G_CTLZ, GO::G_CTLZ_ZERO_POISON,
                      GO::G_CTLS, GO::G_CTPOP, GO::G_BSWAP, GO::G_BITREVERSE},
                     {IntDef0, IntUse0}, 1);
  Registry.configure({GO::G_ADDRSPACE_CAST}, {PointerDef0, PointerUse1}, 1);
  Registry.configure({GO::G_BLOCK_ADDR},
                     {ScalarPointerDef0, kind(MachineOperand::Kind::BlockAddress)}, 1);
  Registry.configure({GO::G_JUMP_TABLE},
                     {ScalarPointerDef0, kind(MachineOperand::Kind::JumpTableIndex)}, 1);
  Registry.configure({GO::G_DYN_STACKALLOC}, {ScalarPointerDef0, ScalarIntUse1, immediate()}, 1,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_STACKSAVE}, {ScalarPointerDef0}, 1, {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_STACKRESTORE}, {ScalarPointerUse0}, 0,
                     {InstructionFlag::HasSideEffects});

  Registry.configure({GO::G_STRICT_FADD, GO::G_STRICT_FSUB, GO::G_STRICT_FMUL, GO::G_STRICT_FDIV,
                      GO::G_STRICT_FREM},
                     {FloatDef0, FloatUse0, FloatUse0}, 1, {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_STRICT_FMA}, {FloatDef0, FloatUse0, FloatUse0, FloatUse0}, 1,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_STRICT_FSQRT}, {FloatDef0, FloatUse0}, 1,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_STRICT_FLDEXP}, {FloatDef0, FloatUse0, IntUse1}, 1,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_STRICT_FCMP, GO::G_STRICT_FCMPS},
                     {IntDef0, kind(MachineOperand::Kind::Predicate), FloatUse1, FloatUse1}, 1,
                     {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_READ_REGISTER}, {AnyDef0, kind(MachineOperand::Kind::Metadata)}, 1,
                     {InstructionFlag::HasSideEffects, InstructionFlag::Convergent});
  Registry.configure({GO::G_WRITE_REGISTER}, {kind(MachineOperand::Kind::Metadata), AnyUse0}, 0,
                     {InstructionFlag::HasSideEffects, InstructionFlag::Convergent});
  Registry.configure({GO::G_MEMCPY, GO::G_MEMMOVE},
                     {ScalarPointerUse0, ScalarPointerUse1, ScalarIntUse2, immediate()}, 0,
                     {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_MEMCPY_INLINE}, {ScalarPointerUse0, ScalarPointerUse1, ScalarIntUse2},
                     0, {InstructionFlag::MayLoad, InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_MEMSET}, {ScalarPointerUse0, ScalarIntUse1, ScalarIntUse2, immediate()},
                     0, {InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_BZERO}, {ScalarPointerUse0, ScalarIntUse1, immediate()}, 0,
                     {InstructionFlag::MayStore}, false, 0, true);
  Registry.configure({GO::G_TRAP}, {}, 0,
                     {InstructionFlag::HasSideEffects, InstructionFlag::MayStore});
  Registry.configure({GO::G_DEBUGTRAP}, {}, 0, {InstructionFlag::HasSideEffects});
  Registry.configure({GO::G_UBSANTRAP}, {immediate()}, 0, {InstructionFlag::HasSideEffects});

  Registry.configure({GO::G_VECREDUCE_SEQ_FADD, GO::G_VECREDUCE_SEQ_FMUL},
                     {ScalarFloatDef0, typedRegister(RegisterOperandRole::Use, ScalarFloats, 1),
                      typedRegister(RegisterOperandRole::Use, VectorTypes, 2)},
                     1);
  Registry.configure({GO::G_VECREDUCE_FADD, GO::G_VECREDUCE_FMUL, GO::G_VECREDUCE_FMAX,
                      GO::G_VECREDUCE_FMIN, GO::G_VECREDUCE_FMAXIMUM, GO::G_VECREDUCE_FMINIMUM},
                     {ScalarFloatDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 1)}, 1);
  Registry.configure({GO::G_VECREDUCE_ADD, GO::G_VECREDUCE_MUL, GO::G_VECREDUCE_AND,
                      GO::G_VECREDUCE_OR, GO::G_VECREDUCE_XOR, GO::G_VECREDUCE_SMAX,
                      GO::G_VECREDUCE_SMIN, GO::G_VECREDUCE_UMAX, GO::G_VECREDUCE_UMIN},
                     {ScalarIntDef0, typedRegister(RegisterOperandRole::Use, VectorTypes, 1)}, 1);
  Registry.configure({GO::G_SBFX, GO::G_UBFX},
                     {ScalarIntDef0, ScalarIntUse0, ScalarIntUse1, ScalarIntUse1}, 1);
}

[[nodiscard]] std::vector<InstructionDescriptor> buildDescriptors() {
  DescriptorRegistry Registry;
  configureCoreOpcodes(Registry);
  configureGenericOpcodes(Registry);
  return Registry.take();
}

[[nodiscard]] const std::vector<InstructionDescriptor> &descriptors() {
  static const std::vector<InstructionDescriptor> Value = buildDescriptors();
  return Value;
}

} // namespace

std::string_view getGenericOpcodeName(GenericOpcode OpcodeValue) noexcept {
  const Opcode Value = opcode(OpcodeValue);
  const auto Position =
      std::ranges::find_if(descriptors(), [Value](const InstructionDescriptor &Descriptor) {
        return Descriptor.InstructionOpcode == Value;
      });
  return Position == descriptors().end() ? std::string_view{} : std::string_view(Position->Name);
}

std::span<const InstructionDescriptor> genericInstructionDescriptors() noexcept {
  return descriptors();
}

std::span<const ImmediatePredicate> genericImmediatePredicates() noexcept {
  static const std::vector<ImmediatePredicate> Predicates;
  return Predicates;
}

Expected<void> appendGenericInstructionInfo(std::vector<InstructionDescriptor> &Descriptors,
                                            std::vector<ImmediatePredicate> &Predicates) {
  const auto ExistingOpcode = [&Descriptors](Opcode Value) {
    return std::ranges::any_of(Descriptors, [Value](const InstructionDescriptor &Descriptor) {
      return Descriptor.InstructionOpcode == Value;
    });
  };
  for (const InstructionDescriptor &Descriptor : descriptors()) {
    if (ExistingOpcode(Descriptor.InstructionOpcode)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target instruction set already contains a target-independent opcode");
    }
    Descriptors.push_back(Descriptor);
  }
  for (const ImmediatePredicate &Predicate : genericImmediatePredicates())
    Predicates.push_back(Predicate);
  return {};
}

} // namespace rcc
