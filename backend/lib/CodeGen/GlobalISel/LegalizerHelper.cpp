// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/LegalizerHelper.h"

#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetCallLowering.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc {
namespace {

struct ScalarLayout final {
  std::uint64_t StoreSize = 0;
  std::uint64_t AllocationSize = 0;
  std::uint64_t ABIAlignment = 0;
};

[[nodiscard]] Expected<std::int64_t> signedImmediate(const MachineOperand &Operand) {
  if (const std::int64_t *Value = Operand.getIf<std::int64_t>())
    return *Value;
  const auto *Integer = Operand.getIf<IntegerImmediateOperand>();
  if (Integer == nullptr || Integer->Value.bitWidth() == 0 || Integer->Value.bitWidth() > 64) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "legalizer requires an immediate no wider than 64 bits");
  }
  std::uint64_t Raw = Integer->Value.words().empty() ? 0 : Integer->Value.words().front();
  const std::uint32_t Width = Integer->Value.bitWidth();
  if (Width < 64 && (Raw & (std::uint64_t{1} << (Width - 1U))) != 0)
    Raw |= ~std::uint64_t{0} << Width;
  return std::bit_cast<std::int64_t>(Raw);
}

[[nodiscard]] std::optional<ScalarLayout> scalarLayout(LowLevelType Type) noexcept {
  if (!Type.isScalarValue() || Type.scalarBitWidth() == 0)
    return std::nullopt;
  const std::uint64_t StoreSize = (Type.scalarBitWidth() + 7U) / 8U;
  const std::uint64_t AllocationSize = std::bit_ceil(StoreSize);
  return ScalarLayout{StoreSize, AllocationSize, std::min<std::uint64_t>(AllocationSize, 8)};
}

[[nodiscard]] Expected<const MachineMemOperand *> createMemoryAccess(MachineFunction &Function,
                                                                     MemoryAccessFlag Access,
                                                                     LowLevelType Type,
                                                                     const ScalarLayout &Layout) {
  auto Size = MemorySize::fixed(Layout.StoreSize);
  auto Align = Alignment::create(Layout.ABIAlignment);
  if (!Size || !Align)
    return makeUnexpected(ErrorCode::InternalError,
                          "legalizer cannot represent scalar memory layout");
  MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.MemoryType = Type;
  Description.Align = *Align;
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] Expected<Register> resultRegister(MachineInstr &Instruction) {
  const MachineOperand *Definition = Instruction.operandAt(0);
  const Register *Result = Definition == nullptr ? nullptr : Definition->getIf<Register>();
  if (Result == nullptr || !Definition->isDef()) {
    return makeUnexpected(ErrorCode::InternalError,
                          "machine IR builder returned an instruction without a register result");
  }
  return *Result;
}

[[nodiscard]] Expected<void> setInsertPointAtFirstTerminator(MachineIRBuilder &Builder,
                                                             MachineBasicBlock &Block) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor =
        Builder.target().instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI predecessor contains an unknown instruction");
    if (Descriptor->Flags.has(InstructionFlag::Terminator))
      return Builder.setInsertPointBefore(*Instruction);
  }
  return Builder.setInsertPoint(Block);
}

[[nodiscard]] Expected<void> setInsertPointAfterPHIs(MachineIRBuilder &Builder,
                                                     MachineBasicBlock &Block) {
  for (MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    const InstructionDescriptor *Descriptor =
        Builder.target().instructionDescriptor(Instruction->opcode());
    if (Descriptor == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "PHI block contains an unknown instruction");
    if (!Descriptor->Flags.has(InstructionFlag::Phi) &&
        !Descriptor->Flags.has(InstructionFlag::Meta)) {
      return Builder.setInsertPointBefore(*Instruction);
    }
  }
  return Builder.setInsertPoint(Block);
}

[[nodiscard]] Expected<Register> buildConstantRegister(MachineIRBuilder &Builder, LowLevelType Type,
                                                       APInt Value) {
  auto Built = Builder.buildConstant(Type, std::move(Value));
  if (!Built)
    return std::unexpected(Built.error());
  return resultRegister(**Built);
}

[[nodiscard]] Expected<Register> buildConstantRegister(MachineIRBuilder &Builder, LowLevelType Type,
                                                       std::int64_t Value) {
  auto Immediate = APInt::fromSigned(Type.scalarBitWidth(), Value);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  return buildConstantRegister(Builder, Type, std::move(*Immediate));
}

[[nodiscard]] Expected<Register> buildResultInstruction(MachineIRBuilder &Builder,
                                                        GenericOpcode Generic, LowLevelType Type,
                                                        std::initializer_list<Register> Sources) {
  auto Built = Builder.buildInstr(opcode(Generic), {Type}, Sources);
  if (!Built)
    return std::unexpected(Built.error());
  return resultRegister(**Built);
}

[[nodiscard]] Expected<Register>
buildFlaggedBinaryInstruction(MachineIRBuilder &Builder, GenericOpcode Generic, LowLevelType Type,
                              Register Left, Register Right, MachineInstrFlags Flags) {
  auto Destination = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Pending = Builder.buildInstr(opcode(Generic));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Left); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Right); !Added)
    return std::unexpected(Added.error());
  if (auto Set = Pending->setFlags(Flags); !Set)
    return std::unexpected(Set.error());
  if (auto Built = Pending->build(); !Built)
    return std::unexpected(Built.error());
  return *Destination;
}

[[nodiscard]] Expected<std::int64_t> addMemoryOffset(std::int64_t Offset,
                                                     std::uint64_t ByteOffset) {
  if (ByteOffset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory split offset exceeds the representable machine memory range");
  }
  const auto SignedByteOffset = static_cast<std::int64_t>(ByteOffset);
  if (Offset > std::numeric_limits<std::int64_t>::max() - SignedByteOffset) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory split offset exceeds the representable machine memory range");
  }
  return Offset + SignedByteOffset;
}

[[nodiscard]] Expected<const MachineMemOperand *>
deriveMemoryOperand(MachineFunction &Function, const MachineMemOperand &Original,
                    LowLevelType MemoryType, std::uint64_t ByteOffset) {
  if (!MemoryType.isScalarValue() || MemoryType.minimumBitWidth() == 0 ||
      MemoryType.minimumBitWidth() % 8U != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory splitting requires a byte-sized scalar memory type");
  }

  const std::uint64_t ByteSize = MemoryType.minimumBitWidth() / 8U;
  auto Size = MemorySize::fixed(ByteSize);
  if (!Size)
    return std::unexpected(Size.error());

  MachineMemOperandDesc Description = Original.description();
  auto Offset = addMemoryOffset(Description.Offset, ByteOffset);
  if (!Offset)
    return std::unexpected(Offset.error());
  Description.Offset = *Offset;
  if (Description.OffsetRange) {
    auto Lower = addMemoryOffset(Description.OffsetRange->Lower, ByteOffset);
    auto Upper = addMemoryOffset(Description.OffsetRange->Upper, ByteOffset);
    if (!Lower)
      return std::unexpected(Lower.error());
    if (!Upper)
      return std::unexpected(Upper.error());
    Description.OffsetRange = MemoryOffsetRange{*Lower, *Upper};
  }

  const std::optional<Alignment> BaseAlignment = Description.BaseAlign;
  std::uint64_t EffectiveAlignment = BaseAlignment.value_or(Description.Align).bytes();
  if (Description.Offset != 0) {
    const auto OffsetBits = static_cast<std::uint64_t>(Description.Offset);
    const std::uint64_t OffsetAlignment = std::uint64_t{1} << std::countr_zero(OffsetBits);
    EffectiveAlignment = std::min(EffectiveAlignment, OffsetAlignment);
  }
  auto AlignmentValue = Alignment::create(EffectiveAlignment);
  if (!AlignmentValue)
    return std::unexpected(AlignmentValue.error());

  Description.Size = *Size;
  Description.MemoryType = MemoryType;
  Description.Align = *AlignmentValue;
  Description.BaseAlign = BaseAlignment;
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] Expected<Register>
buildObjectPointerOffset(MachineIRBuilder &Builder, Register Pointer, std::uint64_t ByteOffset) {
  if (ByteOffset == 0)
    return Pointer;

  auto PointerType = Builder.function().registerInfo().getType(Pointer);
  if (!PointerType)
    return std::unexpected(PointerType.error());
  if (!PointerType->isPointer()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory splitting requires a pointer address operand");
  }
  const std::uint32_t PointerBits = PointerType->scalarBitWidth();
  if (PointerBits < 64U && (ByteOffset >> PointerBits) != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory split offset does not fit the pointer index type");
  }
  auto OffsetType = LowLevelType::makeCategoryNeutralScalar(PointerBits);
  if (!OffsetType)
    return std::unexpected(OffsetType.error());
  auto OffsetValue = APInt::fromUnsigned(PointerBits, ByteOffset);
  if (!OffsetValue)
    return std::unexpected(OffsetValue.error());
  auto Offset = buildConstantRegister(Builder, *OffsetType, std::move(*OffsetValue));
  if (!Offset)
    return std::unexpected(Offset.error());
  return buildFlaggedBinaryInstruction(
      Builder, GenericOpcode::G_PTR_ADD, *PointerType, Pointer, *Offset,
      MachineInstrFlags{MachineInstrFlag::NoUnsignedWrap, MachineInstrFlag::InBounds});
}

[[nodiscard]] Expected<APInt> repeatedByte(std::uint32_t BitWidth, std::uint8_t Byte) {
  if (BitWidth == 0 || BitWidth > APInt::MaxBitWidth || BitWidth % 8U != 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "repeated-byte integer has an invalid bit width");
  constexpr std::uint64_t ByteSplat = 0x0101010101010101ULL;
  std::vector<std::uint64_t> Words((static_cast<std::size_t>(BitWidth) + 63U) / 64U,
                                   ByteSplat * Byte);
  const std::uint32_t HighBits = BitWidth % 64U;
  if (HighBits != 0)
    Words.back() &= (std::uint64_t{1} << HighBits) - 1U;
  return APInt::create(BitWidth, Words);
}

[[nodiscard]] Expected<APInt> oneBitSet(std::uint32_t BitWidth, std::uint32_t Bit) {
  if (BitWidth == 0 || BitWidth > APInt::MaxBitWidth || Bit >= BitWidth)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "single-bit integer has an invalid bit position");
  std::vector<std::uint64_t> Words((static_cast<std::size_t>(BitWidth) + 63U) / 64U, 0);
  Words[Bit / 64U] = std::uint64_t{1} << (Bit % 64U);
  return APInt::create(BitWidth, Words);
}

[[nodiscard]] Expected<Register> buildICmpRegister(MachineIRBuilder &Builder,
                                                   IntegerPredicate Predicate,
                                                   LowLevelType ResultType, Register Left,
                                                   Register Right) {
  auto Built = Builder.buildICmp(Predicate, ResultType, Left, Right);
  if (!Built)
    return std::unexpected(Built.error());
  return resultRegister(**Built);
}

[[nodiscard]] IntegerPredicate minMaxToCompare(GenericOpcode Generic) {
  switch (Generic) {
  case GenericOpcode::G_SMIN:
    return IntegerPredicate::SignedLessThan;
  case GenericOpcode::G_SMAX:
    return IntegerPredicate::SignedGreaterThan;
  case GenericOpcode::G_UMIN:
    return IntegerPredicate::UnsignedLessThan;
  case GenericOpcode::G_UMAX:
    return IntegerPredicate::UnsignedGreaterThan;
  default:
    std::unreachable();
  }
}

[[nodiscard]] bool isEqualityPredicate(IntegerPredicate Predicate) noexcept {
  return Predicate == IntegerPredicate::Equal || Predicate == IntegerPredicate::NotEqual;
}

[[nodiscard]] Expected<IntegerPredicate> unsignedPredicate(IntegerPredicate Predicate) {
  switch (Predicate) {
  case IntegerPredicate::Equal:
  case IntegerPredicate::NotEqual:
  case IntegerPredicate::UnsignedGreaterThan:
  case IntegerPredicate::UnsignedGreaterEqual:
  case IntegerPredicate::UnsignedLessThan:
  case IntegerPredicate::UnsignedLessEqual:
    return Predicate;
  case IntegerPredicate::SignedGreaterThan:
    return IntegerPredicate::UnsignedGreaterThan;
  case IntegerPredicate::SignedGreaterEqual:
    return IntegerPredicate::UnsignedGreaterEqual;
  case IntegerPredicate::SignedLessThan:
    return IntegerPredicate::UnsignedLessThan;
  case IntegerPredicate::SignedLessEqual:
    return IntegerPredicate::UnsignedLessEqual;
  case IntegerPredicate::Count:
    return makeUnexpected(ErrorCode::VerificationError,
                          "integer comparison has an invalid predicate");
  }
  return makeUnexpected(ErrorCode::VerificationError,
                        "integer comparison has an invalid predicate");
}

[[nodiscard]] Expected<LowLevelType> booleanTypeFor(LowLevelType Type) {
  auto BooleanElementType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!BooleanElementType)
    return std::unexpected(BooleanElementType.error());
  if (!Type.isVector())
    return *BooleanElementType;
  if (Type.isScalableVector())
    return LowLevelType::makeScalableVector(Type.minimumElementCount(), *BooleanElementType);
  return LowLevelType::makeFixedVector(Type.minimumElementCount(), *BooleanElementType);
}

[[nodiscard]] Expected<Register> buildConstantBefore(MachineIRBuilder &Builder,
                                                     MachineInstr &Anchor, LowLevelType Type,
                                                     std::uint64_t Value) {
  auto Immediate = APInt::fromUnsigned(Type.scalarBitWidth(), Value);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (auto Point = Builder.setInsertPointBefore(Anchor); !Point)
    return std::unexpected(Point.error());
  return buildConstantRegister(Builder, Type, std::move(*Immediate));
}

[[nodiscard]] Expected<Register> buildBinaryBefore(MachineIRBuilder &Builder, MachineInstr &Anchor,
                                                   GenericOpcode Generic, LowLevelType Type,
                                                   Register Left, Register Right) {
  auto Result = Builder.function().registerInfo().createVirtualRegister(Type);
  if (!Result)
    return std::unexpected(Result.error());
  if (auto Point = Builder.setInsertPointBefore(Anchor); !Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(opcode(Generic));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Added = Pending->addDef(*Result); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Left); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Pending->addUse(Right); !Added)
    return std::unexpected(Added.error());
  auto Built = Pending->build();
  if (!Built)
    return std::unexpected(Built.error());
  return *Result;
}

[[nodiscard]] Expected<std::string> integerLibcallName(GenericOpcode Generic,
                                                       std::uint32_t BitWidth) {
  std::string_view Prefix;
  switch (Generic) {
  case GenericOpcode::G_MUL:
    Prefix = "__mul";
    break;
  case GenericOpcode::G_SDIV:
    Prefix = "__div";
    break;
  case GenericOpcode::G_UDIV:
    Prefix = "__udiv";
    break;
  case GenericOpcode::G_SREM:
    Prefix = "__mod";
    break;
  case GenericOpcode::G_UREM:
    Prefix = "__umod";
    break;
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "integer libcall has no runtime function mapping");
  }

  std::string_view Suffix;
  switch (BitWidth) {
  case 32:
    Suffix = "si3";
    break;
  case 64:
    Suffix = "di3";
    break;
  case 128:
    Suffix = "ti3";
    break;
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "integer libcall requires a 32, 64, or 128-bit scalar");
  }
  return std::string(Prefix) + std::string(Suffix);
}

[[nodiscard]] ABIExtensionKind integerLibcallExtension(GenericOpcode Generic,
                                                       std::uint32_t BitWidth) noexcept {
  if (BitWidth >= 64)
    return ABIExtensionKind::None;
  if (Generic == GenericOpcode::G_SDIV || Generic == GenericOpcode::G_SREM)
    return ABIExtensionKind::SignExtend;
  if (Generic == GenericOpcode::G_UDIV || Generic == GenericOpcode::G_UREM)
    return ABIExtensionKind::ZeroExtend;
  return ABIExtensionKind::AnyExtend;
}

[[nodiscard]] Expected<std::string_view> integerLibcallTypeCode(std::uint32_t BitWidth) {
  switch (BitWidth) {
  case 32:
    return "si";
  case 64:
    return "di";
  case 128:
    return "ti";
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "conversion libcall requires a 32, 64, or 128-bit integer");
  }
}

[[nodiscard]] Expected<std::string_view> floatingLibcallTypeCode(std::uint32_t BitWidth) {
  switch (BitWidth) {
  case 32:
    return "sf";
  case 64:
    return "df";
  case 128:
    return "tf";
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "conversion libcall requires a 32, 64, or 128-bit float");
  }
}

[[nodiscard]] Expected<std::string> floatingBinaryLibcallName(GenericOpcode Generic,
                                                              std::uint32_t BitWidth) {
  if (Generic != GenericOpcode::G_FREM) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "floating libcall has no runtime function mapping");
  }
  switch (BitWidth) {
  case 32:
    return "fmodf";
  case 64:
    return "fmod";
  case 128:
    return "fmodl";
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "floating libcall requires a 32, 64, or 128-bit scalar");
  }
}

[[nodiscard]] Expected<std::string> conversionLibcallName(GenericOpcode Generic,
                                                          std::uint32_t DestinationWidth,
                                                          std::uint32_t SourceWidth) {
  const bool DestinationIsFloat =
      Generic == GenericOpcode::G_SITOFP || Generic == GenericOpcode::G_UITOFP ||
      Generic == GenericOpcode::G_FPEXT || Generic == GenericOpcode::G_FPTRUNC;
  const bool SourceIsFloat =
      Generic == GenericOpcode::G_FPTOSI || Generic == GenericOpcode::G_FPTOUI ||
      Generic == GenericOpcode::G_FPEXT || Generic == GenericOpcode::G_FPTRUNC;
  auto DestinationCode = DestinationIsFloat ? floatingLibcallTypeCode(DestinationWidth)
                                            : integerLibcallTypeCode(DestinationWidth);
  auto SourceCode =
      SourceIsFloat ? floatingLibcallTypeCode(SourceWidth) : integerLibcallTypeCode(SourceWidth);
  if (!DestinationCode)
    return std::unexpected(DestinationCode.error());
  if (!SourceCode)
    return std::unexpected(SourceCode.error());

  std::string Name;
  switch (Generic) {
  case GenericOpcode::G_FPEXT:
    Name = "__extend";
    break;
  case GenericOpcode::G_FPTRUNC:
    Name = "__trunc";
    break;
  case GenericOpcode::G_FPTOSI:
    Name = "__fix";
    break;
  case GenericOpcode::G_FPTOUI:
    Name = "__fixuns";
    break;
  case GenericOpcode::G_SITOFP:
    Name = "__float";
    break;
  case GenericOpcode::G_UITOFP:
    Name = "__floatun";
    break;
  default:
    return makeUnexpected(ErrorCode::Unsupported, "opcode has no conversion libcall mapping");
  }
  Name += *SourceCode;
  Name += *DestinationCode;
  if (Generic == GenericOpcode::G_FPEXT || Generic == GenericOpcode::G_FPTRUNC)
    Name += '2';
  return Name;
}

[[nodiscard]] Expected<ABIValue>
libcallABIValue(Register Value, LowLevelType Type, ABIValueCategory Category,
                std::uint32_t OriginalValue, std::uint16_t FragmentIndex,
                std::uint16_t FragmentCount, std::uint32_t FragmentOffsetBytes,
                Alignment OriginalAlignment, ABIExtensionKind Extension) {
  auto OneByte = Alignment::create(1);
  if (!OneByte)
    return std::unexpected(OneByte.error());
  return ABIValue{
      .Value = Value,
      .Type = Type,
      .Category = Category,
      .OriginalValue = OriginalValue,
      .FragmentIndex = FragmentIndex,
      .FragmentCount = FragmentCount,
      .FragmentOffsetBytes = FragmentOffsetBytes,
      .OriginalAlignment = OriginalAlignment,
      .Extension = Extension,
      .Flags = {},
      .ByValSizeBytes = 0,
      .ByValAlignment = *OneByte,
  };
}

} // namespace

LegalizerHelper::LegalizerHelper(MachineFunction &Function, const LegalizerInfo &Info,
                                 GISelChangeObserver &Observer, MachineIRBuilder &MIRBuilder,
                                 const LibcallLoweringInfo *Libcalls)
    : MIRBuilder(MIRBuilder), Observer(Observer), Function(Function), MRI(Function.registerInfo()),
      Info(Info), Libcalls(Libcalls) {}

Expected<void> LegalizerHelper::widenScalarMergeValues(MachineInstr &Instruction,
                                                       std::size_t TypeIndex,
                                                       LowLevelType WideType) {
  if (TypeIndex != 1)
    return makeUnexpected(ErrorCode::Unsupported,
                          "G_MERGE_VALUES widening requires type index one");
  if (Instruction.operandCount() < 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_MERGE_VALUES requires a destination and sources");

  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *FirstSource = Instruction.operandAt(1)->getIf<Register>();
  if (Destination == nullptr || FirstSource == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_MERGE_VALUES requires register operands");
  auto DestinationType = MRI.getType(*Destination);
  auto SourceType = MRI.getType(*FirstSource);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (DestinationType->isVector())
    return makeUnexpected(ErrorCode::Unsupported,
                          "vector G_MERGE_VALUES widening is not supported");

  const std::uint64_t DestinationSize = DestinationType->minimumBitWidth();
  const std::uint64_t SourceSize = SourceType->minimumBitWidth();
  const std::uint64_t WideSize = WideType.minimumBitWidth();
  const std::uint64_t MergeCount = (DestinationSize + WideSize - 1U) / WideSize;
  const std::size_t SourceCount = Instruction.operandCount() - 1U;
  const std::uint64_t PartSize = DestinationSize / SourceCount;

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  if (WideSize >= DestinationSize) {
    auto BuiltResult = MIRBuilder.buildZExt(WideType, *FirstSource);
    if (!BuiltResult)
      return std::unexpected(BuiltResult.error());
    auto Result = resultRegister(**BuiltResult);
    if (!Result)
      return std::unexpected(Result.error());
    Register ResultRegister = *Result;

    for (std::size_t Index = 2; Index != Instruction.operandCount(); ++Index) {
      const Register *Source = Instruction.operandAt(Index)->getIf<Register>();
      if (Source == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_MERGE_VALUES requires register sources");
      auto ActualSourceType = MRI.getType(*Source);
      if (!ActualSourceType)
        return std::unexpected(ActualSourceType.error());
      if (*ActualSourceType != *SourceType || ActualSourceType->minimumBitWidth() != PartSize)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_MERGE_VALUES source types are inconsistent");

      auto BuiltExtendedInput = MIRBuilder.buildZExt(WideType, *Source);
      if (!BuiltExtendedInput)
        return std::unexpected(BuiltExtendedInput.error());
      auto ExtendedInput = resultRegister(**BuiltExtendedInput);
      if (!ExtendedInput)
        return std::unexpected(ExtendedInput.error());
      auto ShiftAmount = buildConstantRegister(MIRBuilder, WideType,
                                               static_cast<std::int64_t>((Index - 1U) * PartSize));
      if (!ShiftAmount)
        return std::unexpected(ShiftAmount.error());
      auto BuiltShiftedInput = MIRBuilder.buildShl(WideType, *ExtendedInput, *ShiftAmount);
      if (!BuiltShiftedInput)
        return std::unexpected(BuiltShiftedInput.error());
      auto ShiftedInput = resultRegister(**BuiltShiftedInput);
      if (!ShiftedInput)
        return std::unexpected(ShiftedInput.error());

      Register NextResult = *Destination;
      if (Index + 1U != Instruction.operandCount() || WideType != *DestinationType) {
        auto NewResult = MRI.createVirtualRegister(WideType);
        if (!NewResult)
          return std::unexpected(NewResult.error());
        NextResult = *NewResult;
      }
      if (auto Built = MIRBuilder.buildOr(NextResult, ResultRegister, *ShiftedInput); !Built)
        return std::unexpected(Built.error());
      ResultRegister = NextResult;
    }

    if (WideSize > DestinationSize) {
      if (auto Built = MIRBuilder.buildTrunc(*Destination, ResultRegister); !Built)
        return std::unexpected(Built.error());
    } else if (DestinationType->isPointer()) {
      if (auto Built = MIRBuilder.buildIntToPtr(*Destination, ResultRegister); !Built)
        return std::unexpected(Built.error());
    }

    return Instruction.eraseFromParent();
  }

  auto GCDType = LowLevelType::makeCategoryNeutralScalar(std::gcd(SourceSize, WideSize));
  auto WideDestinationType = LowLevelType::makeCategoryNeutralScalar(MergeCount * WideSize);
  if (!GCDType)
    return std::unexpected(GCDType.error());
  if (!WideDestinationType)
    return std::unexpected(WideDestinationType.error());

  std::vector<Register> UnmergedRegisters;
  for (std::size_t Index = 1; Index != Instruction.operandCount(); ++Index) {
    const Register *Source = Instruction.operandAt(Index)->getIf<Register>();
    if (Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_MERGE_VALUES requires register sources");
    if (GCDType->minimumBitWidth() == SourceSize) {
      UnmergedRegisters.push_back(*Source);
      continue;
    }
    auto Extracted = extractGCDType(UnmergedRegisters, *GCDType, *Source);
    if (!Extracted)
      return std::unexpected(Extracted.error());
  }

  const std::uint64_t RequiredPartCount = MergeCount * WideSize;
  if (UnmergedRegisters.size() != RequiredPartCount) {
    auto BuiltUndef = MIRBuilder.buildUndef(*GCDType);
    if (!BuiltUndef)
      return std::unexpected(BuiltUndef.error());
    auto UndefRegister = resultRegister(**BuiltUndef);
    if (!UndefRegister)
      return std::unexpected(UndefRegister.error());
    while (UnmergedRegisters.size() != RequiredPartCount)
      UnmergedRegisters.push_back(*UndefRegister);
  }

  const std::size_t PartsPerGCD = static_cast<std::size_t>(WideSize / GCDType->minimumBitWidth());
  std::vector<Register> NewMergeRegisters;
  NewMergeRegisters.reserve(static_cast<std::size_t>(MergeCount));
  for (std::size_t Index = 0; Index != MergeCount; ++Index) {
    std::vector<Register> Parts(
        UnmergedRegisters.begin() + static_cast<std::ptrdiff_t>(Index * PartsPerGCD),
        UnmergedRegisters.begin() + static_cast<std::ptrdiff_t>((Index + 1U) * PartsPerGCD));
    auto BuiltMerge = MIRBuilder.buildMergeLikeInstr(WideType, Parts);
    if (!BuiltMerge)
      return std::unexpected(BuiltMerge.error());
    auto MergeRegister = resultRegister(**BuiltMerge);
    if (!MergeRegister)
      return std::unexpected(MergeRegister.error());
    NewMergeRegisters.push_back(*MergeRegister);
  }

  if (DestinationSize == WideDestinationType->minimumBitWidth()) {
    if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, NewMergeRegisters); !Built)
      return std::unexpected(Built.error());
  } else {
    auto BuiltFinalMerge = MIRBuilder.buildMergeLikeInstr(*WideDestinationType, NewMergeRegisters);
    if (!BuiltFinalMerge)
      return std::unexpected(BuiltFinalMerge.error());
    auto FinalMerge = resultRegister(**BuiltFinalMerge);
    if (!FinalMerge)
      return std::unexpected(FinalMerge.error());
    if (auto Built = MIRBuilder.buildTrunc(*Destination, *FinalMerge); !Built)
      return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::widenScalarUnmergeValues(MachineInstr &Instruction,
                                                         std::size_t TypeIndex,
                                                         LowLevelType WideType) {
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "G_UNMERGE_VALUES widening requires type index zero");
  if (Instruction.operandCount() < 2)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_UNMERGE_VALUES requires destinations and a source");

  const std::size_t DestinationCount = Instruction.operandCount() - 1U;
  const Register *Source = Instruction.operandAt(DestinationCount)->getIf<Register>();
  const Register *FirstDestination = Instruction.operandAt(0)->getIf<Register>();
  if (Source == nullptr || FirstDestination == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_UNMERGE_VALUES requires register operands");
  auto SourceType = MRI.getType(*Source);
  auto DestinationType = MRI.getType(*FirstDestination);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (SourceType->isVector() || !DestinationType->isCategoryNeutralScalar())
    return makeUnexpected(ErrorCode::Unsupported,
                          "G_UNMERGE_VALUES widening requires scalar types");

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  Register SourceRegister = *Source;
  if (WideType.minimumBitWidth() >= SourceType->minimumBitWidth()) {
    if (SourceType->isPointer()) {
      auto IntegerSourceType =
          LowLevelType::makeCategoryNeutralScalar(SourceType->minimumBitWidth());
      if (!IntegerSourceType)
        return std::unexpected(IntegerSourceType.error());
      auto IntegerSource = MRI.createVirtualRegister(*IntegerSourceType);
      if (!IntegerSource)
        return std::unexpected(IntegerSource.error());
      if (auto Built = MIRBuilder.buildPtrToInt(*IntegerSource, SourceRegister); !Built)
        return std::unexpected(Built.error());
      *SourceType = *IntegerSourceType;
      SourceRegister = *IntegerSource;
    }

    if (WideType.minimumBitWidth() > SourceType->minimumBitWidth()) {
      auto WideSource = MRI.createVirtualRegister(WideType);
      if (!WideSource)
        return std::unexpected(WideSource.error());
      if (auto Built = MIRBuilder.buildAnyExt(*WideSource, SourceRegister); !Built)
        return std::unexpected(Built.error());
      *SourceType = WideType;
      SourceRegister = *WideSource;
    }

    const std::uint64_t DestinationSize = DestinationType->minimumBitWidth();
    if (auto Built = MIRBuilder.buildTrunc(*FirstDestination, SourceRegister); !Built)
      return std::unexpected(Built.error());
    for (std::size_t Index = 1; Index != DestinationCount; ++Index) {
      const Register *Destination = Instruction.operandAt(Index)->getIf<Register>();
      if (Destination == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_UNMERGE_VALUES requires register destinations");
      auto ShiftedSource = MRI.createVirtualRegister(*SourceType);
      if (!ShiftedSource)
        return std::unexpected(ShiftedSource.error());
      auto ShiftAmount = buildConstantRegister(MIRBuilder, *SourceType,
                                               static_cast<std::int64_t>(DestinationSize * Index));
      if (!ShiftAmount)
        return std::unexpected(ShiftAmount.error());
      if (auto Built = MIRBuilder.buildLShr(*ShiftedSource, SourceRegister, *ShiftAmount); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildTrunc(*Destination, *ShiftedSource); !Built)
        return std::unexpected(Built.error());
    }

    return Instruction.eraseFromParent();
  }

  auto LCMType = getLCMType(*SourceType, WideType);
  if (!LCMType)
    return std::unexpected(LCMType.error());
  Register WideSource = SourceRegister;
  if (LCMType->minimumBitWidth() != SourceType->minimumBitWidth()) {
    if (SourceType->isPointer())
      return makeUnexpected(ErrorCode::Unsupported,
                            "widening a pointer G_UNMERGE_VALUES source is not implemented");
    auto ExtendedSource = MRI.createVirtualRegister(*LCMType);
    if (!ExtendedSource)
      return std::unexpected(ExtendedSource.error());
    if (auto Built = MIRBuilder.buildAnyExt(*ExtendedSource, WideSource); !Built)
      return std::unexpected(Built.error());
    WideSource = *ExtendedSource;
  }

  const std::size_t UnmergeCount =
      static_cast<std::size_t>(LCMType->minimumBitWidth() / WideType.minimumBitWidth());
  std::vector<Register> UnmergeRegisters;
  UnmergeRegisters.reserve(UnmergeCount);
  for (std::size_t Index = 0; Index != UnmergeCount; ++Index) {
    auto Part = MRI.createVirtualRegister(WideType);
    if (!Part)
      return std::unexpected(Part.error());
    UnmergeRegisters.push_back(*Part);
  }
  if (auto Built = MIRBuilder.buildUnmergeValues(UnmergeRegisters, WideSource); !Built)
    return std::unexpected(Built.error());

  auto GCDType = getGCDType(WideType, *DestinationType);
  if (!GCDType)
    return std::unexpected(GCDType.error());
  const std::size_t PartsPerRemerge =
      static_cast<std::size_t>(DestinationType->minimumBitWidth() / GCDType->minimumBitWidth());

  if (PartsPerRemerge == 1) {
    const std::size_t PartsPerUnmerge =
        static_cast<std::size_t>(WideType.minimumBitWidth() / DestinationType->minimumBitWidth());
    for (std::size_t Index = 0; Index != UnmergeRegisters.size(); ++Index) {
      std::vector<Register> Destinations;
      Destinations.reserve(PartsPerUnmerge);
      for (std::size_t PartIndex = 0; PartIndex != PartsPerUnmerge; ++PartIndex) {
        const std::size_t DestinationIndex = Index * PartsPerUnmerge + PartIndex;
        if (DestinationIndex < DestinationCount) {
          const Register *Destination = Instruction.operandAt(DestinationIndex)->getIf<Register>();
          if (Destination == nullptr)
            return makeUnexpected(ErrorCode::VerificationError,
                                  "G_UNMERGE_VALUES requires register destinations");
          Destinations.push_back(*Destination);
        } else {
          auto DeadDestination = MRI.createVirtualRegister(*DestinationType);
          if (!DeadDestination)
            return std::unexpected(DeadDestination.error());
          Destinations.push_back(*DeadDestination);
        }
      }
      if (auto Built = MIRBuilder.buildUnmergeValues(Destinations, UnmergeRegisters[Index]); !Built)
        return std::unexpected(Built.error());
    }
  } else {
    std::vector<Register> Parts;
    for (Register UnmergeRegister : UnmergeRegisters) {
      auto Extracted = extractGCDType(Parts, *GCDType, UnmergeRegister);
      if (!Extracted)
        return std::unexpected(Extracted.error());
    }
    for (std::size_t Index = 0; Index != DestinationCount; ++Index) {
      const Register *Destination = Instruction.operandAt(Index)->getIf<Register>();
      if (Destination == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_UNMERGE_VALUES requires register destinations");
      std::vector<Register> RemergeParts(
          Parts.begin() + static_cast<std::ptrdiff_t>(Index * PartsPerRemerge),
          Parts.begin() + static_cast<std::ptrdiff_t>((Index + 1U) * PartsPerRemerge));
      if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, RemergeParts); !Built)
        return std::unexpected(Built.error());
    }
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::insertParts(Register Destination, LowLevelType ResultType,
                                            LowLevelType PartType,
                                            const std::vector<Register> &PartRegisters,
                                            LowLevelType LeftoverType,
                                            const std::vector<Register> &LeftoverRegisters) {
  if (!LeftoverType.isValid()) {
    if (!LeftoverRegisters.empty())
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "leftover registers require a leftover type");
    return MIRBuilder.buildMergeLikeInstr(Destination, PartRegisters).transform([](MachineInstr *) {
    });
  }

  if (ResultType.isVector()) {
    if (LeftoverRegisters.size() != 1)
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "mixed subvector merge requires one leftover register");
    std::vector<Register> AllRegisters = PartRegisters;
    AllRegisters.insert(AllRegisters.end(), LeftoverRegisters.begin(), LeftoverRegisters.end());
    return MIRBuilder.buildMergeLikeInstr(Destination, AllRegisters).transform([](MachineInstr *) {
    });
  }

  std::vector<Register> GCDRegisters;
  auto FirstGCD = getGCDType(ResultType, LeftoverType);
  if (!FirstGCD)
    return std::unexpected(FirstGCD.error());
  auto GCDType = getGCDType(*FirstGCD, PartType);
  if (!GCDType)
    return std::unexpected(GCDType.error());
  for (Register Part : PartRegisters) {
    auto Extracted = extractGCDType(GCDRegisters, *GCDType, Part);
    if (!Extracted)
      return std::unexpected(Extracted.error());
  }
  for (Register Part : LeftoverRegisters) {
    auto Extracted = extractGCDType(GCDRegisters, *GCDType, Part);
    if (!Extracted)
      return std::unexpected(Extracted.error());
  }
  auto LCMType = buildLCMMergePieces(ResultType, LeftoverType, *GCDType, GCDRegisters);
  if (!LCMType)
    return std::unexpected(LCMType.error());
  return buildWidenedRemergeToDst(Destination, *LCMType, GCDRegisters);
}

Expected<void> LegalizerHelper::extractGCDType(std::vector<Register> &Parts, LowLevelType GCDType,
                                               Register Source) {
  auto SourceType = MRI.getType(Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (*SourceType == GCDType) {
    Parts.push_back(Source);
    return {};
  }
  const std::uint64_t SourceSize = SourceType->minimumBitWidth();
  const std::uint64_t GCDSize = GCDType.minimumBitWidth();
  if (GCDSize == 0 || SourceSize % GCDSize != 0)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "GCD type does not evenly divide the source type");
  return extractParts(Source, GCDType, static_cast<int>(SourceSize / GCDSize), Parts, MIRBuilder,
                      MRI);
}

Expected<LowLevelType> LegalizerHelper::extractGCDType(std::vector<Register> &Parts,
                                                       LowLevelType DestinationType,
                                                       LowLevelType NarrowType, Register Source) {
  auto SourceType = MRI.getType(Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  auto FirstGCD = getGCDType(*SourceType, NarrowType);
  if (!FirstGCD)
    return std::unexpected(FirstGCD.error());
  auto GCDType = getGCDType(*FirstGCD, DestinationType);
  if (!GCDType)
    return std::unexpected(GCDType.error());
  auto Extracted = extractGCDType(Parts, *GCDType, Source);
  if (!Extracted)
    return std::unexpected(Extracted.error());
  return *GCDType;
}

Expected<LowLevelType> LegalizerHelper::buildLCMMergePieces(LowLevelType DestinationType,
                                                            LowLevelType NarrowType,
                                                            LowLevelType GCDType,
                                                            std::vector<Register> &Registers,
                                                            GenericOpcode PadStrategy) {
  auto LCMType = getLCMType(DestinationType, NarrowType);
  if (!LCMType)
    return std::unexpected(LCMType.error());
  const int PartCount = static_cast<int>(LCMType->minimumBitWidth() / NarrowType.minimumBitWidth());
  const int SubPartCount =
      static_cast<int>(NarrowType.minimumBitWidth() / GCDType.minimumBitWidth());
  const int OriginalSourceCount = static_cast<int>(Registers.size());

  Register PadRegister;
  if (OriginalSourceCount < PartCount * SubPartCount) {
    if (PadStrategy == GenericOpcode::G_ZEXT) {
      auto Pad = buildConstantRegister(MIRBuilder, GCDType, 0);
      if (!Pad)
        return std::unexpected(Pad.error());
      PadRegister = *Pad;
    } else if (PadStrategy == GenericOpcode::G_ANYEXT) {
      auto Built = MIRBuilder.buildUndef(GCDType);
      if (!Built)
        return std::unexpected(Built.error());
      auto Pad = resultRegister(**Built);
      if (!Pad)
        return std::unexpected(Pad.error());
      PadRegister = *Pad;
    } else if (PadStrategy == GenericOpcode::G_SEXT) {
      if (Registers.empty())
        return makeUnexpected(ErrorCode::InternalError,
                              "sign-extension padding requires a source part");
      auto ShiftType = LowLevelType::makeCategoryNeutralScalar(64);
      if (!ShiftType)
        return std::unexpected(ShiftType.error());
      auto ShiftAmount = buildConstantRegister(
          MIRBuilder, *ShiftType, static_cast<std::int64_t>(GCDType.minimumBitWidth() - 1U));
      if (!ShiftAmount)
        return std::unexpected(ShiftAmount.error());
      auto Pad = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, GCDType,
                                        {Registers.back(), *ShiftAmount});
      if (!Pad)
        return std::unexpected(Pad.error());
      PadRegister = *Pad;
    } else {
      return makeUnexpected(ErrorCode::InternalError,
                            "LCM merge received an invalid extension strategy");
    }
  }

  std::vector<Register> Remerge(static_cast<std::size_t>(PartCount));
  std::vector<Register> SubMerge(static_cast<std::size_t>(SubPartCount));
  Register AllPadRegister;
  for (int PartIndex = 0; PartIndex != PartCount; ++PartIndex) {
    bool AllMergePartsArePadding = true;
    for (int SubPartIndex = 0; SubPartIndex != SubPartCount; ++SubPartIndex) {
      const int Index = PartIndex * SubPartCount + SubPartIndex;
      if (Index >= OriginalSourceCount) {
        SubMerge[static_cast<std::size_t>(SubPartIndex)] = PadRegister;
        continue;
      }
      SubMerge[static_cast<std::size_t>(SubPartIndex)] = Registers[static_cast<std::size_t>(Index)];
      AllMergePartsArePadding = false;
    }

    if (AllMergePartsArePadding && AllPadRegister.isNoRegister()) {
      if (PadStrategy == GenericOpcode::G_ZEXT) {
        auto Pad = buildConstantRegister(MIRBuilder, NarrowType, 0);
        if (!Pad)
          return std::unexpected(Pad.error());
        AllPadRegister = *Pad;
      } else if (PadStrategy == GenericOpcode::G_ANYEXT) {
        auto Built = MIRBuilder.buildUndef(NarrowType);
        if (!Built)
          return std::unexpected(Built.error());
        auto Pad = resultRegister(**Built);
        if (!Pad)
          return std::unexpected(Pad.error());
        AllPadRegister = *Pad;
      }
    }
    if (!AllPadRegister.isNoRegister()) {
      Remerge[static_cast<std::size_t>(PartIndex)] = AllPadRegister;
      continue;
    }
    if (SubPartCount == 1) {
      Remerge[static_cast<std::size_t>(PartIndex)] = SubMerge.front();
    } else {
      auto Built = MIRBuilder.buildMergeLikeInstr(NarrowType, SubMerge);
      if (!Built)
        return std::unexpected(Built.error());
      auto RemergeRegister = resultRegister(**Built);
      if (!RemergeRegister)
        return std::unexpected(RemergeRegister.error());
      Remerge[static_cast<std::size_t>(PartIndex)] = *RemergeRegister;
    }
    if (AllMergePartsArePadding && AllPadRegister.isNoRegister())
      AllPadRegister = Remerge[static_cast<std::size_t>(PartIndex)];
  }
  Registers = std::move(Remerge);
  return *LCMType;
}

Expected<void>
LegalizerHelper::buildWidenedRemergeToDst(Register Destination, LowLevelType LCMType,
                                          const std::vector<Register> &RemergeRegisters) {
  auto DestinationType = MRI.getType(Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (*DestinationType == LCMType)
    return MIRBuilder.buildMergeLikeInstr(Destination, RemergeRegisters)
        .transform([](MachineInstr *) {});

  auto Remerge = MRI.createVirtualRegister(LCMType);
  if (!Remerge)
    return std::unexpected(Remerge.error());
  auto BuiltMerge = MIRBuilder.buildMergeLikeInstr(*Remerge, RemergeRegisters);
  if (!BuiltMerge)
    return std::unexpected(BuiltMerge.error());
  if (!DestinationType->isVector() && !LCMType.isVector())
    return MIRBuilder.buildTrunc(Destination, *Remerge).transform([](MachineInstr *) {});

  if (LCMType.isVector()) {
    const std::size_t DefinitionCount =
        static_cast<std::size_t>(LCMType.minimumBitWidth() / DestinationType->minimumBitWidth());
    std::vector<Register> Definitions;
    Definitions.reserve(DefinitionCount);
    Definitions.push_back(Destination);
    for (std::size_t Index = 1; Index != DefinitionCount; ++Index) {
      auto Definition = MRI.createVirtualRegister(*DestinationType);
      if (!Definition)
        return std::unexpected(Definition.error());
      Definitions.push_back(*Definition);
    }
    return MIRBuilder.buildUnmergeValues(Definitions, *Remerge).transform([](MachineInstr *) {});
  }
  return makeUnexpected(ErrorCode::Unsupported,
                        "widened remerge has an unsupported type combination");
}

Expected<void> LegalizerHelper::multiplyRegisters(std::vector<Register> &Destinations,
                                                  const std::vector<Register> &FirstSources,
                                                  const std::vector<Register> &SecondSources,
                                                  LowLevelType NarrowType) {
  if (FirstSources.empty() || FirstSources.size() != SecondSources.size() ||
      (Destinations.size() != FirstSources.size() &&
       Destinations.size() != FirstSources.size() * 2U)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "multi-precision multiplication has incompatible register parts");
  }
  auto BooleanType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!BooleanType)
    return std::unexpected(BooleanType.error());

  const auto BuildUAddo = [&](Register Left,
                              Register Right) -> Expected<std::pair<Register, Register>> {
    auto Sum = MRI.createVirtualRegister(NarrowType);
    auto Carry = MRI.createVirtualRegister(*BooleanType);
    if (!Sum)
      return std::unexpected(Sum.error());
    if (!Carry)
      return std::unexpected(Carry.error());
    auto Built =
        MIRBuilder.buildInstr(opcode(GenericOpcode::G_UADDO), {*Sum, *Carry}, {Left, Right});
    if (!Built)
      return std::unexpected(Built.error());
    return std::pair{*Sum, *Carry};
  };

  const std::size_t SourcePartCount = FirstSources.size();
  const std::size_t DestinationPartCount = Destinations.size();
  auto Product = buildResultInstruction(MIRBuilder, GenericOpcode::G_MUL, NarrowType,
                                        {FirstSources.front(), SecondSources.front()});
  if (!Product)
    return std::unexpected(Product.error());
  Destinations.front() = *Product;

  Register PreviousCarrySum;
  std::vector<Register> Factors;
  for (std::size_t DestinationIndex = 1; DestinationIndex != DestinationPartCount;
       ++DestinationIndex) {
    // Collect high halves from the preceding product diagonal.
    const std::size_t HighStart =
        DestinationIndex < SourcePartCount ? 0 : DestinationIndex - SourcePartCount;
    const std::size_t HighEnd = std::min(DestinationIndex - 1U, SourcePartCount - 1U);
    for (std::size_t Index = HighStart; Index <= HighEnd; ++Index) {
      auto HighProduct = buildResultInstruction(
          MIRBuilder, GenericOpcode::G_UMULH, NarrowType,
          {FirstSources[DestinationIndex - 1U - Index], SecondSources[Index]});
      if (!HighProduct)
        return std::unexpected(HighProduct.error());
      Factors.push_back(*HighProduct);
    }

    // Visit the current low-half diagonal from its low first-source part.
    const std::size_t LowStart =
        DestinationIndex + 1U < SourcePartCount ? 0 : DestinationIndex - SourcePartCount + 1U;
    const std::size_t LowEnd = std::min(DestinationIndex, SourcePartCount - 1U);
    for (std::size_t ReverseIndex = LowEnd + 1U; ReverseIndex != LowStart; --ReverseIndex) {
      const std::size_t Index = ReverseIndex - 1U;
      auto LowProduct =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_MUL, NarrowType,
                                 {FirstSources[DestinationIndex - Index], SecondSources[Index]});
      if (!LowProduct)
        return std::unexpected(LowProduct.error());
      Factors.push_back(*LowProduct);
    }
    if (DestinationIndex != 1U)
      Factors.push_back(PreviousCarrySum);
    if (Factors.size() < 2)
      return makeUnexpected(ErrorCode::InternalError,
                            "multi-precision multiplication has too few factors");

    Register FactorSum;
    Register CarrySum;
    if (DestinationIndex + 1U != DestinationPartCount) {
      auto SumAndCarry = BuildUAddo(Factors[0], Factors[1]);
      if (!SumAndCarry)
        return std::unexpected(SumAndCarry.error());
      FactorSum = SumAndCarry->first;
      auto ExtendedCarry = MRI.createVirtualRegister(NarrowType);
      if (!ExtendedCarry)
        return std::unexpected(ExtendedCarry.error());
      if (auto Built = MIRBuilder.buildZExt(*ExtendedCarry, SumAndCarry->second); !Built)
        return std::unexpected(Built.error());
      CarrySum = *ExtendedCarry;

      for (std::size_t Index = 2; Index != Factors.size(); ++Index) {
        SumAndCarry = BuildUAddo(FactorSum, Factors[Index]);
        if (!SumAndCarry)
          return std::unexpected(SumAndCarry.error());
        FactorSum = SumAndCarry->first;
        auto ExtendedCarryPart = MRI.createVirtualRegister(NarrowType);
        if (!ExtendedCarryPart)
          return std::unexpected(ExtendedCarryPart.error());
        if (auto Built = MIRBuilder.buildZExt(*ExtendedCarryPart, SumAndCarry->second); !Built)
          return std::unexpected(Built.error());
        auto NextCarrySum = buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, NarrowType,
                                                   {CarrySum, *ExtendedCarryPart});
        if (!NextCarrySum)
          return std::unexpected(NextCarrySum.error());
        CarrySum = *NextCarrySum;
      }
    } else {
      auto Sum = buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, NarrowType,
                                        {Factors[0], Factors[1]});
      if (!Sum)
        return std::unexpected(Sum.error());
      FactorSum = *Sum;
      for (std::size_t Index = 2; Index != Factors.size(); ++Index) {
        Sum = buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, NarrowType,
                                     {FactorSum, Factors[Index]});
        if (!Sum)
          return std::unexpected(Sum.error());
        FactorSum = *Sum;
      }
    }

    PreviousCarrySum = CarrySum;
    Destinations[DestinationIndex] = FactorSum;
    Factors.clear();
  }
  return {};
}

Expected<void> LegalizerHelper::widenScalarSrc(MachineInstr &Instruction, LowLevelType WideType,
                                               std::size_t OperandIndex,
                                               GenericOpcode ExtensionOpcode) {
  MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Source = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Source == nullptr || !Source->isVirtual() || !Operand->isUse())
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar widening source is not a virtual-register use");

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Extension = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {WideType}, {*Source});
  if (!Extension)
    return std::unexpected(Extension.error());
  auto WideSource = resultRegister(**Extension);
  if (!WideSource)
    return std::unexpected(WideSource.error());
  return Operand->setRegister(*WideSource);
}

Expected<void> LegalizerHelper::narrowScalarSrc(MachineInstr &Instruction, LowLevelType NarrowType,
                                                std::size_t OperandIndex) {
  MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Source = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Source == nullptr || !Source->isVirtual() || !Operand->isUse())
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar narrowing source is not a virtual-register use");

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Truncation = MIRBuilder.buildTrunc(NarrowType, *Source);
  if (!Truncation)
    return std::unexpected(Truncation.error());
  auto NarrowSource = resultRegister(**Truncation);
  if (!NarrowSource)
    return std::unexpected(NarrowSource.error());
  return Operand->setRegister(*NarrowSource);
}

Expected<void> LegalizerHelper::narrowScalarDst(MachineInstr &Instruction, LowLevelType NarrowType,
                                                std::size_t OperandIndex,
                                                GenericOpcode ExtensionOpcode) {
  MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Destination = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Destination == nullptr || !Destination->isVirtual() || !Operand->isDef())
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar narrowing destination is not a virtual-register definition");

  const Register OriginalDestination = *Destination;
  auto NarrowDestination = MRI.createVirtualRegister(NarrowType);
  if (!NarrowDestination)
    return std::unexpected(NarrowDestination.error());
  if (auto Point = MIRBuilder.setInsertPointAfter(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Extension = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {OriginalDestination},
                                             {*NarrowDestination});
      !Extension) {
    return std::unexpected(Extension.error());
  }
  return Operand->setRegister(*NarrowDestination);
}

Expected<void> LegalizerHelper::widenScalarDst(MachineInstr &Instruction, LowLevelType WideType,
                                               std::size_t OperandIndex,
                                               GenericOpcode TruncOpcode) {
  MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  const Register *Destination = Operand == nullptr ? nullptr : Operand->getIf<Register>();
  if (Destination == nullptr || !Destination->isVirtual() || !Operand->isDef())
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar widening destination is not a virtual-register definition");

  auto WideDestination = MRI.createVirtualRegister(WideType);
  if (!WideDestination)
    return std::unexpected(WideDestination.error());
  if (auto Point = MIRBuilder.setInsertPointAfter(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Truncation = MIRBuilder.buildInstr(opcode(TruncOpcode));
  if (!Truncation)
    return std::unexpected(Truncation.error());
  if (auto Added = Truncation->addDef(*Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Truncation->addUse(*WideDestination); !Added)
    return std::unexpected(Added.error());
  if (auto Built = Truncation->build(); !Built)
    return std::unexpected(Built.error());
  return Operand->setRegister(*WideDestination);
}

Expected<void> LegalizerHelper::widenScalarAddSubOverflow(MachineInstr &Instruction,
                                                          std::size_t TypeIndex,
                                                          LowLevelType WideType) {
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  GenericOpcode ArithmeticOpcode;
  GenericOpcode ExtensionOpcode;
  bool HasCarryIn = false;
  switch (Generic) {
  case GenericOpcode::G_SADDO:
    ArithmeticOpcode = GenericOpcode::G_ADD;
    ExtensionOpcode = GenericOpcode::G_SEXT;
    break;
  case GenericOpcode::G_SSUBO:
    ArithmeticOpcode = GenericOpcode::G_SUB;
    ExtensionOpcode = GenericOpcode::G_SEXT;
    break;
  case GenericOpcode::G_UADDO:
    ArithmeticOpcode = GenericOpcode::G_ADD;
    ExtensionOpcode = GenericOpcode::G_ZEXT;
    break;
  case GenericOpcode::G_USUBO:
    ArithmeticOpcode = GenericOpcode::G_SUB;
    ExtensionOpcode = GenericOpcode::G_ZEXT;
    break;
  case GenericOpcode::G_SADDE:
    ArithmeticOpcode = GenericOpcode::G_UADDE;
    ExtensionOpcode = GenericOpcode::G_SEXT;
    HasCarryIn = true;
    break;
  case GenericOpcode::G_SSUBE:
    ArithmeticOpcode = GenericOpcode::G_USUBE;
    ExtensionOpcode = GenericOpcode::G_SEXT;
    HasCarryIn = true;
    break;
  case GenericOpcode::G_UADDE:
    ArithmeticOpcode = GenericOpcode::G_UADDE;
    ExtensionOpcode = GenericOpcode::G_ZEXT;
    HasCarryIn = true;
    break;
  case GenericOpcode::G_USUBE:
    ArithmeticOpcode = GenericOpcode::G_USUBE;
    ExtensionOpcode = GenericOpcode::G_ZEXT;
    HasCarryIn = true;
    break;
  default:
    return makeUnexpected(ErrorCode::InternalError,
                          "unexpected opcode during add/sub overflow widening");
  }

  if (TypeIndex == 1) {
    if (HasCarryIn) {
      if (auto Result = widenScalarSrc(Instruction, WideType, 4,
                                       MIRBuilder.getBoolExtOp(WideType.isVector(), false));
          !Result) {
        return Result;
      }
    }
    return widenScalarDst(Instruction, WideType, 1);
  }
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "add/sub overflow widening requires type index zero or one");
  if (Instruction.operandCount() != (HasCarryIn ? 5U : 4U))
    return makeUnexpected(ErrorCode::VerificationError,
                          "add/sub overflow widening has an invalid operand count");

  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *OverflowOperand = Instruction.operandAt(1);
  const MachineOperand *LeftOperand = Instruction.operandAt(2);
  const MachineOperand *RightOperand = Instruction.operandAt(3);
  const MachineOperand *CarryInOperand = HasCarryIn ? Instruction.operandAt(4) : nullptr;
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Overflow =
      OverflowOperand == nullptr ? nullptr : OverflowOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  const Register *CarryIn = CarryInOperand == nullptr ? nullptr : CarryInOperand->getIf<Register>();
  if (Destination == nullptr || Overflow == nullptr || Left == nullptr || Right == nullptr ||
      (HasCarryIn && CarryIn == nullptr)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "add/sub overflow widening requires register operands");
  }

  auto OriginalType = MRI.getType(*Destination);
  auto OverflowType = MRI.getType(*Overflow);
  if (!OriginalType)
    return std::unexpected(OriginalType.error());
  if (!OverflowType)
    return std::unexpected(OverflowType.error());

  auto WideLeft = MRI.createVirtualRegister(WideType);
  auto WideRight = MRI.createVirtualRegister(WideType);
  auto WideResult = MRI.createVirtualRegister(WideType);
  auto TruncatedResult = MRI.createVirtualRegister(*OriginalType);
  auto ExtendedResult = MRI.createVirtualRegister(WideType);
  if (!WideLeft)
    return std::unexpected(WideLeft.error());
  if (!WideRight)
    return std::unexpected(WideRight.error());
  if (!WideResult)
    return std::unexpected(WideResult.error());
  if (!TruncatedResult)
    return std::unexpected(TruncatedResult.error());
  if (!ExtendedResult)
    return std::unexpected(ExtendedResult.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*WideLeft}, {*Left}); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*WideRight}, {*Right}); !Built)
    return std::unexpected(Built.error());
  if (HasCarryIn) {
    if (auto Built = MIRBuilder.buildInstr(opcode(ArithmeticOpcode), {*WideResult, *OverflowType},
                                           {*WideLeft, *WideRight, *CarryIn});
        !Built) {
      return std::unexpected(Built.error());
    }
  } else if (auto Built = MIRBuilder.buildInstr(opcode(ArithmeticOpcode), {*WideResult},
                                                {*WideLeft, *WideRight});
             !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildTrunc(*TruncatedResult, *WideResult); !Built)
    return std::unexpected(Built.error());
  if (auto Built =
          MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*ExtendedResult}, {*TruncatedResult});
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built =
          MIRBuilder.buildICmp(IntegerPredicate::NotEqual, *Overflow, *WideResult, *ExtendedResult);
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildTrunc(*Destination, *WideResult); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::widenScalarMulo(MachineInstr &Instruction, std::size_t TypeIndex,
                                                LowLevelType WideType) {
  if (TypeIndex == 1)
    return widenScalarDst(Instruction, WideType, 1);
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply-overflow widening requires type index zero or one");
  if (Instruction.operandCount() != 4)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply-overflow widening requires four register operands");

  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_SMULO);
  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *OriginalOverflow = Instruction.operandAt(1)->getIf<Register>();
  const Register *Left = Instruction.operandAt(2)->getIf<Register>();
  const Register *Right = Instruction.operandAt(3)->getIf<Register>();
  if (Destination == nullptr || OriginalOverflow == nullptr || Left == nullptr || Right == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply-overflow widening requires register operands");

  auto SourceType = MRI.getType(*Left);
  auto OverflowType = MRI.getType(*OriginalOverflow);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!OverflowType)
    return std::unexpected(OverflowType.error());
  const std::uint32_t SourceBitWidth = SourceType->scalarBitWidth();
  if (SourceBitWidth == 0 || !WideType.isCategoryNeutralScalar())
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply-overflow widening requires scalar types");

  const GenericOpcode ExtensionOpcode = IsSigned ? GenericOpcode::G_SEXT : GenericOpcode::G_ZEXT;
  auto WideLeft = buildResultInstruction(MIRBuilder, ExtensionOpcode, WideType, {*Left});
  auto WideRight = buildResultInstruction(MIRBuilder, ExtensionOpcode, WideType, {*Right});
  if (!WideLeft)
    return std::unexpected(WideLeft.error());
  if (!WideRight)
    return std::unexpected(WideRight.error());

  const bool WideMulCanOverflow = static_cast<std::uint64_t>(WideType.scalarBitWidth()) <
                                  2U * static_cast<std::uint64_t>(SourceBitWidth);
  Register Mul;
  Register WideOverflow;
  if (WideMulCanOverflow) {
    auto Built =
        MIRBuilder.buildInstr(opcode(IsSigned ? GenericOpcode::G_SMULO : GenericOpcode::G_UMULO),
                              {WideType, *OverflowType}, {*WideLeft, *WideRight});
    if (!Built)
      return std::unexpected(Built.error());
    const MachineOperand *ResultOperand = (*Built)->operandAt(0);
    const MachineOperand *OverflowOperand = (*Built)->operandAt(1);
    const Register *Result = ResultOperand == nullptr ? nullptr : ResultOperand->getIf<Register>();
    const Register *Overflow =
        OverflowOperand == nullptr ? nullptr : OverflowOperand->getIf<Register>();
    if (Result == nullptr || Overflow == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "multiply-overflow widening produced invalid results");
    Mul = *Result;
    WideOverflow = *Overflow;
  } else {
    auto Built =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_MUL, WideType, {*WideLeft, *WideRight});
    if (!Built)
      return std::unexpected(Built.error());
    Mul = *Built;
  }

  if (auto Built = MIRBuilder.buildTrunc(*Destination, Mul); !Built)
    return std::unexpected(Built.error());

  Register ExtendedResult;
  if (IsSigned) {
    auto Built = MIRBuilder.buildSExtInReg(WideType, Mul, SourceBitWidth);
    if (!Built)
      return std::unexpected(Built.error());
    auto Result = resultRegister(**Built);
    if (!Result)
      return std::unexpected(Result.error());
    ExtendedResult = *Result;
  } else {
    auto Mask = APInt::getLowBitsSet(WideType.scalarBitWidth(), SourceBitWidth);
    if (!Mask)
      return std::unexpected(Mask.error());
    auto MaskRegister = buildConstantRegister(MIRBuilder, WideType, std::move(*Mask));
    if (!MaskRegister)
      return std::unexpected(MaskRegister.error());
    auto Built =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, WideType, {Mul, *MaskRegister});
    if (!Built)
      return std::unexpected(Built.error());
    ExtendedResult = *Built;
  }

  if (WideMulCanOverflow) {
    auto Overflow = MRI.createVirtualRegister(*OverflowType);
    if (!Overflow)
      return std::unexpected(Overflow.error());
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::NotEqual, *Overflow, Mul, ExtendedResult);
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildOr(*OriginalOverflow, WideOverflow, *Overflow); !Built)
      return std::unexpected(Built.error());
  } else if (auto Built = MIRBuilder.buildICmp(IntegerPredicate::NotEqual, *OriginalOverflow, Mul,
                                               ExtendedResult);
             !Built) {
    return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::widenScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                            LowLevelType WideType) {
  auto Query = LegalizerInfo::buildQuery(Instruction, Function, MIRBuilder.target());
  if (!Query)
    return std::unexpected(Query.error());
  if (!WideType.isCategoryNeutralScalar() || TypeIndex >= Query->Types.size() ||
      !Query->Types[TypeIndex].isCategoryNeutralScalar() ||
      WideType.scalarBitWidth() <= Query->Types[TypeIndex].scalarBitWidth()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "scalar widening has an invalid type index or target type");
  }

  const auto WidenSource = [&](std::size_t OperandIndex, GenericOpcode ExtensionOpcode) {
    return widenScalarSrc(Instruction, WideType, OperandIndex, ExtensionOpcode);
  };
  const auto WidenDestination = [&](std::size_t OperandIndex = 0,
                                    GenericOpcode TruncOpcode = GenericOpcode::G_TRUNC) {
    return widenScalarDst(Instruction, WideType, OperandIndex, TruncOpcode);
  };
  const auto Apply = [](Expected<void> Result) -> Expected<void> {
    if (!Result)
      return std::unexpected(Result.error());
    return {};
  };

  Observer.changingInstr(Instruction);
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_MERGE_VALUES:
    return widenScalarMergeValues(Instruction, TypeIndex, WideType);
  case GenericOpcode::G_UNMERGE_VALUES:
    return widenScalarUnmergeValues(Instruction, TypeIndex, WideType);
  case GenericOpcode::G_ATOMICRMW_XCHG:
  case GenericOpcode::G_ATOMICRMW_ADD:
  case GenericOpcode::G_ATOMICRMW_SUB:
  case GenericOpcode::G_ATOMICRMW_AND:
  case GenericOpcode::G_ATOMICRMW_OR:
  case GenericOpcode::G_ATOMICRMW_XOR:
  case GenericOpcode::G_ATOMICRMW_MIN:
  case GenericOpcode::G_ATOMICRMW_MAX:
  case GenericOpcode::G_ATOMICRMW_UMIN:
  case GenericOpcode::G_ATOMICRMW_UMAX:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "atomic RMW value widening has an invalid type index");
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ATOMIC_CMPXCHG:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "compare-exchange widening has an invalid type index");
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenSource(3, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS:
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenSource(3, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = Apply(WidenSource(4, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "compare-exchange success widening has an invalid type index");
    if (auto Result = Apply(WidenDestination(1)); !Result)
      return Result;
    break;

  case GenericOpcode::G_CONSTANT: {
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "constant widening has an invalid type index");
    MachineOperand *SourceOperand = Instruction.operandAt(1);
    const auto *Immediate =
        SourceOperand == nullptr ? nullptr : SourceOperand->getIf<IntegerImmediateOperand>();
    if (Immediate == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_CONSTANT widening requires an integer immediate");
    const GenericOpcode ExtensionOpcode =
        Info.getExtOpcodeForWideningConstant(Query->Types[TypeIndex]);
    if (ExtensionOpcode != GenericOpcode::G_SEXT && ExtensionOpcode != GenericOpcode::G_ZEXT &&
        ExtensionOpcode != GenericOpcode::G_ANYEXT) {
      return makeUnexpected(ErrorCode::InternalError,
                            "constant widening selected an invalid extension opcode");
    }
    auto Extended = ExtensionOpcode == GenericOpcode::G_SEXT
                        ? Immediate->Value.sext(WideType.scalarBitWidth())
                        : Immediate->Value.zext(WideType.scalarBitWidth());
    if (!Extended)
      return std::unexpected(Extended.error());
    if (auto Replaced = SourceOperand->setIntegerImmediate(std::move(*Extended)); !Replaced)
      return std::unexpected(Replaced.error());
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;
  }

  case GenericOpcode::G_IMPLICIT_DEF:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "implicit definition widening has an invalid type index");
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_BRCOND:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_BRCOND widening requires type index zero");
    if (auto Result = Apply(WidenSource(0, MIRBuilder.getBoolExtOp(false, false))); !Result)
      return Result;
    break;

  case GenericOpcode::G_CTTZ:
  case GenericOpcode::G_CTTZ_ZERO_POISON:
  case GenericOpcode::G_CTLZ:
  case GenericOpcode::G_CTLZ_ZERO_POISON:
  case GenericOpcode::G_CTLS:
  case GenericOpcode::G_CTPOP: {
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "bit-count widening has an invalid type index");

    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *SourceOperand = Instruction.operandAt(1);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Destination == nullptr || Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "bit-count widening requires register operands");
    auto SourceType = MRI.getType(*Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());

    GenericOpcode ExtensionOpcode = GenericOpcode::G_ZEXT;
    switch (Generic) {
    case GenericOpcode::G_CTTZ:
    case GenericOpcode::G_CTTZ_ZERO_POISON:
    case GenericOpcode::G_CTLZ_ZERO_POISON:
      ExtensionOpcode = GenericOpcode::G_ANYEXT;
      break;
    case GenericOpcode::G_CTLS:
      ExtensionOpcode = GenericOpcode::G_SEXT;
      break;
    default:
      break;
    }

    auto WideSource = buildResultInstruction(MIRBuilder, ExtensionOpcode, WideType, {*Source});
    if (!WideSource)
      return std::unexpected(WideSource.error());
    GenericOpcode WideOpcode = Generic;
    if (Generic == GenericOpcode::G_CTTZ) {
      auto TopBitValue = oneBitSet(WideType.scalarBitWidth(), SourceType->scalarBitWidth());
      if (!TopBitValue)
        return std::unexpected(TopBitValue.error());
      auto TopBit = buildConstantRegister(MIRBuilder, WideType, std::move(*TopBitValue));
      if (!TopBit)
        return std::unexpected(TopBit.error());
      auto NonZeroSource =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, WideType, {*WideSource, *TopBit});
      if (!NonZeroSource)
        return std::unexpected(NonZeroSource.error());
      WideSource = *NonZeroSource;
      WideOpcode = GenericOpcode::G_CTTZ_ZERO_POISON;
    }

    const std::uint32_t SizeDifference = WideType.scalarBitWidth() - SourceType->scalarBitWidth();
    if (Generic == GenericOpcode::G_CTLZ_ZERO_POISON) {
      auto ShiftAmount =
          buildConstantRegister(MIRBuilder, WideType, static_cast<std::int64_t>(SizeDifference));
      if (!ShiftAmount)
        return std::unexpected(ShiftAmount.error());
      auto Shifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, WideType,
                                            {*WideSource, *ShiftAmount});
      if (!Shifted)
        return std::unexpected(Shifted.error());
      WideSource = *Shifted;
    }

    auto Result = buildResultInstruction(MIRBuilder, WideOpcode, WideType, {*WideSource});
    if (!Result)
      return std::unexpected(Result.error());
    if (Generic == GenericOpcode::G_CTLZ || Generic == GenericOpcode::G_CTLS) {
      auto Difference =
          buildConstantRegister(MIRBuilder, WideType, static_cast<std::int64_t>(SizeDifference));
      if (!Difference)
        return std::unexpected(Difference.error());
      if (Generic == GenericOpcode::G_CTLZ) {
        Result = buildFlaggedBinaryInstruction(MIRBuilder, GenericOpcode::G_SUB, WideType, *Result,
                                               *Difference,
                                               MachineInstrFlags{MachineInstrFlag::NoUnsignedWrap});
      } else {
        Result = buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, WideType,
                                        {*Result, *Difference});
      }
      if (!Result)
        return std::unexpected(Result.error());
    }

    if (auto Built = MIRBuilder.buildZExtOrTrunc(*Destination, *Result); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }

  case GenericOpcode::G_BSWAP: {
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "byte-swap widening has an invalid type index");
    MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "byte-swap widening requires a register destination");
    const Register OriginalDestination = *Destination;
    auto OriginalType = MRI.getType(OriginalDestination);
    if (!OriginalType)
      return std::unexpected(OriginalType.error());
    auto WideDestination = MRI.createVirtualRegister(WideType);
    auto Shifted = MRI.createVirtualRegister(WideType);
    if (!WideDestination)
      return std::unexpected(WideDestination.error());
    if (!Shifted)
      return std::unexpected(Shifted.error());
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Replaced = DestinationOperand->setRegister(*WideDestination); !Replaced)
      return std::unexpected(Replaced.error());

    if (auto Point = MIRBuilder.setInsertPointAfter(Instruction); !Point)
      return std::unexpected(Point.error());
    const std::uint32_t Difference = WideType.scalarBitWidth() - OriginalType->scalarBitWidth();
    auto ShiftAmount = buildConstantRegister(MIRBuilder, WideType, Difference);
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());
    if (auto Built = MIRBuilder.buildLShr(*Shifted, *WideDestination, *ShiftAmount); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildTrunc(OriginalDestination, *Shifted); !Built)
      return std::unexpected(Built.error());
    break;
  }

  case GenericOpcode::G_LOAD:
  case GenericOpcode::G_SEXTLOAD:
  case GenericOpcode::G_ZEXTLOAD:
  case GenericOpcode::G_FPEXTLOAD:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "load widening has an invalid type index");
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_STORE: {
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "store widening has an invalid type index");
    const Register *Source = Instruction.operandAt(0)->getIf<Register>();
    if (Source == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "store widening requires a register source");
    auto SourceType = MRI.getType(*Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    if (!SourceType->isCategoryNeutralScalar())
      return makeUnexpected(ErrorCode::Unsupported, "non-scalar store widening is not implemented");
    const GenericOpcode ExtensionOpcode =
        SourceType->scalarBitWidth() == 1 ? GenericOpcode::G_ZEXT : GenericOpcode::G_ANYEXT;
    if (auto Result = Apply(WidenSource(0, ExtensionOpcode)); !Result)
      return Result;
    break;
  }

  case GenericOpcode::G_FPTRUNCSTORE:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating truncating store widening has an invalid type index");
    if (auto Result = Apply(WidenSource(0, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_SADDO:
  case GenericOpcode::G_SSUBO:
  case GenericOpcode::G_UADDO:
  case GenericOpcode::G_USUBO:
  case GenericOpcode::G_SADDE:
  case GenericOpcode::G_SSUBE:
  case GenericOpcode::G_UADDE:
  case GenericOpcode::G_USUBE:
    return widenScalarAddSubOverflow(Instruction, TypeIndex, WideType);

  case GenericOpcode::G_SMULO:
  case GenericOpcode::G_UMULO:
    if (TypeIndex != 1)
      return widenScalarMulo(Instruction, TypeIndex, WideType);
    if (auto Result = widenScalarMulo(Instruction, TypeIndex, WideType); !Result)
      return Result;
    break;

  case GenericOpcode::G_ADD:
  case GenericOpcode::G_SUB:
  case GenericOpcode::G_MUL:
  case GenericOpcode::G_AND:
  case GenericOpcode::G_OR:
  case GenericOpcode::G_XOR:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "binary operation widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_SDIV:
  case GenericOpcode::G_SREM:
  case GenericOpcode::G_SMIN:
  case GenericOpcode::G_SMAX:
  case GenericOpcode::G_ABDS:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "signed operation widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_SEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_SEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_UDIV:
  case GenericOpcode::G_UREM:
  case GenericOpcode::G_UMIN:
  case GenericOpcode::G_UMAX:
  case GenericOpcode::G_ABDU:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "unsigned operation widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_SHL:
  case GenericOpcode::G_LSHR:
  case GenericOpcode::G_ASHR:
    if (TypeIndex == 0) {
      GenericOpcode ExtensionOpcode = GenericOpcode::G_ANYEXT;
      if (Instruction.opcode() == opcode(GenericOpcode::G_LSHR))
        ExtensionOpcode = GenericOpcode::G_ZEXT;
      else if (Instruction.opcode() == opcode(GenericOpcode::G_ASHR))
        ExtensionOpcode = GenericOpcode::G_SEXT;
      if (auto Result = Apply(WidenSource(1, ExtensionOpcode)); !Result)
        return Result;
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "shift widening has an invalid type index");
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_ROTL:
  case GenericOpcode::G_ROTR:
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported, "rotate value widening is not defined");
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_FREEZE:
  case GenericOpcode::G_CONSTANT_FOLD_BARRIER:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "unary operation widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_ANYEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ABS:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "absolute-value widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_SEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  case GenericOpcode::G_ICMP:
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer comparison widening has an invalid type index");
    {
      const MachineOperand *PredicateMachineOperand = Instruction.operandAt(1);
      const auto *Predicate = PredicateMachineOperand == nullptr
                                  ? nullptr
                                  : PredicateMachineOperand->getIf<PredicateOperand>();
      const IntegerPredicate *Integer =
          Predicate == nullptr ? nullptr : Predicate->integerPredicate();
      if (Integer == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_ICMP widening requires an integer predicate");

      bool IsSigned = false;
      switch (*Integer) {
      case IntegerPredicate::SignedGreaterThan:
      case IntegerPredicate::SignedGreaterEqual:
      case IntegerPredicate::SignedLessThan:
      case IntegerPredicate::SignedLessEqual:
        IsSigned = true;
        break;
      case IntegerPredicate::Equal:
      case IntegerPredicate::NotEqual:
      case IntegerPredicate::UnsignedGreaterThan:
      case IntegerPredicate::UnsignedGreaterEqual:
      case IntegerPredicate::UnsignedLessThan:
      case IntegerPredicate::UnsignedLessEqual:
        break;
      case IntegerPredicate::Count:
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_ICMP widening received an invalid integer predicate");
      }
      const GenericOpcode ExtensionOpcode =
          IsSigned || Info.isSExtCheaperThanZExt(Query->Types[TypeIndex], WideType)
              ? GenericOpcode::G_SEXT
              : GenericOpcode::G_ZEXT;
      if (auto Result = Apply(WidenSource(2, ExtensionOpcode)); !Result)
        return Result;
      if (auto Result = Apply(WidenSource(3, ExtensionOpcode)); !Result)
        return Result;
    }
    break;

  case GenericOpcode::G_FCMP:
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating comparison widening has an invalid type index");
    if (auto Result = Apply(WidenSource(2, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    if (auto Result = Apply(WidenSource(3, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_SELECT:
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenSource(2, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = Apply(WidenSource(3, GenericOpcode::G_ANYEXT)); !Result)
        return Result;
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "select widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_PHI: {
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_PHI widening requires type index zero");
    if (Instruction.operandCount() < 3 || Instruction.operandCount() % 2U == 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_PHI widening requires incoming value/block pairs");

    for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
      MachineOperand *ValueOperand = Instruction.operandAt(Index);
      const MachineOperand *BlockOperand = Instruction.operandAt(Index + 1U);
      const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
      const auto *Incoming =
          BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
      if (Value == nullptr || Incoming == nullptr || Incoming->Block == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_PHI widening has an invalid incoming pair");
      if (auto Point = setInsertPointAtFirstTerminator(MIRBuilder, *Incoming->Block); !Point)
        return std::unexpected(Point.error());
      auto Extension = MIRBuilder.buildInstr(opcode(GenericOpcode::G_ANYEXT), {WideType}, {*Value});
      if (!Extension)
        return std::unexpected(Extension.error());
      auto WideValue = resultRegister(**Extension);
      if (!WideValue)
        return std::unexpected(WideValue.error());
      if (auto Changed = ValueOperand->setRegister(*WideValue); !Changed)
        return std::unexpected(Changed.error());
    }

    MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    MachineBasicBlock *Block = Instruction.parent();
    if (Destination == nullptr || Block == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_PHI widening has an invalid destination");
    const Register OriginalDestination = *Destination;
    auto WideDestination = MRI.createVirtualRegister(WideType);
    if (!WideDestination)
      return std::unexpected(WideDestination.error());
    if (auto Changed = DestinationOperand->setRegister(*WideDestination); !Changed)
      return std::unexpected(Changed.error());
    if (auto Point = setInsertPointAfterPHIs(MIRBuilder, *Block); !Point)
      return std::unexpected(Point.error());
    if (auto Truncation = MIRBuilder.buildTrunc(OriginalDestination, *WideDestination);
        !Truncation) {
      return std::unexpected(Truncation.error());
    }
    break;
  }

  case GenericOpcode::G_FADD:
  case GenericOpcode::G_FMUL:
  case GenericOpcode::G_FSUB:
  case GenericOpcode::G_FMA:
  case GenericOpcode::G_FMAD:
  case GenericOpcode::G_FNEG:
  case GenericOpcode::G_FABS:
  case GenericOpcode::G_FCANONICALIZE:
  case GenericOpcode::G_FMINNUM:
  case GenericOpcode::G_FMAXNUM:
  case GenericOpcode::G_FMINNUM_IEEE:
  case GenericOpcode::G_FMAXNUM_IEEE:
  case GenericOpcode::G_FMINIMUM:
  case GenericOpcode::G_FMAXIMUM:
  case GenericOpcode::G_FMINIMUMNUM:
  case GenericOpcode::G_FMAXIMUMNUM:
  case GenericOpcode::G_FDIV:
  case GenericOpcode::G_FREM:
  case GenericOpcode::G_FCEIL:
  case GenericOpcode::G_FFLOOR:
  case GenericOpcode::G_FCOS:
  case GenericOpcode::G_FSIN:
  case GenericOpcode::G_FTAN:
  case GenericOpcode::G_FACOS:
  case GenericOpcode::G_FASIN:
  case GenericOpcode::G_FATAN:
  case GenericOpcode::G_FATAN2:
  case GenericOpcode::G_FCOSH:
  case GenericOpcode::G_FSINH:
  case GenericOpcode::G_FTANH:
  case GenericOpcode::G_FLOG10:
  case GenericOpcode::G_FLOG:
  case GenericOpcode::G_FLOG2:
  case GenericOpcode::G_FRINT:
  case GenericOpcode::G_FNEARBYINT:
  case GenericOpcode::G_FSQRT:
  case GenericOpcode::G_FEXP:
  case GenericOpcode::G_FEXP2:
  case GenericOpcode::G_FEXP10:
  case GenericOpcode::G_FPOW:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating operation widening has an invalid type index");
    for (std::size_t OperandIndex = 1; OperandIndex < Instruction.operandCount(); ++OperandIndex) {
      if (auto Result = Apply(WidenSource(OperandIndex, GenericOpcode::G_FPEXT)); !Result)
        return Result;
    }
    if (auto Result = Apply(WidenDestination(0, GenericOpcode::G_FPTRUNC)); !Result)
      return Result;
    break;

  case GenericOpcode::G_FPEXT:
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported,
                            "floating-point extension destination widening is not defined");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_FPTOSI:
  case GenericOpcode::G_FPTOUI:
  case GenericOpcode::G_INTRINSIC_LRINT:
  case GenericOpcode::G_INTRINSIC_LLRINT:
  case GenericOpcode::G_IS_FPCLASS:
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenDestination()); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating-to-integer widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_FPEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_SITOFP:
  case GenericOpcode::G_UITOFP:
    if (TypeIndex == 0) {
      if (auto Result = Apply(WidenDestination(0, GenericOpcode::G_FPTRUNC)); !Result)
        return Result;
      break;
    }
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer-to-floating widening has an invalid type index");
    if (auto Result = Apply(WidenSource(1, Instruction.opcode() == opcode(GenericOpcode::G_SITOFP)
                                               ? GenericOpcode::G_SEXT
                                               : GenericOpcode::G_ZEXT));
        !Result)
      return Result;
    break;

  case GenericOpcode::G_INTTOPTR:
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported,
                            "G_INTTOPTR destination widening is not defined");
    if (auto Result = Apply(WidenSource(1, GenericOpcode::G_ZEXT)); !Result)
      return Result;
    break;

  case GenericOpcode::G_PTRTOINT:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::Unsupported, "G_PTRTOINT source widening is not defined");
    if (auto Result = Apply(WidenDestination()); !Result)
      return Result;
    break;

  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "scalar widening is not implemented for this opcode");
  }

  Observer.changedInstr(Instruction);
  return {};
}

Expected<void> LegalizerHelper::narrowScalarAddSub(MachineInstr &Instruction, std::size_t TypeIndex,
                                                   LowLevelType NarrowType) {
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::Unsupported, "add/sub narrowing requires type index zero");

  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "add/sub narrowing requires a register destination");
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (DestinationType->isVector())
    return makeUnexpected(ErrorCode::Unsupported, "vector add/sub narrowing is not implemented");
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  GenericOpcode FirstOpcode;
  GenericOpcode MiddleOpcode;
  GenericOpcode FinalOpcode;
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_SADDO:
  case GenericOpcode::G_SADDE:
  case GenericOpcode::G_UADDO:
  case GenericOpcode::G_UADDE:
  case GenericOpcode::G_ADD:
    FirstOpcode = GenericOpcode::G_UADDO;
    MiddleOpcode = GenericOpcode::G_UADDE;
    FinalOpcode = Instruction.opcode() == opcode(GenericOpcode::G_SADDO) ||
                          Instruction.opcode() == opcode(GenericOpcode::G_SADDE)
                      ? GenericOpcode::G_SADDE
                      : GenericOpcode::G_UADDE;
    break;
  case GenericOpcode::G_SSUBO:
  case GenericOpcode::G_SSUBE:
  case GenericOpcode::G_USUBO:
  case GenericOpcode::G_USUBE:
  case GenericOpcode::G_SUB:
    FirstOpcode = GenericOpcode::G_USUBO;
    MiddleOpcode = GenericOpcode::G_USUBE;
    FinalOpcode = Instruction.opcode() == opcode(GenericOpcode::G_SSUBO) ||
                          Instruction.opcode() == opcode(GenericOpcode::G_SSUBE)
                      ? GenericOpcode::G_SSUBE
                      : GenericOpcode::G_USUBE;
    break;
  default:
    return makeUnexpected(ErrorCode::InternalError,
                          "unexpected add/sub opcode during scalar narrowing");
  }

  const InstructionDescriptor *Descriptor =
      MIRBuilder.target().instructionDescriptor(Instruction.opcode());
  if (Descriptor == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "add/sub narrowing has no instruction descriptor");
  const std::size_t DefinitionCount = Descriptor->NumExplicitDefs;
  const MachineOperand *FirstSourceOperand = Instruction.operandAt(DefinitionCount);
  const MachineOperand *SecondSourceOperand = Instruction.operandAt(DefinitionCount + 1);
  const Register *FirstSource =
      FirstSourceOperand == nullptr ? nullptr : FirstSourceOperand->getIf<Register>();
  const Register *SecondSource =
      SecondSourceOperand == nullptr ? nullptr : SecondSourceOperand->getIf<Register>();
  if (FirstSource == nullptr || SecondSource == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "add/sub narrowing requires two register sources");

  Register CarryDestination;
  Register CarryIn;
  if (DefinitionCount == 2) {
    const MachineOperand *CarryOperand = Instruction.operandAt(1);
    const Register *Carry = CarryOperand == nullptr ? nullptr : CarryOperand->getIf<Register>();
    if (Carry == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "add/sub narrowing has an invalid carry destination");
    CarryDestination = *Carry;
  }
  if (Instruction.operandCount() == DefinitionCount + 3) {
    const MachineOperand *CarryOperand = Instruction.operandAt(DefinitionCount + 2);
    const Register *Carry = CarryOperand == nullptr ? nullptr : CarryOperand->getIf<Register>();
    if (Carry == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "add/sub narrowing has an invalid carry input");
    CarryIn = *Carry;
  }

  LowLevelType LeftoverType;
  LowLevelType DummyType;
  std::vector<Register> FirstParts;
  std::vector<Register> SecondParts;
  std::vector<Register> FirstLeftovers;
  std::vector<Register> SecondLeftovers;
  std::vector<Register> DestinationParts;
  auto FirstExtracted = extractParts(*FirstSource, *DestinationType, NarrowType, LeftoverType,
                                     FirstParts, FirstLeftovers, MIRBuilder, MRI);
  if (!FirstExtracted)
    return std::unexpected(FirstExtracted.error());
  auto SecondExtracted = extractParts(*SecondSource, *DestinationType, NarrowType, DummyType,
                                      SecondParts, SecondLeftovers, MIRBuilder, MRI);
  if (!SecondExtracted)
    return std::unexpected(SecondExtracted.error());

  const std::size_t NarrowPartCount = FirstParts.size();
  FirstParts.insert(FirstParts.end(), FirstLeftovers.begin(), FirstLeftovers.end());
  SecondParts.insert(SecondParts.end(), SecondLeftovers.begin(), SecondLeftovers.end());
  if (FirstParts.size() != SecondParts.size())
    return makeUnexpected(ErrorCode::InternalError,
                          "add/sub source splits have different part counts");
  DestinationParts.reserve(FirstParts.size());

  auto BooleanType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!BooleanType)
    return std::unexpected(BooleanType.error());
  for (std::size_t Index = 0; Index != FirstParts.size(); ++Index) {
    auto PartType = MRI.getType(FirstParts[Index]);
    if (!PartType)
      return std::unexpected(PartType.error());
    auto PartDestination = MRI.createVirtualRegister(*PartType);
    if (!PartDestination)
      return std::unexpected(PartDestination.error());

    Register CarryOut;
    if (Index + 1 == FirstParts.size() && !CarryDestination.isNoRegister()) {
      CarryOut = CarryDestination;
    } else {
      auto NewCarry = MRI.createVirtualRegister(*BooleanType);
      if (!NewCarry)
        return std::unexpected(NewCarry.error());
      CarryOut = *NewCarry;
    }

    Expected<MachineInstr *> Built;
    if (CarryIn.isNoRegister()) {
      Built = MIRBuilder.buildInstr(opcode(FirstOpcode), {*PartDestination, CarryOut},
                                    {FirstParts[Index], SecondParts[Index]});
    } else if (Index + 1 == FirstParts.size()) {
      Built = MIRBuilder.buildInstr(opcode(FinalOpcode), {*PartDestination, CarryOut},
                                    {FirstParts[Index], SecondParts[Index], CarryIn});
    } else {
      Built = MIRBuilder.buildInstr(opcode(MiddleOpcode), {*PartDestination, CarryOut},
                                    {FirstParts[Index], SecondParts[Index], CarryIn});
    }
    if (!Built)
      return std::unexpected(Built.error());
    DestinationParts.push_back(*PartDestination);
    CarryIn = CarryOut;
  }

  std::vector<Register> MainDestinations(DestinationParts.begin(),
                                         DestinationParts.begin() +
                                             static_cast<std::ptrdiff_t>(NarrowPartCount));
  std::vector<Register> LeftoverDestinations(DestinationParts.begin() +
                                                 static_cast<std::ptrdiff_t>(NarrowPartCount),
                                             DestinationParts.end());
  auto Inserted = insertParts(*Destination, *DestinationType, NarrowType, MainDestinations,
                              LeftoverType, LeftoverDestinations);
  if (!Inserted)
    return std::unexpected(Inserted.error());
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarMul(MachineInstr &Instruction, std::size_t TypeIndex,
                                                LowLevelType NarrowType) {
  if (TypeIndex != 0 || Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::Unsupported,
                          "multiply narrowing requires type index zero and two sources");
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *FirstSourceOperand = Instruction.operandAt(1);
  const MachineOperand *SecondSourceOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *FirstSource =
      FirstSourceOperand == nullptr ? nullptr : FirstSourceOperand->getIf<Register>();
  const Register *SecondSource =
      SecondSourceOperand == nullptr ? nullptr : SecondSourceOperand->getIf<Register>();
  if (Destination == nullptr || FirstSource == nullptr || SecondSource == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply narrowing requires register operands");
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (DestinationType->isVector() || !NarrowType.isCategoryNeutralScalar())
    return makeUnexpected(ErrorCode::Unsupported,
                          "multiply narrowing requires scalar integer types");

  const std::uint64_t DestinationSize = DestinationType->minimumBitWidth();
  const std::uint64_t NarrowSize = NarrowType.minimumBitWidth();
  if (NarrowSize == 0 || NarrowSize >= DestinationSize || DestinationSize % NarrowSize != 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "multiply narrowing requires an even smaller scalar type");
  const std::size_t PartCount = static_cast<std::size_t>(DestinationSize / NarrowSize);
  const bool IsMultiplyHigh = Instruction.opcode() == opcode(GenericOpcode::G_UMULH);
  const std::size_t TemporaryPartCount = PartCount * (IsMultiplyHigh ? 2U : 1U);

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  std::vector<Register> FirstParts;
  std::vector<Register> SecondParts;
  std::vector<Register> TemporaryDestinations(TemporaryPartCount);
  if (auto Extracted = extractParts(*FirstSource, NarrowType, static_cast<int>(PartCount),
                                    FirstParts, MIRBuilder, MRI);
      !Extracted) {
    return std::unexpected(Extracted.error());
  }
  if (auto Extracted = extractParts(*SecondSource, NarrowType, static_cast<int>(PartCount),
                                    SecondParts, MIRBuilder, MRI);
      !Extracted) {
    return std::unexpected(Extracted.error());
  }
  if (auto Multiplied =
          multiplyRegisters(TemporaryDestinations, FirstParts, SecondParts, NarrowType);
      !Multiplied) {
    return std::unexpected(Multiplied.error());
  }

  std::vector<Register> ResultParts(TemporaryDestinations.end() -
                                        static_cast<std::ptrdiff_t>(PartCount),
                                    TemporaryDestinations.end());
  if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, ResultParts); !Built)
    return std::unexpected(Built.error());
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarExt(MachineInstr &Instruction, std::size_t TypeIndex,
                                                LowLevelType NarrowType) {
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::Unsupported, "extension narrowing requires type index zero");

  const auto Destination = Instruction.operandAt(0)->registerValue();
  const auto Source = Instruction.operandAt(1)->registerValue();
  if (!Destination || !Source)
    return makeUnexpected(ErrorCode::VerificationError,
                          "extension narrowing requires register operands");

  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (DestinationType->isVector())
    return makeUnexpected(ErrorCode::Unsupported, "vector extension narrowing is not implemented");

  std::vector<Register> Parts;
  auto GCDType = extractGCDType(Parts, *DestinationType, NarrowType, *Source);
  if (!GCDType)
    return std::unexpected(GCDType.error());
  auto LCMType = buildLCMMergePieces(*DestinationType, NarrowType, *GCDType, Parts,
                                     static_cast<GenericOpcode>(Instruction.opcode().value()));
  if (!LCMType)
    return std::unexpected(LCMType.error());
  auto Remerged = buildWidenedRemergeToDst(*Destination, *LCMType, Parts);
  if (!Remerged)
    return std::unexpected(Remerged.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarBasic(MachineInstr &Instruction, std::size_t TypeIndex,
                                                  LowLevelType NarrowType) {
  if (TypeIndex != 0 || Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::Unsupported,
                          "basic scalar narrowing requires type index zero and two sources");

  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *FirstSource = Instruction.operandAt(1)->getIf<Register>();
  const Register *SecondSource = Instruction.operandAt(2)->getIf<Register>();
  if (Destination == nullptr || FirstSource == nullptr || SecondSource == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "basic scalar narrowing requires register operands");
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());

  LowLevelType LeftoverType;
  LowLevelType UnusedType;
  std::vector<Register> FirstParts;
  std::vector<Register> FirstLeftovers;
  std::vector<Register> SecondParts;
  std::vector<Register> SecondLeftovers;
  auto FirstExtracted = extractParts(*FirstSource, *DestinationType, NarrowType, LeftoverType,
                                     FirstParts, FirstLeftovers, MIRBuilder, MRI);
  if (!FirstExtracted)
    return std::unexpected(FirstExtracted.error());
  auto SecondExtracted = extractParts(*SecondSource, *DestinationType, NarrowType, UnusedType,
                                      SecondParts, SecondLeftovers, MIRBuilder, MRI);
  if (!SecondExtracted)
    return std::unexpected(SecondExtracted.error());
  if (!*FirstExtracted || !*SecondExtracted || FirstParts.size() != SecondParts.size() ||
      FirstLeftovers.size() != SecondLeftovers.size()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "basic scalar operands cannot be split consistently");
  }

  std::vector<Register> DestinationParts;
  DestinationParts.reserve(FirstParts.size());
  for (std::size_t Index = 0; Index != FirstParts.size(); ++Index) {
    auto Built = MIRBuilder.buildInstr(Instruction.opcode(), {NarrowType},
                                       {FirstParts[Index], SecondParts[Index]});
    if (!Built)
      return std::unexpected(Built.error());
    auto Result = resultRegister(**Built);
    if (!Result)
      return std::unexpected(Result.error());
    DestinationParts.push_back(*Result);
  }

  std::vector<Register> DestinationLeftovers;
  DestinationLeftovers.reserve(FirstLeftovers.size());
  for (std::size_t Index = 0; Index != FirstLeftovers.size(); ++Index) {
    auto Built = MIRBuilder.buildInstr(Instruction.opcode(), {LeftoverType},
                                       {FirstLeftovers[Index], SecondLeftovers[Index]});
    if (!Built)
      return std::unexpected(Built.error());
    auto Result = resultRegister(**Built);
    if (!Result)
      return std::unexpected(Result.error());
    DestinationLeftovers.push_back(*Result);
  }

  if (auto Inserted = insertParts(*Destination, *DestinationType, NarrowType, DestinationParts,
                                  LeftoverType, DestinationLeftovers);
      !Inserted) {
    return std::unexpected(Inserted.error());
  }
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarSelect(MachineInstr &Instruction, std::size_t TypeIndex,
                                                   LowLevelType NarrowType) {
  if (TypeIndex != 0 || Instruction.operandCount() != 4)
    return makeUnexpected(ErrorCode::Unsupported,
                          "select narrowing requires type index zero and three sources");

  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *Condition = Instruction.operandAt(1)->getIf<Register>();
  const Register *TrueSource = Instruction.operandAt(2)->getIf<Register>();
  const Register *FalseSource = Instruction.operandAt(3)->getIf<Register>();
  if (Destination == nullptr || Condition == nullptr || TrueSource == nullptr ||
      FalseSource == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "select narrowing requires register operands");
  }
  auto DestinationType = MRI.getType(*Destination);
  auto ConditionType = MRI.getType(*Condition);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!ConditionType)
    return std::unexpected(ConditionType.error());
  if (ConditionType->isVector())
    return makeUnexpected(ErrorCode::Unsupported, "vector select narrowing is not implemented");

  LowLevelType LeftoverType;
  LowLevelType UnusedType;
  std::vector<Register> TrueParts;
  std::vector<Register> TrueLeftovers;
  std::vector<Register> FalseParts;
  std::vector<Register> FalseLeftovers;
  auto TrueExtracted = extractParts(*TrueSource, *DestinationType, NarrowType, LeftoverType,
                                    TrueParts, TrueLeftovers, MIRBuilder, MRI);
  if (!TrueExtracted)
    return std::unexpected(TrueExtracted.error());
  auto FalseExtracted = extractParts(*FalseSource, *DestinationType, NarrowType, UnusedType,
                                     FalseParts, FalseLeftovers, MIRBuilder, MRI);
  if (!FalseExtracted)
    return std::unexpected(FalseExtracted.error());
  if (!*TrueExtracted || !*FalseExtracted || TrueParts.size() != FalseParts.size() ||
      TrueLeftovers.size() != FalseLeftovers.size()) {
    return makeUnexpected(ErrorCode::Unsupported, "select operands cannot be split consistently");
  }

  std::vector<Register> DestinationParts;
  DestinationParts.reserve(TrueParts.size());
  for (std::size_t Index = 0; Index != TrueParts.size(); ++Index) {
    auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {NarrowType},
                                       {*Condition, TrueParts[Index], FalseParts[Index]});
    if (!Built)
      return std::unexpected(Built.error());
    auto Result = resultRegister(**Built);
    if (!Result)
      return std::unexpected(Result.error());
    DestinationParts.push_back(*Result);
  }

  std::vector<Register> DestinationLeftovers;
  DestinationLeftovers.reserve(TrueLeftovers.size());
  for (std::size_t Index = 0; Index != TrueLeftovers.size(); ++Index) {
    auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {LeftoverType},
                                       {*Condition, TrueLeftovers[Index], FalseLeftovers[Index]});
    if (!Built)
      return std::unexpected(Built.error());
    auto Result = resultRegister(**Built);
    if (!Result)
      return std::unexpected(Result.error());
    DestinationLeftovers.push_back(*Result);
  }

  if (auto Inserted = insertParts(*Destination, *DestinationType, NarrowType, DestinationParts,
                                  LeftoverType, DestinationLeftovers);
      !Inserted) {
    return std::unexpected(Inserted.error());
  }
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarShiftByConstant(MachineInstr &Instruction,
                                                            const APInt &Amount,
                                                            LowLevelType HalfType,
                                                            LowLevelType AmountType) {
  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *Source = Instruction.operandAt(1)->getIf<Register>();
  if (Destination == nullptr || Source == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "constant shift narrowing requires register operands");

  auto LowPart = MRI.createVirtualRegister(HalfType);
  auto HighPart = MRI.createVirtualRegister(HalfType);
  if (!LowPart)
    return std::unexpected(LowPart.error());
  if (!HighPart)
    return std::unexpected(HighPart.error());
  if (auto Built = MIRBuilder.buildUnmergeValues({*LowPart, *HighPart}, *Source); !Built)
    return std::unexpected(Built.error());

  if (Amount.isZero()) {
    if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, {*LowPart, *HighPart}); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }

  const std::uint64_t HalfBits = HalfType.minimumBitWidth();
  const std::uint64_t TotalBits = HalfBits * 2U;
  const bool AmountExceeds64Bits = Amount.activeBitWidth() > 64;
  const std::uint64_t AmountValue = Amount.word(0);
  const bool ExceedsTotal = AmountExceeds64Bits || AmountValue > TotalBits;
  const bool ExceedsHalf = AmountExceeds64Bits || AmountValue > HalfBits;

  const auto BuildConstant = [&](LowLevelType Type, std::uint64_t Value) {
    return buildConstantRegister(MIRBuilder, Type, static_cast<std::int64_t>(Value));
  };
  const auto BuildBinary = [&](GenericOpcode Generic, Register Left, Register Right) {
    return buildResultInstruction(MIRBuilder, Generic, HalfType, {Left, Right});
  };

  Register LowResult;
  Register HighResult;
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic == GenericOpcode::G_SHL) {
    if (ExceedsTotal) {
      auto Zero = BuildConstant(HalfType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      LowResult = *Zero;
      HighResult = *Zero;
    } else if (ExceedsHalf) {
      auto Zero = BuildConstant(HalfType, 0);
      auto Shift = BuildConstant(AmountType, AmountValue - HalfBits);
      if (!Zero)
        return std::unexpected(Zero.error());
      if (!Shift)
        return std::unexpected(Shift.error());
      auto Shifted = BuildBinary(GenericOpcode::G_SHL, *LowPart, *Shift);
      if (!Shifted)
        return std::unexpected(Shifted.error());
      LowResult = *Zero;
      HighResult = *Shifted;
    } else if (AmountValue == HalfBits) {
      auto Zero = BuildConstant(HalfType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      LowResult = *Zero;
      HighResult = *LowPart;
    } else {
      auto Shift = BuildConstant(AmountType, AmountValue);
      auto InverseShift = BuildConstant(AmountType, HalfBits - AmountValue);
      if (!Shift)
        return std::unexpected(Shift.error());
      if (!InverseShift)
        return std::unexpected(InverseShift.error());
      auto LowShift = BuildBinary(GenericOpcode::G_SHL, *LowPart, *Shift);
      auto HighShift = BuildBinary(GenericOpcode::G_SHL, *HighPart, *Shift);
      auto Carry = BuildBinary(GenericOpcode::G_LSHR, *LowPart, *InverseShift);
      if (!LowShift)
        return std::unexpected(LowShift.error());
      if (!HighShift)
        return std::unexpected(HighShift.error());
      if (!Carry)
        return std::unexpected(Carry.error());
      auto Combined = BuildBinary(GenericOpcode::G_OR, *HighShift, *Carry);
      if (!Combined)
        return std::unexpected(Combined.error());
      LowResult = *LowShift;
      HighResult = *Combined;
    }
  } else if (Generic == GenericOpcode::G_LSHR) {
    if (ExceedsTotal) {
      auto Zero = BuildConstant(HalfType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      LowResult = *Zero;
      HighResult = *Zero;
    } else if (ExceedsHalf) {
      auto Shift = BuildConstant(AmountType, AmountValue - HalfBits);
      auto Zero = BuildConstant(HalfType, 0);
      if (!Shift)
        return std::unexpected(Shift.error());
      if (!Zero)
        return std::unexpected(Zero.error());
      auto Shifted = BuildBinary(GenericOpcode::G_LSHR, *HighPart, *Shift);
      if (!Shifted)
        return std::unexpected(Shifted.error());
      LowResult = *Shifted;
      HighResult = *Zero;
    } else if (AmountValue == HalfBits) {
      auto Zero = BuildConstant(HalfType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      LowResult = *HighPart;
      HighResult = *Zero;
    } else {
      auto Shift = BuildConstant(AmountType, AmountValue);
      auto InverseShift = BuildConstant(AmountType, HalfBits - AmountValue);
      if (!Shift)
        return std::unexpected(Shift.error());
      if (!InverseShift)
        return std::unexpected(InverseShift.error());
      auto LowShift = BuildBinary(GenericOpcode::G_LSHR, *LowPart, *Shift);
      auto Carry = BuildBinary(GenericOpcode::G_SHL, *HighPart, *InverseShift);
      auto HighShift = BuildBinary(GenericOpcode::G_LSHR, *HighPart, *Shift);
      if (!LowShift)
        return std::unexpected(LowShift.error());
      if (!Carry)
        return std::unexpected(Carry.error());
      if (!HighShift)
        return std::unexpected(HighShift.error());
      auto Combined = BuildBinary(GenericOpcode::G_OR, *LowShift, *Carry);
      if (!Combined)
        return std::unexpected(Combined.error());
      LowResult = *Combined;
      HighResult = *HighShift;
    }
  } else if (Generic == GenericOpcode::G_ASHR) {
    auto SignShift = BuildConstant(AmountType, HalfBits - 1U);
    if (!SignShift)
      return std::unexpected(SignShift.error());
    auto Sign = BuildBinary(GenericOpcode::G_ASHR, *HighPart, *SignShift);
    if (!Sign)
      return std::unexpected(Sign.error());
    if (ExceedsTotal) {
      LowResult = *Sign;
      HighResult = *Sign;
    } else if (ExceedsHalf) {
      auto Shift = BuildConstant(AmountType, AmountValue - HalfBits);
      if (!Shift)
        return std::unexpected(Shift.error());
      auto Shifted = BuildBinary(GenericOpcode::G_ASHR, *HighPart, *Shift);
      if (!Shifted)
        return std::unexpected(Shifted.error());
      LowResult = *Shifted;
      HighResult = *Sign;
    } else if (AmountValue == HalfBits) {
      LowResult = *HighPart;
      HighResult = *Sign;
    } else {
      auto Shift = BuildConstant(AmountType, AmountValue);
      auto InverseShift = BuildConstant(AmountType, HalfBits - AmountValue);
      if (!Shift)
        return std::unexpected(Shift.error());
      if (!InverseShift)
        return std::unexpected(InverseShift.error());
      auto LowShift = BuildBinary(GenericOpcode::G_LSHR, *LowPart, *Shift);
      auto Carry = BuildBinary(GenericOpcode::G_SHL, *HighPart, *InverseShift);
      auto HighShift = BuildBinary(GenericOpcode::G_ASHR, *HighPart, *Shift);
      if (!LowShift)
        return std::unexpected(LowShift.error());
      if (!Carry)
        return std::unexpected(Carry.error());
      if (!HighShift)
        return std::unexpected(HighShift.error());
      auto Combined = BuildBinary(GenericOpcode::G_OR, *LowShift, *Carry);
      if (!Combined)
        return std::unexpected(Combined.error());
      LowResult = *Combined;
      HighResult = *HighShift;
    }
  } else {
    return makeUnexpected(ErrorCode::InternalError,
                          "constant shift narrowing received a non-shift opcode");
  }

  if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, {LowResult, HighResult}); !Built)
    return std::unexpected(Built.error());
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarShift(MachineInstr &Instruction, std::size_t TypeIndex,
                                                  LowLevelType NarrowType) {
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "shift narrowing requires one destination and two sources");
  if (TypeIndex == 1) {
    Observer.changingInstr(Instruction);
    auto Narrowed = narrowScalarSrc(Instruction, NarrowType, 2);
    if (!Narrowed)
      return std::unexpected(Narrowed.error());
    Observer.changedInstr(Instruction);
    return {};
  }
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::Unsupported, "shift narrowing has an invalid type index");

  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *Source = Instruction.operandAt(1)->getIf<Register>();
  const Register *Amount = Instruction.operandAt(2)->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || Amount == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "shift narrowing requires register operands");
  auto DestinationType = MRI.getType(*Destination);
  auto AmountType = MRI.getType(*Amount);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!AmountType)
    return std::unexpected(AmountType.error());
  if (!DestinationType->isCategoryNeutralScalar())
    return makeUnexpected(ErrorCode::Unsupported, "vector shift narrowing is not implemented");

  const std::uint64_t DestinationBits = DestinationType->scalarBitWidth();
  if (DestinationBits == 0 || DestinationBits % 2U != 0)
    return makeUnexpected(ErrorCode::Unsupported, "shift narrowing requires an even scalar width");

  if (NarrowType.isCategoryNeutralScalar() && NarrowType.scalarBitWidth() != 0 &&
      DestinationBits % NarrowType.scalarBitWidth() == 0 &&
      DestinationBits / NarrowType.scalarBitWidth() >= 8U) {
    return narrowScalarShiftMultiway(Instruction, NarrowType);
  }

  auto HalfType = LowLevelType::makeCategoryNeutralScalar(DestinationBits / 2U);
  if (!HalfType)
    return std::unexpected(HalfType.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Constant = getIConstantVRegValWithLookThrough(*Amount, MRI))
    return narrowScalarShiftByConstant(Instruction, Constant->Value, *HalfType, *AmountType);

  auto HalfBits = buildConstantRegister(MIRBuilder, *AmountType,
                                        static_cast<std::int64_t>(DestinationBits / 2U));
  if (!HalfBits)
    return std::unexpected(HalfBits.error());
  auto LowPart = MRI.createVirtualRegister(*HalfType);
  auto HighPart = MRI.createVirtualRegister(*HalfType);
  if (!LowPart)
    return std::unexpected(LowPart.error());
  if (!HighPart)
    return std::unexpected(HighPart.error());
  if (auto Built = MIRBuilder.buildUnmergeValues({*LowPart, *HighPart}, *Source); !Built)
    return std::unexpected(Built.error());

  auto AmountExcess =
      buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *AmountType, {*Amount, *HalfBits});
  auto AmountLack =
      buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *AmountType, {*HalfBits, *Amount});
  auto ZeroAmount = buildConstantRegister(MIRBuilder, *AmountType, 0);
  auto ConditionType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!AmountExcess)
    return std::unexpected(AmountExcess.error());
  if (!AmountLack)
    return std::unexpected(AmountLack.error());
  if (!ZeroAmount)
    return std::unexpected(ZeroAmount.error());
  if (!ConditionType)
    return std::unexpected(ConditionType.error());
  auto IsShort = buildICmpRegister(MIRBuilder, IntegerPredicate::UnsignedLessThan, *ConditionType,
                                   *Amount, *HalfBits);
  auto IsZero =
      buildICmpRegister(MIRBuilder, IntegerPredicate::Equal, *ConditionType, *Amount, *ZeroAmount);
  if (!IsShort)
    return std::unexpected(IsShort.error());
  if (!IsZero)
    return std::unexpected(IsZero.error());

  const auto Build = [&](GenericOpcode Generic, Register Left, Register Right) {
    return buildResultInstruction(MIRBuilder, Generic, *HalfType, {Left, Right});
  };
  const auto Select = [&](Register Condition, Register TrueValue, Register FalseValue) {
    return buildResultInstruction(MIRBuilder, GenericOpcode::G_SELECT, *HalfType,
                                  {Condition, TrueValue, FalseValue});
  };

  Register LowResult;
  Register HighResult;
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic == GenericOpcode::G_SHL) {
    auto LowShort = Build(GenericOpcode::G_SHL, *LowPart, *Amount);
    auto LowCarry = Build(GenericOpcode::G_LSHR, *LowPart, *AmountLack);
    auto HighShift = Build(GenericOpcode::G_SHL, *HighPart, *Amount);
    if (!LowShort)
      return std::unexpected(LowShort.error());
    if (!LowCarry)
      return std::unexpected(LowCarry.error());
    if (!HighShift)
      return std::unexpected(HighShift.error());
    auto HighShort = Build(GenericOpcode::G_OR, *LowCarry, *HighShift);
    auto LowLong = buildConstantRegister(MIRBuilder, *HalfType, 0);
    auto HighLong = Build(GenericOpcode::G_SHL, *LowPart, *AmountExcess);
    if (!HighShort)
      return std::unexpected(HighShort.error());
    if (!LowLong)
      return std::unexpected(LowLong.error());
    if (!HighLong)
      return std::unexpected(HighLong.error());
    auto Low = Select(*IsShort, *LowShort, *LowLong);
    auto HighNonZero = Select(*IsShort, *HighShort, *HighLong);
    if (!Low)
      return std::unexpected(Low.error());
    if (!HighNonZero)
      return std::unexpected(HighNonZero.error());
    auto High = Select(*IsZero, *HighPart, *HighNonZero);
    if (!High)
      return std::unexpected(High.error());
    LowResult = *Low;
    HighResult = *High;
  } else if (Generic == GenericOpcode::G_LSHR || Generic == GenericOpcode::G_ASHR) {
    auto HighShort = Build(Generic, *HighPart, *Amount);
    auto LowShift = Build(GenericOpcode::G_LSHR, *LowPart, *Amount);
    auto HighCarry = Build(GenericOpcode::G_SHL, *HighPart, *AmountLack);
    if (!HighShort)
      return std::unexpected(HighShort.error());
    if (!LowShift)
      return std::unexpected(LowShift.error());
    if (!HighCarry)
      return std::unexpected(HighCarry.error());
    auto LowShort = Build(GenericOpcode::G_OR, *LowShift, *HighCarry);
    if (!LowShort)
      return std::unexpected(LowShort.error());

    Expected<Register> HighLong =
        Generic == GenericOpcode::G_LSHR
            ? buildConstantRegister(MIRBuilder, *HalfType, 0)
            : buildConstantRegister(MIRBuilder, *AmountType,
                                    static_cast<std::int64_t>(DestinationBits / 2U - 1U))
                  .and_then([&](Register Shift) {
                    return Build(GenericOpcode::G_ASHR, *HighPart, Shift);
                  });
    auto LowLong = Build(Generic, *HighPart, *AmountExcess);
    if (!HighLong)
      return std::unexpected(HighLong.error());
    if (!LowLong)
      return std::unexpected(LowLong.error());
    auto LowNonZero = Select(*IsShort, *LowShort, *LowLong);
    if (!LowNonZero)
      return std::unexpected(LowNonZero.error());
    auto Low = Select(*IsZero, *LowPart, *LowNonZero);
    auto High = Select(*IsShort, *HighShort, *HighLong);
    if (!Low)
      return std::unexpected(Low.error());
    if (!High)
      return std::unexpected(High.error());
    LowResult = *Low;
    HighResult = *High;
  } else {
    return makeUnexpected(ErrorCode::InternalError, "shift narrowing received a non-shift opcode");
  }

  if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, {LowResult, HighResult}); !Built)
    return std::unexpected(Built.error());
  return Instruction.eraseFromParent();
}

Expected<Register> LegalizerHelper::buildConstantShiftPart(GenericOpcode Generic,
                                                           std::size_t PartIndex,
                                                           std::size_t PartCount,
                                                           const std::vector<Register> &SourceParts,
                                                           const ShiftParameters &Parameters,
                                                           LowLevelType TargetType) {
  const auto WordShift = getIConstantVRegValWithLookThrough(Parameters.WordShift, MRI, false);
  const auto BitShift = getIConstantVRegValWithLookThrough(Parameters.BitShift, MRI, false);
  if (!WordShift || !BitShift || WordShift->Value.activeBitWidth() > 64 ||
      BitShift->Value.activeBitWidth() > 64) {
    return makeUnexpected(ErrorCode::InternalError,
                          "multiway shift parameters are not representable constants");
  }
  const std::uint64_t ShiftWords = WordShift->Value.word(0);
  const std::uint64_t ShiftBits = BitShift->Value.word(0);
  const bool NeedsCarry = ShiftBits != 0;
  const auto Build = [&](GenericOpcode Opcode, Register Left, Register Right) {
    return buildResultInstruction(MIRBuilder, Opcode, TargetType, {Left, Right});
  };

  if (Generic == GenericOpcode::G_SHL) {
    if (PartIndex < ShiftWords)
      return Parameters.Zero;
    const std::size_t SourceIndex = PartIndex - static_cast<std::size_t>(ShiftWords);
    if (!NeedsCarry)
      return SourceParts[SourceIndex];
    auto High = Build(GenericOpcode::G_SHL, SourceParts[SourceIndex], Parameters.BitShift);
    if (!High)
      return std::unexpected(High.error());
    if (SourceIndex == 0)
      return *High;
    auto Low =
        Build(GenericOpcode::G_LSHR, SourceParts[SourceIndex - 1U], Parameters.InverseBitShift);
    if (!Low)
      return std::unexpected(Low.error());
    return Build(GenericOpcode::G_OR, *High, *Low);
  }

  if (Generic == GenericOpcode::G_LSHR) {
    const std::uint64_t SourceIndex = PartIndex + ShiftWords;
    if (SourceIndex >= PartCount)
      return Parameters.Zero;
    if (!NeedsCarry)
      return SourceParts[static_cast<std::size_t>(SourceIndex)];
    auto Low = Build(GenericOpcode::G_LSHR, SourceParts[static_cast<std::size_t>(SourceIndex)],
                     Parameters.BitShift);
    if (!Low)
      return std::unexpected(Low.error());
    if (SourceIndex + 1U >= PartCount)
      return *Low;
    auto High = Build(GenericOpcode::G_SHL, SourceParts[static_cast<std::size_t>(SourceIndex + 1U)],
                      Parameters.InverseBitShift);
    if (!High)
      return std::unexpected(High.error());
    return Build(GenericOpcode::G_OR, *Low, *High);
  }

  if (Generic == GenericOpcode::G_ASHR) {
    const std::uint64_t SourceIndex = PartIndex + ShiftWords;
    if (SourceIndex >= PartCount)
      return Parameters.SignBit;
    if (!NeedsCarry)
      return SourceParts[static_cast<std::size_t>(SourceIndex)];
    const GenericOpcode MainOpcode =
        SourceIndex + 1U == PartCount ? GenericOpcode::G_ASHR : GenericOpcode::G_LSHR;
    auto Low =
        Build(MainOpcode, SourceParts[static_cast<std::size_t>(SourceIndex)], Parameters.BitShift);
    if (!Low)
      return std::unexpected(Low.error());
    const Register HighSource = SourceIndex + 1U < PartCount
                                    ? SourceParts[static_cast<std::size_t>(SourceIndex + 1U)]
                                    : Parameters.SignBit;
    auto High = Build(GenericOpcode::G_SHL, HighSource, Parameters.InverseBitShift);
    if (!High)
      return std::unexpected(High.error());
    return Build(GenericOpcode::G_OR, *Low, *High);
  }

  return makeUnexpected(ErrorCode::InternalError,
                        "constant shift part received a non-shift opcode");
}

Expected<Register> LegalizerHelper::buildVariableShiftPart(GenericOpcode Generic,
                                                           Register MainOperand,
                                                           Register ShiftAmount,
                                                           LowLevelType TargetType,
                                                           Register CarryOperand) {
  const GenericOpcode MainOpcode =
      Generic == GenericOpcode::G_ASHR ? GenericOpcode::G_LSHR : Generic;
  auto MainShifted =
      buildResultInstruction(MIRBuilder, MainOpcode, TargetType, {MainOperand, ShiftAmount});
  if (!MainShifted)
    return std::unexpected(MainShifted.error());
  if (CarryOperand.isNoRegister())
    return *MainShifted;

  auto AmountType = MRI.getType(ShiftAmount);
  if (!AmountType)
    return std::unexpected(AmountType.error());
  auto ZeroAmount = buildConstantRegister(MIRBuilder, *AmountType, 0);
  auto BoolType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!ZeroAmount)
    return std::unexpected(ZeroAmount.error());
  if (!BoolType)
    return std::unexpected(BoolType.error());
  auto IsZero =
      buildICmpRegister(MIRBuilder, IntegerPredicate::Equal, *BoolType, ShiftAmount, *ZeroAmount);
  auto TargetBits = buildConstantRegister(MIRBuilder, *AmountType,
                                          static_cast<std::int64_t>(TargetType.scalarBitWidth()));
  if (!IsZero)
    return std::unexpected(IsZero.error());
  if (!TargetBits)
    return std::unexpected(TargetBits.error());
  auto InverseAmount = buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *AmountType,
                                              {*TargetBits, ShiftAmount});
  if (!InverseAmount)
    return std::unexpected(InverseAmount.error());

  const GenericOpcode CarryOpcode =
      Generic == GenericOpcode::G_SHL ? GenericOpcode::G_LSHR : GenericOpcode::G_SHL;
  auto CarryBits =
      buildResultInstruction(MIRBuilder, CarryOpcode, TargetType, {CarryOperand, *InverseAmount});
  auto Zero = buildConstantRegister(MIRBuilder, TargetType, 0);
  if (!CarryBits)
    return std::unexpected(CarryBits.error());
  if (!Zero)
    return std::unexpected(Zero.error());
  auto SafeCarry = buildResultInstruction(MIRBuilder, GenericOpcode::G_SELECT, TargetType,
                                          {*IsZero, *Zero, *CarryBits});
  if (!SafeCarry)
    return std::unexpected(SafeCarry.error());
  return buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, TargetType,
                                {*MainShifted, *SafeCarry});
}

Expected<void> LegalizerHelper::narrowScalarShiftByConstantMultiway(MachineInstr &Instruction,
                                                                    const APInt &Amount,
                                                                    LowLevelType TargetType,
                                                                    LowLevelType AmountType) {
  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *Source = Instruction.operandAt(1)->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || Amount.activeBitWidth() > 64)
    return makeUnexpected(ErrorCode::Unsupported,
                          "constant multiway shift requires representable register operands");
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  const std::uint64_t DestinationBits = DestinationType->scalarBitWidth();
  const std::uint64_t TargetBits = TargetType.scalarBitWidth();
  if (TargetBits == 0 || DestinationBits % TargetBits != 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiway shift target type does not divide the destination");
  const std::size_t PartCount = static_cast<std::size_t>(DestinationBits / TargetBits);

  std::vector<Register> SourceParts;
  auto Extracted =
      extractParts(*Source, TargetType, static_cast<int>(PartCount), SourceParts, MIRBuilder, MRI);
  if (!Extracted)
    return std::unexpected(Extracted.error());
  if (Amount.isZero()) {
    if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, SourceParts); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }

  const std::uint64_t AmountValue = Amount.word(0);
  const std::uint64_t ShiftWords = AmountValue / TargetBits;
  const std::uint64_t ShiftBits = AmountValue % TargetBits;
  ShiftParameters Parameters;
  auto WordShift =
      buildConstantRegister(MIRBuilder, AmountType, static_cast<std::int64_t>(ShiftWords));
  auto BitShift =
      buildConstantRegister(MIRBuilder, AmountType, static_cast<std::int64_t>(ShiftBits));
  auto InverseBitShift = buildConstantRegister(MIRBuilder, AmountType,
                                               static_cast<std::int64_t>(TargetBits - ShiftBits));
  auto Zero = buildConstantRegister(MIRBuilder, TargetType, 0);
  if (!WordShift)
    return std::unexpected(WordShift.error());
  if (!BitShift)
    return std::unexpected(BitShift.error());
  if (!InverseBitShift)
    return std::unexpected(InverseBitShift.error());
  if (!Zero)
    return std::unexpected(Zero.error());
  Parameters.WordShift = *WordShift;
  Parameters.BitShift = *BitShift;
  Parameters.InverseBitShift = *InverseBitShift;
  Parameters.Zero = *Zero;

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic == GenericOpcode::G_ASHR) {
    auto SignAmount =
        buildConstantRegister(MIRBuilder, AmountType, static_cast<std::int64_t>(TargetBits - 1U));
    if (!SignAmount)
      return std::unexpected(SignAmount.error());
    auto SignBit = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, TargetType,
                                          {SourceParts.back(), *SignAmount});
    if (!SignBit)
      return std::unexpected(SignBit.error());
    Parameters.SignBit = *SignBit;
  }

  std::vector<Register> DestinationParts;
  DestinationParts.reserve(PartCount);
  for (std::size_t Index = 0; Index != PartCount; ++Index) {
    auto Part =
        buildConstantShiftPart(Generic, Index, PartCount, SourceParts, Parameters, TargetType);
    if (!Part)
      return std::unexpected(Part.error());
    DestinationParts.push_back(*Part);
  }
  if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, DestinationParts); !Built)
    return std::unexpected(Built.error());
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarShiftMultiway(MachineInstr &Instruction,
                                                          LowLevelType TargetType) {
  const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
  const Register *Source = Instruction.operandAt(1)->getIf<Register>();
  const Register *Amount = Instruction.operandAt(2)->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || Amount == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiway shift narrowing requires register operands");
  auto DestinationType = MRI.getType(*Destination);
  auto AmountType = MRI.getType(*Amount);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!AmountType)
    return std::unexpected(AmountType.error());
  const std::uint64_t DestinationBits = DestinationType->scalarBitWidth();
  const std::uint64_t TargetBits = TargetType.scalarBitWidth();
  if (TargetBits == 0 || DestinationBits % TargetBits != 0 || !std::has_single_bit(TargetBits)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiway shift requires a power-of-two divisor type");
  }
  const std::size_t PartCount = static_cast<std::size_t>(DestinationBits / TargetBits);
  if (auto Constant = getIConstantVRegValWithLookThrough(*Amount, MRI)) {
    return narrowScalarShiftByConstantMultiway(Instruction, Constant->Value, TargetType,
                                               *AmountType);
  }

  std::vector<Register> SourceParts;
  auto Extracted =
      extractParts(*Source, TargetType, static_cast<int>(PartCount), SourceParts, MIRBuilder, MRI);
  if (!Extracted)
    return std::unexpected(Extracted.error());

  auto ZeroAmount = buildConstantRegister(MIRBuilder, *AmountType, 0);
  auto BoolType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!ZeroAmount)
    return std::unexpected(ZeroAmount.error());
  if (!BoolType)
    return std::unexpected(BoolType.error());
  auto IsZero =
      buildICmpRegister(MIRBuilder, IntegerPredicate::Equal, *BoolType, *Amount, *ZeroAmount);
  if (!IsZero)
    return std::unexpected(IsZero.error());

  const auto TargetLog2 = static_cast<std::int64_t>(std::countr_zero(TargetBits));
  auto Log2 = buildConstantRegister(MIRBuilder, *AmountType, TargetLog2);
  auto Mask =
      buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(TargetBits - 1U));
  if (!Log2)
    return std::unexpected(Log2.error());
  if (!Mask)
    return std::unexpected(Mask.error());
  auto WordShift =
      buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *AmountType, {*Amount, *Log2});
  auto BitShift =
      buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *AmountType, {*Amount, *Mask});
  auto Zero = buildConstantRegister(MIRBuilder, TargetType, 0);
  if (!WordShift)
    return std::unexpected(WordShift.error());
  if (!BitShift)
    return std::unexpected(BitShift.error());
  if (!Zero)
    return std::unexpected(Zero.error());

  Register FillValue = *Zero;
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic == GenericOpcode::G_ASHR) {
    auto SignAmount =
        buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(TargetBits - 1U));
    if (!SignAmount)
      return std::unexpected(SignAmount.error());
    auto Sign = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, TargetType,
                                       {SourceParts.back(), *SignAmount});
    if (!Sign)
      return std::unexpected(Sign.error());
    FillValue = *Sign;
  }

  std::vector<Register> DestinationParts(PartCount);
  for (std::size_t PartIndex = 0; PartIndex != PartCount; ++PartIndex) {
    Register InBoundsResult = FillValue;
    for (std::size_t WordIndex = 0; WordIndex != PartCount; ++WordIndex) {
      auto WordConstant =
          buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(WordIndex));
      if (!WordConstant)
        return std::unexpected(WordConstant.error());
      auto IsWord = buildICmpRegister(MIRBuilder, IntegerPredicate::Equal, *BoolType, *WordShift,
                                      *WordConstant);
      if (!IsWord)
        return std::unexpected(IsWord.error());

      std::ptrdiff_t MainIndex;
      std::ptrdiff_t CarryIndex;
      if (Generic == GenericOpcode::G_SHL) {
        MainIndex = static_cast<std::ptrdiff_t>(PartIndex) - static_cast<std::ptrdiff_t>(WordIndex);
        CarryIndex = MainIndex - 1;
      } else if (Generic == GenericOpcode::G_LSHR || Generic == GenericOpcode::G_ASHR) {
        MainIndex = static_cast<std::ptrdiff_t>(PartIndex) + static_cast<std::ptrdiff_t>(WordIndex);
        CarryIndex = MainIndex + 1;
      } else {
        return makeUnexpected(ErrorCode::InternalError,
                              "multiway shift received a non-shift opcode");
      }

      Register Result = FillValue;
      if (MainIndex >= 0 && MainIndex < static_cast<std::ptrdiff_t>(PartCount)) {
        Register Carry;
        if (CarryIndex >= 0 && CarryIndex < static_cast<std::ptrdiff_t>(PartCount))
          Carry = SourceParts[static_cast<std::size_t>(CarryIndex)];
        else if (Generic == GenericOpcode::G_ASHR &&
                 CarryIndex >= static_cast<std::ptrdiff_t>(PartCount))
          Carry = FillValue;
        auto Shifted =
            buildVariableShiftPart(Generic, SourceParts[static_cast<std::size_t>(MainIndex)],
                                   *BitShift, TargetType, Carry);
        if (!Shifted)
          return std::unexpected(Shifted.error());
        Result = *Shifted;
      }

      auto Selected = buildResultInstruction(MIRBuilder, GenericOpcode::G_SELECT, TargetType,
                                             {*IsWord, Result, InBoundsResult});
      if (!Selected)
        return std::unexpected(Selected.error());
      InBoundsResult = *Selected;
    }
    auto Selected = buildResultInstruction(MIRBuilder, GenericOpcode::G_SELECT, TargetType,
                                           {*IsZero, SourceParts[PartIndex], InBoundsResult});
    if (!Selected)
      return std::unexpected(Selected.error());
    DestinationParts[PartIndex] = *Selected;
  }

  if (auto Built = MIRBuilder.buildMergeLikeInstr(*Destination, DestinationParts); !Built)
    return std::unexpected(Built.error());
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalarLoadStore(MachineInstr &Instruction,
                                                      std::size_t TypeIndex,
                                                      LowLevelType NarrowType) {
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic != GenericOpcode::G_LOAD && Generic != GenericOpcode::G_STORE) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "memory scalar narrowing received an unsupported opcode");
  }
  if (TypeIndex != 0 || !NarrowType.isCategoryNeutralScalar() ||
      NarrowType.minimumBitWidth() == 0 || NarrowType.minimumBitWidth() % 8U != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory scalar narrowing requires a byte-sized scalar value type");
  }
  if (Instruction.operandCount() != 2 || Instruction.memOperands().size() != 1 ||
      Instruction.memOperands().front() == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory scalar narrowing requires one address and one memory operand");
  }

  const MachineOperand *ValueOperand = Instruction.operandAt(0);
  const MachineOperand *AddressOperand = Instruction.operandAt(1);
  const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
  const Register *Address = AddressOperand == nullptr ? nullptr : AddressOperand->getIf<Register>();
  if (Value == nullptr || Address == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory scalar narrowing requires register value and address operands");
  }
  auto ValueType = MRI.getType(*Value);
  if (!ValueType)
    return std::unexpected(ValueType.error());
  if (!ValueType->isCategoryNeutralScalar() ||
      NarrowType.minimumBitWidth() >= ValueType->minimumBitWidth()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory scalar narrowing requires a smaller integer scalar type");
  }

  const MachineMemOperand &Memory = *Instruction.memOperands().front();
  const MachineMemOperandDesc &Description = Memory.description();
  if (!Description.Size.isKnown() || Description.Size.isScalable() || !Description.MemoryType) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory scalar narrowing requires a fixed typed memory operand");
  }
  const std::uint64_t MemorySizeInBits = Description.Size.minimumBytes() * 8U;
  if (MemorySizeInBits != ValueType->minimumBitWidth()) {
    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());

    if (Generic == GenericOpcode::G_LOAD) {
      auto Temporary = MRI.createVirtualRegister(NarrowType);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      auto Load = MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
      if (!Load)
        return std::unexpected(Load.error());
      if (auto Added = Load->addDef(*Temporary); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Load->addUse(*Address); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Load->addMemOperand(Memory); !Added)
        return std::unexpected(Added.error());
      if (auto Built = Load->build(); !Built)
        return std::unexpected(Built.error());
      if (auto Extended = MIRBuilder.buildAnyExt(*Value, *Temporary); !Extended)
        return std::unexpected(Extended.error());
    } else {
      auto Temporary = MRI.createVirtualRegister(NarrowType);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      if (auto Truncated = MIRBuilder.buildTrunc(*Temporary, *Value); !Truncated)
        return std::unexpected(Truncated.error());
      auto Store = MIRBuilder.buildInstr(opcode(GenericOpcode::G_STORE));
      if (!Store)
        return std::unexpected(Store.error());
      if (auto Added = Store->addUse(*Temporary); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Store->addUse(*Address); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Store->addMemOperand(Memory); !Added)
        return std::unexpected(Added.error());
      if (auto Built = Store->build(); !Built)
        return std::unexpected(Built.error());
    }

    return Instruction.eraseFromParent();
  }

  return reduceLoadStoreWidth(Instruction, TypeIndex, NarrowType);
}

Expected<void> LegalizerHelper::reduceLoadStoreWidth(MachineInstr &Instruction,
                                                     std::size_t TypeIndex,
                                                     LowLevelType NarrowType) {
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic != GenericOpcode::G_LOAD && Generic != GenericOpcode::G_STORE) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "memory width reduction received an unsupported opcode");
  }
  if (TypeIndex != 0 || !NarrowType.isCategoryNeutralScalar() ||
      NarrowType.minimumBitWidth() == 0 || NarrowType.minimumBitWidth() % 8U != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory width reduction requires a byte-sized scalar target type");
  }
  if (Instruction.operandCount() != 2 || Instruction.memOperands().size() != 1 ||
      Instruction.memOperands().front() == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory width reduction requires one address and one memory operand");
  }

  const bool IsLoad = Generic == GenericOpcode::G_LOAD;
  const MachineOperand *ValueOperand = Instruction.operandAt(0);
  const MachineOperand *AddressOperand = Instruction.operandAt(1);
  const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
  const Register *Address = AddressOperand == nullptr ? nullptr : AddressOperand->getIf<Register>();
  if (Value == nullptr || Address == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "memory width reduction requires register value and address operands");
  }
  auto ValueType = MRI.getType(*Value);
  if (!ValueType)
    return std::unexpected(ValueType.error());
  if (!ValueType->isCategoryNeutralScalar()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory width reduction does not support pointer or vector values");
  }

  const MachineMemOperand &Memory = *Instruction.memOperands().front();
  const MachineMemOperandDesc &Description = Memory.description();
  if (Memory.isAtomic()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory width reduction does not support atomic operations");
  }
  if (!Description.Size.isKnown() || Description.Size.isScalable() || !Description.MemoryType ||
      Description.MemoryType->isVector()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "memory width reduction requires a fixed scalar memory operand");
  }
  const std::uint64_t ValueBits = ValueType->minimumBitWidth();
  if (ValueBits != Description.Size.minimumBytes() * 8U ||
      NarrowType.minimumBitWidth() >= ValueBits) {
    return makeUnexpected(
        ErrorCode::Unsupported,
        "memory width reduction cannot split the requested value and memory widths");
  }

  const std::size_t PartCount = static_cast<std::size_t>(ValueBits / NarrowType.minimumBitWidth());
  const std::uint64_t LeftoverBits = ValueBits - PartCount * NarrowType.minimumBitWidth();
  LowLevelType ExpectedLeftoverType;
  if (LeftoverBits != 0) {
    auto Type = LowLevelType::makeCategoryNeutralScalar(LeftoverBits);
    if (!Type)
      return std::unexpected(Type.error());
    if (LeftoverBits % 8U != 0) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "memory width reduction leaves a non-byte-sized scalar fragment");
    }
    ExpectedLeftoverType = *Type;
  }

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  std::vector<Register> Parts;
  std::vector<Register> Leftovers;
  LowLevelType LeftoverType;
  if (IsLoad) {
    Parts.reserve(PartCount);
    for (std::size_t Index = 0; Index != PartCount; ++Index) {
      auto Part = MRI.createVirtualRegister(NarrowType);
      if (!Part)
        return std::unexpected(Part.error());
      Parts.push_back(*Part);
    }
    LeftoverType = ExpectedLeftoverType;
    if (ExpectedLeftoverType.isValid()) {
      auto Leftover = MRI.createVirtualRegister(ExpectedLeftoverType);
      if (!Leftover)
        return std::unexpected(Leftover.error());
      Leftovers.push_back(*Leftover);
    }
  } else {
    auto Extracted = extractParts(*Value, *ValueType, NarrowType, LeftoverType, Parts, Leftovers,
                                  MIRBuilder, MRI);
    if (!Extracted)
      return std::unexpected(Extracted.error());
    if (!*Extracted || Parts.size() != PartCount || LeftoverType != ExpectedLeftoverType ||
        (ExpectedLeftoverType.isValid() && Leftovers.size() != 1U)) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "memory width reduction cannot split the store value consistently");
    }
  }

  const auto SplitPieces = [&](LowLevelType PartType, const std::vector<Register> &Values,
                               std::uint64_t &Offset) -> Expected<void> {
    const std::uint64_t PartBytes = PartType.minimumBitWidth() / 8U;
    for (Register Part : Values) {
      auto AddressPart = buildObjectPointerOffset(MIRBuilder, *Address, Offset);
      if (!AddressPart)
        return std::unexpected(AddressPart.error());
      auto MemoryPart = deriveMemoryOperand(Function, Memory, PartType, Offset);
      if (!MemoryPart)
        return std::unexpected(MemoryPart.error());

      auto Access =
          MIRBuilder.buildInstr(opcode(IsLoad ? GenericOpcode::G_LOAD : GenericOpcode::G_STORE));
      if (!Access)
        return std::unexpected(Access.error());
      if (IsLoad) {
        if (auto Added = Access->addDef(Part); !Added)
          return std::unexpected(Added.error());
      } else if (auto Added = Access->addUse(Part); !Added) {
        return std::unexpected(Added.error());
      }
      if (auto Added = Access->addUse(*AddressPart); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Access->addMemOperand(**MemoryPart); !Added)
        return std::unexpected(Added.error());
      if (auto Built = Access->build(); !Built)
        return std::unexpected(Built.error());
      Offset += PartBytes;
    }
    return {};
  };

  std::uint64_t Offset = 0;
  if (auto Split = SplitPieces(NarrowType, Parts, Offset); !Split)
    return std::unexpected(Split.error());
  if (LeftoverType.isValid()) {
    if (auto Split = SplitPieces(LeftoverType, Leftovers, Offset); !Split)
      return std::unexpected(Split.error());
  }

  if (IsLoad) {
    if (auto Inserted = insertParts(*Value, *ValueType, NarrowType, Parts, LeftoverType, Leftovers);
        !Inserted) {
      return std::unexpected(Inserted.error());
    }
  }
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::narrowScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                             LowLevelType NarrowType) {
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_IMPLICIT_DEF: {
    const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_IMPLICIT_DEF narrowing requires a register destination");
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());

    const std::uint64_t DestinationSize = DestinationType->minimumBitWidth();
    const std::uint64_t NarrowSize = NarrowType.minimumBitWidth();
    if (NarrowSize == 0 || NarrowSize >= DestinationSize)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_IMPLICIT_DEF narrowing requires a smaller nonzero type");

    if (DestinationSize % NarrowSize != 0) {
      LowLevelType ImplicitType = NarrowType;
      if (DestinationType->isVector()) {
        auto VectorType =
            DestinationType->isScalableVector()
                ? LowLevelType::makeScalableVector(DestinationType->minimumElementCount(),
                                                   NarrowType)
                : LowLevelType::makeFixedVector(DestinationType->minimumElementCount(), NarrowType);
        if (!VectorType)
          return std::unexpected(VectorType.error());
        ImplicitType = *VectorType;
      }
      auto Implicit = MIRBuilder.buildUndef(ImplicitType);
      if (!Implicit)
        return std::unexpected(Implicit.error());
      auto ImplicitReg = resultRegister(**Implicit);
      if (!ImplicitReg)
        return std::unexpected(ImplicitReg.error());
      if (auto Extended = MIRBuilder.buildAnyExt(*Destination, *ImplicitReg); !Extended)
        return std::unexpected(Extended.error());
    } else {
      const std::size_t PartCount = static_cast<std::size_t>(DestinationSize / NarrowSize);
      std::vector<Register> Parts;
      Parts.reserve(PartCount);
      for (std::size_t Index = 0; Index != PartCount; ++Index) {
        auto Part = MIRBuilder.buildUndef(NarrowType);
        if (!Part)
          return std::unexpected(Part.error());
        auto PartReg = resultRegister(**Part);
        if (!PartReg)
          return std::unexpected(PartReg.error());
        Parts.push_back(*PartReg);
      }
      if (auto Merged = MIRBuilder.buildMergeLikeInstr(*Destination, Parts); !Merged)
        return std::unexpected(Merged.error());
    }

    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_CONSTANT: {
    const Register *Destination = Instruction.operandAt(0)->getIf<Register>();
    const auto *Immediate = Instruction.operandAt(1)->getIf<IntegerImmediateOperand>();
    if (Destination == nullptr || Immediate == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_CONSTANT narrowing requires register and integer operands");
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());

    const std::uint64_t TotalSize = DestinationType->minimumBitWidth();
    const std::uint64_t NarrowSize = NarrowType.minimumBitWidth();
    if (NarrowSize == 0 || NarrowSize >= TotalSize || NarrowSize > APInt::MaxBitWidth ||
        TotalSize > APInt::MaxBitWidth) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_CONSTANT narrowing requires a smaller representable type");
    }

    const std::size_t PartCount = static_cast<std::size_t>(TotalSize / NarrowSize);
    std::vector<Register> Parts;
    Parts.reserve(PartCount);
    for (std::size_t Index = 0; Index != PartCount; ++Index) {
      auto Shifted = Immediate->Value.logicalShiftRight(Index * NarrowSize);
      if (!Shifted)
        return std::unexpected(Shifted.error());
      APInt PartValue = std::move(*Shifted);
      if (PartValue.bitWidth() > NarrowSize) {
        auto Truncated = PartValue.trunc(static_cast<std::uint32_t>(NarrowSize));
        if (!Truncated)
          return std::unexpected(Truncated.error());
        PartValue = std::move(*Truncated);
      }
      auto Part = buildConstantRegister(MIRBuilder, NarrowType, std::move(PartValue));
      if (!Part)
        return std::unexpected(Part.error());
      Parts.push_back(*Part);
    }

    LowLevelType LeftoverType;
    std::vector<Register> Leftovers;
    const std::uint64_t LeftoverBits = TotalSize - PartCount * NarrowSize;
    if (LeftoverBits != 0) {
      auto Type = LowLevelType::makeCategoryNeutralScalar(LeftoverBits);
      if (!Type)
        return std::unexpected(Type.error());
      LeftoverType = *Type;
      auto Shifted = Immediate->Value.logicalShiftRight(PartCount * NarrowSize);
      if (!Shifted)
        return std::unexpected(Shifted.error());
      APInt LeftoverValue = std::move(*Shifted);
      if (LeftoverValue.bitWidth() > LeftoverBits) {
        auto Truncated = LeftoverValue.trunc(static_cast<std::uint32_t>(LeftoverBits));
        if (!Truncated)
          return std::unexpected(Truncated.error());
        LeftoverValue = std::move(*Truncated);
      }
      auto Leftover = buildConstantRegister(MIRBuilder, LeftoverType, std::move(LeftoverValue));
      if (!Leftover)
        return std::unexpected(Leftover.error());
      Leftovers.push_back(*Leftover);
    }

    if (auto Inserted =
            insertParts(*Destination, *DestinationType, NarrowType, Parts, LeftoverType, Leftovers);
        !Inserted) {
      return std::unexpected(Inserted.error());
    }
    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_ADD:
  case GenericOpcode::G_SUB:
  case GenericOpcode::G_SADDO:
  case GenericOpcode::G_SSUBO:
  case GenericOpcode::G_SADDE:
  case GenericOpcode::G_SSUBE:
  case GenericOpcode::G_UADDO:
  case GenericOpcode::G_USUBO:
  case GenericOpcode::G_UADDE:
  case GenericOpcode::G_USUBE:
    return narrowScalarAddSub(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_MUL:
  case GenericOpcode::G_UMULH:
    return narrowScalarMul(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_LOAD:
  case GenericOpcode::G_STORE:
    return narrowScalarLoadStore(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_SEXT:
  case GenericOpcode::G_ZEXT:
  case GenericOpcode::G_ANYEXT:
    return narrowScalarExt(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_INTTOPTR:
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported,
                            "G_INTTOPTR destination narrowing is not defined");
    Observer.changingInstr(Instruction);
    if (auto Narrowed = narrowScalarSrc(Instruction, NarrowType, 1); !Narrowed)
      return std::unexpected(Narrowed.error());
    Observer.changedInstr(Instruction);
    return {};
  case GenericOpcode::G_PTRTOINT:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::Unsupported, "G_PTRTOINT source narrowing is not defined");
    Observer.changingInstr(Instruction);
    if (auto Narrowed = narrowScalarDst(Instruction, NarrowType, 0, GenericOpcode::G_ZEXT);
        !Narrowed) {
      return std::unexpected(Narrowed.error());
    }
    Observer.changedInstr(Instruction);
    return {};
  case GenericOpcode::G_PHI: {
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::Unsupported, "G_PHI narrowing requires type index zero");
    if (Instruction.operandCount() < 3 || Instruction.operandCount() % 2U == 0)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_PHI narrowing requires incoming value/block pairs");
    MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_PHI narrowing requires a register destination");
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());
    const std::uint64_t DestinationSize = DestinationType->minimumBitWidth();
    const std::uint64_t NarrowSize = NarrowType.minimumBitWidth();
    if (NarrowSize == 0 || NarrowSize >= DestinationSize || DestinationSize % NarrowSize != 0)
      return makeUnexpected(ErrorCode::Unsupported,
                            "G_PHI narrowing requires an exact scalar split");

    const std::size_t PartCount = static_cast<std::size_t>(DestinationSize / NarrowSize);
    const std::size_t IncomingCount = (Instruction.operandCount() - 1U) / 2U;
    std::vector<std::vector<Register>> IncomingParts(IncomingCount);
    std::vector<MachineBasicBlock *> IncomingBlocks(IncomingCount);
    Observer.changingInstr(Instruction);
    for (std::size_t Index = 1; Index < Instruction.operandCount(); Index += 2) {
      const MachineOperand *ValueOperand = Instruction.operandAt(Index);
      const MachineOperand *BlockOperand = Instruction.operandAt(Index + 1U);
      const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
      const auto *Incoming =
          BlockOperand == nullptr ? nullptr : BlockOperand->getIf<BasicBlockOperand>();
      if (Value == nullptr || Incoming == nullptr || Incoming->Block == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "G_PHI narrowing has an invalid incoming pair");
      if (auto Point = setInsertPointAtFirstTerminator(MIRBuilder, *Incoming->Block); !Point)
        return std::unexpected(Point.error());
      const std::size_t IncomingIndex = Index / 2U;
      IncomingBlocks[IncomingIndex] = Incoming->Block;
      auto &Parts = IncomingParts[IncomingIndex];
      Parts.reserve(PartCount);
      for (std::size_t PartIndex = 0; PartIndex != PartCount; ++PartIndex) {
        auto Part = MRI.createVirtualRegister(NarrowType);
        if (!Part)
          return std::unexpected(Part.error());
        Parts.push_back(*Part);
      }
      if (auto Unmerged = MIRBuilder.buildUnmergeValues(Parts, *Value); !Unmerged)
        return std::unexpected(Unmerged.error());
    }

    MachineBasicBlock &Block = *Instruction.parent();
    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    std::vector<Register> Destinations;
    Destinations.reserve(PartCount);
    for (std::size_t PartIndex = 0; PartIndex != PartCount; ++PartIndex) {
      auto DestinationPart = MRI.createVirtualRegister(NarrowType);
      if (!DestinationPart)
        return std::unexpected(DestinationPart.error());
      auto Phi = MIRBuilder.buildInstr(opcode(GenericOpcode::G_PHI));
      if (!Phi)
        return std::unexpected(Phi.error());
      if (auto Added = Phi->addDef(*DestinationPart); !Added)
        return std::unexpected(Added.error());
      for (std::size_t IncomingIndex = 0; IncomingIndex != IncomingCount; ++IncomingIndex) {
        if (auto Added = Phi->addUse(IncomingParts[IncomingIndex][PartIndex]); !Added)
          return std::unexpected(Added.error());
        if (auto Added = Phi->addBasicBlock(*IncomingBlocks[IncomingIndex]); !Added)
          return std::unexpected(Added.error());
      }
      if (auto Built = Phi->build(); !Built)
        return std::unexpected(Built.error());
      Destinations.push_back(*DestinationPart);
    }

    if (auto Point = setInsertPointAfterPHIs(MIRBuilder, Block); !Point)
      return std::unexpected(Point.error());
    if (auto Merged = MIRBuilder.buildMergeLikeInstr(*Destination, Destinations); !Merged)
      return std::unexpected(Merged.error());
    Observer.changedInstr(Instruction);
    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_FCMP:
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::Unsupported,
                            "floating comparison source narrowing is not defined");
    Observer.changingInstr(Instruction);
    if (auto Narrowed = narrowScalarDst(Instruction, NarrowType, 0, GenericOpcode::G_ZEXT);
        !Narrowed) {
      return std::unexpected(Narrowed.error());
    }
    Observer.changedInstr(Instruction);
    return {};
  case GenericOpcode::G_SEXT_INREG: {
    if (TypeIndex != 0)
      return makeUnexpected(ErrorCode::Unsupported,
                            "G_SEXT_INREG narrowing requires type index zero");

    MachineOperand *DestinationOperand = Instruction.operandAt(0);
    MachineOperand *SourceOperand = Instruction.operandAt(1);
    MachineOperand *WidthOperand = Instruction.operandAt(2);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
    if (Destination == nullptr || Source == nullptr || WidthOperand == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_SEXT_INREG narrowing requires register and width operands");
    auto SizeInBits = signedImmediate(*WidthOperand);
    if (!SizeInBits)
      return std::unexpected(SizeInBits.error());
    auto DestinationType = MRI.getType(*Destination);
    if (!DestinationType)
      return std::unexpected(DestinationType.error());

    const std::uint64_t TotalSize = DestinationType->minimumBitWidth();
    const std::uint64_t NarrowSize = NarrowType.minimumBitWidth();
    if (*SizeInBits <= 0 || static_cast<std::uint64_t>(*SizeInBits) > TotalSize ||
        NarrowSize == 0 || NarrowSize >= TotalSize) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_SEXT_INREG narrowing has invalid scalar widths");
    }

    if (NarrowSize > static_cast<std::uint64_t>(*SizeInBits)) {
      const Register OriginalDestination = *Destination;
      Observer.changingInstr(Instruction);
      auto NarrowedSource = narrowScalarSrc(Instruction, NarrowType, 1);
      if (!NarrowedSource)
        return std::unexpected(NarrowedSource.error());
      auto NarrowDestination = MRI.createVirtualRegister(NarrowType);
      if (!NarrowDestination)
        return std::unexpected(NarrowDestination.error());
      if (auto Changed = DestinationOperand->setRegister(*NarrowDestination); !Changed)
        return std::unexpected(Changed.error());
      if (auto Point = MIRBuilder.setInsertPointAfter(Instruction); !Point)
        return std::unexpected(Point.error());
      if (auto Extended = MIRBuilder.buildSExt(OriginalDestination, *NarrowDestination); !Extended)
        return std::unexpected(Extended.error());
      Observer.changedInstr(Instruction);
      return {};
    }

    if (TotalSize % NarrowSize != 0)
      return makeUnexpected(ErrorCode::Unsupported,
                            "G_SEXT_INREG cannot be split evenly to the requested type");
    const std::size_t PartCount = static_cast<std::size_t>(TotalSize / NarrowSize);
    std::vector<Register> SourceParts;
    SourceParts.reserve(PartCount);
    for (std::size_t Index = 0; Index != PartCount; ++Index) {
      auto Part = MRI.createVirtualRegister(NarrowType);
      if (!Part)
        return std::unexpected(Part.error());
      SourceParts.push_back(*Part);
    }
    if (auto Unmerged = MIRBuilder.buildUnmergeValues(SourceParts, *Source); !Unmerged)
      return std::unexpected(Unmerged.error());

    auto ShiftAmount =
        buildConstantRegister(MIRBuilder, NarrowType, static_cast<std::int64_t>(NarrowSize - 1U));
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());

    std::vector<Register> DestinationParts;
    DestinationParts.reserve(PartCount);
    Register FullExtension;
    Register PartialExtension;
    const std::uint64_t ExtensionBits = static_cast<std::uint64_t>(*SizeInBits);
    for (std::size_t Index = 0; Index != PartCount; ++Index) {
      if ((Index + 1U) * NarrowSize <= ExtensionBits) {
        DestinationParts.push_back(SourceParts[Index]);
        PartialExtension = SourceParts[Index];
        continue;
      }
      if (Index * NarrowSize >= ExtensionBits) {
        if (PartialExtension.isNoRegister())
          return makeUnexpected(ErrorCode::InternalError,
                                "G_SEXT_INREG split has no sign-bearing part");
        if (FullExtension.isNoRegister()) {
          auto Extended = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, NarrowType,
                                                 {PartialExtension, *ShiftAmount});
          if (!Extended)
            return std::unexpected(Extended.error());
          FullExtension = *Extended;
        }
        DestinationParts.push_back(FullExtension);
        continue;
      }

      auto Extended = MIRBuilder.buildSExtInReg(
          NarrowType, SourceParts[Index], static_cast<std::uint32_t>(ExtensionBits % NarrowSize));
      if (!Extended)
        return std::unexpected(Extended.error());
      auto ExtendedRegister = resultRegister(**Extended);
      if (!ExtendedRegister)
        return std::unexpected(ExtendedRegister.error());
      DestinationParts.push_back(*ExtendedRegister);
      PartialExtension = *ExtendedRegister;
    }

    if (auto Merged = MIRBuilder.buildMergeLikeInstr(*Destination, DestinationParts); !Merged)
      return std::unexpected(Merged.error());
    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_AND:
  case GenericOpcode::G_OR:
  case GenericOpcode::G_XOR:
    return narrowScalarBasic(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_SELECT:
    return narrowScalarSelect(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_SHL:
  case GenericOpcode::G_LSHR:
  case GenericOpcode::G_ASHR:
    return narrowScalarShift(Instruction, TypeIndex, NarrowType);
  case GenericOpcode::G_ICMP: {
    if (TypeIndex != 1)
      return makeUnexpected(ErrorCode::Unsupported,
                            "G_ICMP narrowing requires the source type index");
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *PredicateMachineOperand = Instruction.operandAt(1);
    const MachineOperand *LeftOperand = Instruction.operandAt(2);
    const MachineOperand *RightOperand = Instruction.operandAt(3);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const auto *PredicateValue = PredicateMachineOperand == nullptr
                                     ? nullptr
                                     : PredicateMachineOperand->getIf<PredicateOperand>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    const IntegerPredicate *Predicate =
        PredicateValue == nullptr ? nullptr : PredicateValue->integerPredicate();
    if (Destination == nullptr || Left == nullptr || Right == nullptr || Predicate == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "G_ICMP narrowing requires an integer predicate and register operands");
    }
    auto SourceType = MRI.getType(*Left);
    auto ResultType = MRI.getType(*Destination);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    if (!ResultType)
      return std::unexpected(ResultType.error());

    LowLevelType LeftoverType;
    LowLevelType UnusedType;
    std::vector<Register> LeftParts;
    std::vector<Register> LeftLeftovers;
    std::vector<Register> RightParts;
    std::vector<Register> RightLeftovers;
    auto LeftExtracted = extractParts(*Left, *SourceType, NarrowType, LeftoverType, LeftParts,
                                      LeftLeftovers, MIRBuilder, MRI);
    if (!LeftExtracted)
      return std::unexpected(LeftExtracted.error());
    auto RightExtracted = extractParts(*Right, *SourceType, NarrowType, UnusedType, RightParts,
                                       RightLeftovers, MIRBuilder, MRI);
    if (!RightExtracted)
      return std::unexpected(RightExtracted.error());
    if (!*LeftExtracted || !*RightExtracted || LeftParts.size() != RightParts.size() ||
        LeftLeftovers.size() != RightLeftovers.size()) {
      return makeUnexpected(ErrorCode::Unsupported, "G_ICMP operands cannot be split consistently");
    }

    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    if (isEqualityPredicate(*Predicate)) {
      auto Zero = buildConstantRegister(MIRBuilder, NarrowType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());

      std::vector<Register> Xors;
      Xors.reserve(LeftParts.size() + LeftLeftovers.size());
      for (std::size_t Index = 0; Index != LeftParts.size(); ++Index) {
        auto Xor = buildResultInstruction(MIRBuilder, GenericOpcode::G_XOR, NarrowType,
                                          {LeftParts[Index], RightParts[Index]});
        if (!Xor)
          return std::unexpected(Xor.error());
        Xors.push_back(*Xor);
      }

      std::vector<Register> WidenedXors;
      for (std::size_t Index = 0; Index != LeftLeftovers.size(); ++Index) {
        auto Xor = buildResultInstruction(MIRBuilder, GenericOpcode::G_XOR, LeftoverType,
                                          {LeftLeftovers[Index], RightLeftovers[Index]});
        if (!Xor)
          return std::unexpected(Xor.error());
        auto GCDType = extractGCDType(WidenedXors, NarrowType, LeftoverType, *Xor);
        if (!GCDType)
          return std::unexpected(GCDType.error());
        auto LCMType = buildLCMMergePieces(LeftoverType, NarrowType, *GCDType, WidenedXors,
                                           GenericOpcode::G_ZEXT);
        if (!LCMType)
          return std::unexpected(LCMType.error());
        Xors.insert(Xors.end(), WidenedXors.begin(), WidenedXors.end());
      }

      if (Xors.size() < 2) {
        return makeUnexpected(ErrorCode::InternalError,
                              "G_ICMP narrowing produced fewer than two comparison parts");
      }
      auto Combined =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, NarrowType, {Xors[0], Xors[1]});
      if (!Combined)
        return std::unexpected(Combined.error());
      for (std::size_t Index = 2; Index != Xors.size(); ++Index) {
        auto Next = buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, NarrowType,
                                           {*Combined, Xors[Index]});
        if (!Next)
          return std::unexpected(Next.error());
        Combined = std::move(Next);
      }
      if (auto Built = MIRBuilder.buildICmp(*Predicate, *Destination, *Combined, *Zero); !Built)
        return std::unexpected(Built.error());
    } else {
      Register ComparisonInput;
      const auto buildComparisonPart = [&](Register LeftPart, Register RightPart,
                                           bool IsFinal) -> Expected<Register> {
        IntegerPredicate PartPredicate = *Predicate;
        if (!IsFinal) {
          auto Unsigned = unsignedPredicate(*Predicate);
          if (!Unsigned)
            return std::unexpected(Unsigned.error());
          PartPredicate = *Unsigned;
        }
        Register ComparisonOutput = *Destination;
        if (!IsFinal) {
          auto Temporary = MRI.createVirtualRegister(*ResultType);
          if (!Temporary)
            return std::unexpected(Temporary.error());
          ComparisonOutput = *Temporary;
        }
        if (ComparisonInput.isNoRegister()) {
          auto Built = MIRBuilder.buildICmp(PartPredicate, ComparisonOutput, LeftPart, RightPart);
          if (!Built)
            return std::unexpected(Built.error());
        } else {
          auto Comparison = MRI.createVirtualRegister(*ResultType);
          auto Equal = MRI.createVirtualRegister(*ResultType);
          if (!Comparison)
            return std::unexpected(Comparison.error());
          if (!Equal)
            return std::unexpected(Equal.error());
          if (auto Built = MIRBuilder.buildICmp(PartPredicate, *Comparison, LeftPart, RightPart);
              !Built) {
            return std::unexpected(Built.error());
          }
          if (auto Built =
                  MIRBuilder.buildICmp(IntegerPredicate::Equal, *Equal, LeftPart, RightPart);
              !Built) {
            return std::unexpected(Built.error());
          }
          if (auto Built =
                  MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {ComparisonOutput},
                                        {*Equal, ComparisonInput, *Comparison});
              !Built) {
            return std::unexpected(Built.error());
          }
        }
        return ComparisonOutput;
      };

      for (std::size_t Index = 0; Index != LeftParts.size(); ++Index) {
        const bool IsFinal = Index + 1U == LeftParts.size() && LeftLeftovers.empty();
        auto Comparison = buildComparisonPart(LeftParts[Index], RightParts[Index], IsFinal);
        if (!Comparison)
          return std::unexpected(Comparison.error());
        ComparisonInput = *Comparison;
      }
      for (std::size_t Index = 0; Index != LeftLeftovers.size(); ++Index) {
        const bool IsFinal = Index + 1U == LeftLeftovers.size();
        auto Comparison = buildComparisonPart(LeftLeftovers[Index], RightLeftovers[Index], IsFinal);
        if (!Comparison)
          return std::unexpected(Comparison.error());
        ComparisonInput = *Comparison;
      }
    }

    return Instruction.eraseFromParent();
  }
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "scalar narrowing is not implemented for this opcode");
  }
}

Expected<void> LegalizerHelper::fewerElements(MachineInstr &Instruction, std::size_t TypeIndex,
                                              LowLevelType NarrowType) {
  auto Query = LegalizerInfo::buildQuery(Instruction, Function, MIRBuilder.target());
  if (!Query)
    return std::unexpected(Query.error());
  if (TypeIndex >= Query->Types.size() || TypeIndex >= Query->TypeOperands.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "vector legalization has an invalid type index");
  }

  const LowLevelType VectorType = Query->Types[TypeIndex];
  const std::optional<LowLevelType> ElementType = VectorType.scalarElementType();
  if (!VectorType.isVector() || VectorType.isScalableVector() || !ElementType ||
      *ElementType != NarrowType || !NarrowType.isCategoryNeutralScalar()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "vector legalization requires fixed vectors split into scalar elements");
  }

  std::size_t ExplicitOperandCount = 0;
  while (ExplicitOperandCount < Instruction.operandCount() &&
         !Instruction.operandAt(ExplicitOperandCount)->isImplicit()) {
    ++ExplicitOperandCount;
  }
  const std::vector<std::size_t> &TypedOperands = Query->TypeOperands[TypeIndex];
  if (TypedOperands.size() != ExplicitOperandCount || TypedOperands.empty()) {
    return makeUnexpected(
        ErrorCode::Unsupported,
        "vector legalization requires all explicit operands to share one vector type");
  }

  std::vector<std::size_t> DefinitionIndices;
  std::vector<std::size_t> SourceIndices;
  DefinitionIndices.reserve(TypedOperands.size());
  SourceIndices.reserve(TypedOperands.size());
  for (const std::size_t OperandIndex : TypedOperands) {
    const MachineOperand *Operand = Instruction.operandAt(OperandIndex);
    const Register *RegisterOperand = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (RegisterOperand == nullptr || !RegisterOperand->isVirtual()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "vector legalization requires virtual-register operands");
    }
    auto OperandType = MRI.getType(*RegisterOperand);
    if (!OperandType)
      return std::unexpected(OperandType.error());
    if (*OperandType != VectorType) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          "vector legalization type operands do not match the requested vector type");
    }
    if (Operand->isDef() && !Operand->isUse()) {
      DefinitionIndices.push_back(OperandIndex);
    } else if (Operand->isUse() && !Operand->isDef()) {
      SourceIndices.push_back(OperandIndex);
    } else {
      return makeUnexpected(ErrorCode::Unsupported,
                            "vector legalization does not support tied or non-register operands");
    }
  }
  if (DefinitionIndices.empty() || SourceIndices.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "vector legalization requires register definitions and sources");
  }

  const std::size_t ElementCount = VectorType.minimumElementCount();
  std::vector<std::vector<Register>> SourceParts;
  SourceParts.reserve(SourceIndices.size());
  for (const std::size_t SourceIndex : SourceIndices) {
    const Register Source = *Instruction.operandAt(SourceIndex)->getIf<Register>();
    std::vector<Register> Parts;
    auto Extracted =
        extractParts(Source, NarrowType, static_cast<int>(ElementCount), Parts, MIRBuilder, MRI);
    if (!Extracted)
      return std::unexpected(Extracted.error());
    SourceParts.push_back(std::move(Parts));
  }

  std::vector<std::vector<Register>> DestinationParts(DefinitionIndices.size());
  for (std::vector<Register> &Parts : DestinationParts)
    Parts.reserve(ElementCount);
  for (std::size_t ElementIndex = 0; ElementIndex < ElementCount; ++ElementIndex) {
    auto ScalarInstruction = MIRBuilder.buildInstr(Instruction.opcode());
    if (!ScalarInstruction)
      return std::unexpected(ScalarInstruction.error());
    if (auto SetFlags = ScalarInstruction->setFlags(Instruction.flags()); !SetFlags)
      return std::unexpected(SetFlags.error());
    for (std::size_t DefinitionNumber = 0; DefinitionNumber < DefinitionIndices.size();
         ++DefinitionNumber) {
      auto Destination = MRI.createVirtualRegister(NarrowType);
      if (!Destination)
        return std::unexpected(Destination.error());
      if (auto Added = ScalarInstruction->addDef(*Destination); !Added)
        return std::unexpected(Added.error());
      DestinationParts[DefinitionNumber].push_back(*Destination);
    }
    for (const std::vector<Register> &Parts : SourceParts) {
      if (auto Added = ScalarInstruction->addUse(Parts[ElementIndex]); !Added)
        return std::unexpected(Added.error());
    }
    if (auto Built = ScalarInstruction->build(); !Built)
      return std::unexpected(Built.error());
  }

  for (std::size_t DefinitionNumber = 0; DefinitionNumber < DefinitionIndices.size();
       ++DefinitionNumber) {
    const Register Destination =
        *Instruction.operandAt(DefinitionIndices[DefinitionNumber])->getIf<Register>();
    if (auto Merged =
            MIRBuilder.buildMergeLikeInstr(Destination, DestinationParts[DefinitionNumber]);
        !Merged) {
      return std::unexpected(Merged.error());
    }
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerVAArg(MachineInstr &Instruction) {
  if (Instruction.opcode() != opcode(GenericOpcode::G_VAARG) || Instruction.operandCount() != 3 ||
      !Instruction.memOperands().empty()) {
    return makeUnexpected(ErrorCode::VerificationError, "G_VAARG has invalid operands");
  }

  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  MachineOperand *ListOperand = Instruction.operandAt(1);
  MachineOperand *AlignmentOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *ListPointer = ListOperand == nullptr ? nullptr : ListOperand->getIf<Register>();
  if (Destination == nullptr || ListPointer == nullptr || !Destination->isVirtual() ||
      !ListPointer->isVirtual() || !DestinationOperand->isDef() || !ListOperand->isUse() ||
      AlignmentOperand == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_VAARG requires virtual-register operands");
  }

  auto DestinationInfo = MRI.virtualRegisterInfo(*Destination);
  auto PointerInfo = MRI.virtualRegisterInfo(*ListPointer);
  auto RequestedAlignment = signedImmediate(*AlignmentOperand);
  if (!DestinationInfo || !PointerInfo)
    return makeUnexpected(ErrorCode::VerificationError, "G_VAARG has invalid register information");
  const std::optional<LowLevelType> &DestinationTypeInfo = DestinationInfo->get().Type;
  const std::optional<LowLevelType> &PointerTypeInfo = PointerInfo->get().Type;
  if (!DestinationTypeInfo.has_value() || !PointerTypeInfo.has_value() || !RequestedAlignment ||
      *RequestedAlignment <= 0 ||
      !std::has_single_bit(static_cast<std::uint64_t>(*RequestedAlignment))) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_VAARG has invalid type or alignment information");
  }

  const LowLevelType DestinationType = DestinationTypeInfo.value();
  const LowLevelType PointerType = PointerTypeInfo.value();
  const auto DestinationLayout = scalarLayout(DestinationType);
  const auto PointerLayout = scalarLayout(PointerType);
  if (!DestinationLayout || !PointerLayout || !PointerType.isPointer()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "G_VAARG requires scalar result and pointer list types");
  }

  auto ListLoadMemory =
      createMemoryAccess(Function, MemoryAccessFlag::Load, PointerType, *PointerLayout);
  auto ListStoreMemory =
      createMemoryAccess(Function, MemoryAccessFlag::Store, PointerType, *PointerLayout);
  if (!ListLoadMemory)
    return std::unexpected(ListLoadMemory.error());
  if (!ListStoreMemory)
    return std::unexpected(ListStoreMemory.error());

  auto CurrentList = MRI.createVirtualRegister(PointerType);
  if (!CurrentList)
    return std::unexpected(CurrentList.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto ListLoad = MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
  if (!ListLoad)
    return std::unexpected(ListLoad.error());
  if (auto Added = ListLoad->addDef(*CurrentList); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListLoad->addUse(*ListPointer); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListLoad->addMemOperand(**ListLoadMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = ListLoad->build(); !Built)
    return std::unexpected(Built.error());

  Register ValueAddress = *CurrentList;
  const std::uint64_t AlignmentValue = static_cast<std::uint64_t>(*RequestedAlignment);
  auto PointerScalarType = LowLevelType::makeCategoryNeutralScalar(PointerType.scalarBitWidth());
  if (!PointerScalarType)
    return std::unexpected(PointerScalarType.error());
  if (AlignmentValue > 1) {
    auto AlignmentOffset =
        buildConstantBefore(MIRBuilder, Instruction, *PointerScalarType, AlignmentValue - 1U);
    if (!AlignmentOffset)
      return std::unexpected(AlignmentOffset.error());
    auto BiasedAddress = buildBinaryBefore(MIRBuilder, Instruction, GenericOpcode::G_PTR_ADD,
                                           PointerType, ValueAddress, *AlignmentOffset);
    if (!BiasedAddress)
      return std::unexpected(BiasedAddress.error());
    auto AlignmentMask =
        buildConstantBefore(MIRBuilder, Instruction, *PointerScalarType, ~(AlignmentValue - 1U));
    if (!AlignmentMask)
      return std::unexpected(AlignmentMask.error());
    auto AlignedAddress = buildBinaryBefore(MIRBuilder, Instruction, GenericOpcode::G_PTRMASK,
                                            PointerType, *BiasedAddress, *AlignmentMask);
    if (!AlignedAddress)
      return std::unexpected(AlignedAddress.error());
    ValueAddress = *AlignedAddress;
  }

  auto Increment = buildConstantBefore(MIRBuilder, Instruction, *PointerScalarType,
                                       DestinationLayout->AllocationSize);
  if (!Increment)
    return std::unexpected(Increment.error());
  auto NextList = buildBinaryBefore(MIRBuilder, Instruction, GenericOpcode::G_PTR_ADD, PointerType,
                                    ValueAddress, *Increment);
  if (!NextList)
    return std::unexpected(NextList.error());

  auto ListStore = MIRBuilder.buildInstr(opcode(GenericOpcode::G_STORE));
  if (!ListStore)
    return std::unexpected(ListStore.error());
  if (auto Added = ListStore->addUse(*NextList); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListStore->addUse(*ListPointer); !Added)
    return std::unexpected(Added.error());
  if (auto Added = ListStore->addMemOperand(**ListStoreMemory); !Added)
    return std::unexpected(Added.error());
  if (auto Built = ListStore->build(); !Built)
    return std::unexpected(Built.error());

  if (!MRI.useEmpty(*Destination)) {
    auto ValueLoadMemory =
        createMemoryAccess(Function, MemoryAccessFlag::Load, DestinationType, *DestinationLayout);
    if (!ValueLoadMemory)
      return std::unexpected(ValueLoadMemory.error());
    auto ValueLoad = MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
    if (!ValueLoad)
      return std::unexpected(ValueLoad.error());
    if (auto Added = ValueLoad->addDef(*Destination); !Added)
      return std::unexpected(Added.error());
    if (auto Added = ValueLoad->addUse(ValueAddress); !Added)
      return std::unexpected(Added.error());
    if (auto Added = ValueLoad->addMemOperand(**ValueLoadMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = ValueLoad->build(); !Built)
      return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerLoad(MachineInstr &Instruction) {
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic != GenericOpcode::G_LOAD && Generic != GenericOpcode::G_SEXTLOAD &&
      Generic != GenericOpcode::G_ZEXTLOAD) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "load lowering received an unsupported opcode");
  }
  if (Instruction.operandCount() != 2 || Instruction.memOperands().size() != 1 ||
      Instruction.memOperands().front() == nullptr) {
    return makeUnexpected(
        ErrorCode::VerificationError,
        "load lowering requires one destination, one address, and one memory operand");
  }

  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *AddressOperand = Instruction.operandAt(1);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Address = AddressOperand == nullptr ? nullptr : AddressOperand->getIf<Register>();
  if (Destination == nullptr || Address == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "load lowering requires register destination and address operands");
  }
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!DestinationType->isScalarValue()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "load lowering does not support vector result types");
  }

  const MachineMemOperand &Memory = *Instruction.memOperands().front();
  const MachineMemOperandDesc &Description = Memory.description();
  if (Memory.isAtomic())
    return makeUnexpected(ErrorCode::Unsupported,
                          "load lowering does not support atomic operations");
  if (!Description.Size.isKnown() || Description.Size.isScalable() || !Description.MemoryType ||
      Description.MemoryType->isVector()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "load lowering requires a fixed scalar memory operand");
  }
  const LowLevelType MemoryType = *Description.MemoryType;
  const std::uint64_t MemoryBits = MemoryType.minimumBitWidth();
  const std::uint64_t MemoryStoreBits = Description.Size.minimumBytes() * 8U;
  if (MemoryBits == 0 || MemoryBits > MemoryStoreBits) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "load lowering has an inconsistent memory type and size");
  }

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  const auto BuildLoad = [&](GenericOpcode LoadOpcode, Register Result, Register Pointer,
                             const MachineMemOperand &LoadMemory) -> Expected<void> {
    auto Load = MIRBuilder.buildInstr(opcode(LoadOpcode));
    if (!Load)
      return std::unexpected(Load.error());
    if (auto Added = Load->addDef(Result); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Load->addUse(Pointer); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Load->addMemOperand(LoadMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = Load->build(); !Built)
      return std::unexpected(Built.error());
    return {};
  };

  if (MemoryBits != MemoryStoreBits) {
    if (!MemoryType.isCategoryNeutralScalar()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "non-byte-sized pointer loads are not supported");
    }
    auto WideMemoryType = LowLevelType::makeCategoryNeutralScalar(MemoryStoreBits);
    if (!WideMemoryType)
      return std::unexpected(WideMemoryType.error());
    auto WideMemory = deriveMemoryOperand(Function, Memory, *WideMemoryType, 0);
    if (!WideMemory)
      return std::unexpected(WideMemory.error());

    LowLevelType LoadType = *DestinationType;
    Register LoadDestination = *Destination;
    if (MemoryStoreBits > DestinationType->minimumBitWidth()) {
      LoadType = *WideMemoryType;
      auto Temporary = MRI.createVirtualRegister(LoadType);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      LoadDestination = *Temporary;
    }

    if (Generic == GenericOpcode::G_SEXTLOAD) {
      auto Loaded = MRI.createVirtualRegister(LoadType);
      if (!Loaded)
        return std::unexpected(Loaded.error());
      if (auto Built = BuildLoad(GenericOpcode::G_LOAD, *Loaded, *Address, **WideMemory); !Built)
        return std::unexpected(Built.error());
      if (auto Extended = MIRBuilder.buildSExtInReg(LoadDestination, *Loaded,
                                                    static_cast<std::uint32_t>(MemoryBits));
          !Extended) {
        return std::unexpected(Extended.error());
      }
    } else if (Generic == GenericOpcode::G_ZEXTLOAD || *WideMemoryType == LoadType) {
      auto Loaded = MRI.createVirtualRegister(LoadType);
      if (!Loaded)
        return std::unexpected(Loaded.error());
      if (auto Built = BuildLoad(GenericOpcode::G_LOAD, *Loaded, *Address, **WideMemory); !Built)
        return std::unexpected(Built.error());
      auto Assert = MIRBuilder.buildInstr(opcode(GenericOpcode::G_ASSERT_ZEXT));
      if (!Assert)
        return std::unexpected(Assert.error());
      if (auto Added = Assert->addDef(LoadDestination); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Assert->addUse(*Loaded); !Added)
        return std::unexpected(Added.error());
      if (auto Added = Assert->addImmediate(static_cast<std::int64_t>(MemoryBits)); !Added)
        return std::unexpected(Added.error());
      if (auto Built = Assert->build(); !Built)
        return std::unexpected(Built.error());
    } else if (auto Built =
                   BuildLoad(GenericOpcode::G_LOAD, LoadDestination, *Address, **WideMemory);
               !Built) {
      return std::unexpected(Built.error());
    }

    if (*DestinationType != LoadType) {
      if (auto Truncated = MIRBuilder.buildTrunc(*Destination, LoadDestination); !Truncated)
        return std::unexpected(Truncated.error());
    }
    return Instruction.eraseFromParent();
  }

  std::uint64_t LargeSplitBits = 0;
  std::uint64_t SmallSplitBits = 0;
  if (!std::has_single_bit(MemoryBits)) {
    LargeSplitBits = std::bit_floor(MemoryBits);
    SmallSplitBits = MemoryBits - LargeSplitBits;
  } else {
    if (MemoryBits < 16U) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "load lowering cannot split a byte-sized memory operation");
    }
    LargeSplitBits = MemoryBits / 2U;
    SmallSplitBits = LargeSplitBits;
  }
  if (LargeSplitBits == 0 || SmallSplitBits == 0 || LargeSplitBits % 8U != 0 ||
      SmallSplitBits % 8U != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "load lowering cannot split the requested memory width into bytes");
  }

  const std::uint64_t AnyExtBits = std::bit_ceil(DestinationType->minimumBitWidth());
  if (AnyExtBits == 0 || AnyExtBits > LowLevelType::MaxBitWidth) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "load lowering result width cannot be represented by the machine IR");
  }
  auto AnyExtType = LowLevelType::makeCategoryNeutralScalar(AnyExtBits);
  auto LargeMemoryType = LowLevelType::makeCategoryNeutralScalar(LargeSplitBits);
  auto SmallMemoryType = LowLevelType::makeCategoryNeutralScalar(SmallSplitBits);
  if (!AnyExtType)
    return std::unexpected(AnyExtType.error());
  if (!LargeMemoryType)
    return std::unexpected(LargeMemoryType.error());
  if (!SmallMemoryType)
    return std::unexpected(SmallMemoryType.error());
  auto LargeMemory = deriveMemoryOperand(Function, Memory, *LargeMemoryType, 0);
  auto SmallMemory = deriveMemoryOperand(Function, Memory, *SmallMemoryType, LargeSplitBits / 8U);
  if (!LargeMemory)
    return std::unexpected(LargeMemory.error());
  if (!SmallMemory)
    return std::unexpected(SmallMemory.error());

  auto LargeLoad = MRI.createVirtualRegister(*AnyExtType);
  if (!LargeLoad)
    return std::unexpected(LargeLoad.error());
  if (auto Built = BuildLoad(GenericOpcode::G_ZEXTLOAD, *LargeLoad, *Address, **LargeMemory);
      !Built)
    return std::unexpected(Built.error());
  auto SmallAddress = buildObjectPointerOffset(MIRBuilder, *Address, LargeSplitBits / 8U);
  if (!SmallAddress)
    return std::unexpected(SmallAddress.error());
  auto SmallLoad = MRI.createVirtualRegister(*AnyExtType);
  if (!SmallLoad)
    return std::unexpected(SmallLoad.error());
  if (auto Built = BuildLoad(Generic, *SmallLoad, *SmallAddress, **SmallMemory); !Built)
    return std::unexpected(Built.error());
  auto ShiftAmount =
      buildConstantRegister(MIRBuilder, *AnyExtType, static_cast<std::int64_t>(LargeSplitBits));
  if (!ShiftAmount)
    return std::unexpected(ShiftAmount.error());
  auto Shifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *AnyExtType,
                                        {*SmallLoad, *ShiftAmount});
  if (!Shifted)
    return std::unexpected(Shifted.error());

  if (*AnyExtType == *DestinationType) {
    if (auto Combined = MIRBuilder.buildOr(*Destination, *Shifted, *LargeLoad); !Combined)
      return std::unexpected(Combined.error());
  } else {
    auto Combined = buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, *AnyExtType,
                                           {*Shifted, *LargeLoad});
    if (!Combined)
      return std::unexpected(Combined.error());
    if (AnyExtBits != DestinationType->minimumBitWidth()) {
      if (auto Truncated = MIRBuilder.buildTrunc(*Destination, *Combined); !Truncated)
        return std::unexpected(Truncated.error());
    } else if (DestinationType->isPointer()) {
      if (auto Converted = MIRBuilder.buildIntToPtr(*Destination, *Combined); !Converted)
        return std::unexpected(Converted.error());
    } else {
      return makeUnexpected(ErrorCode::InternalError,
                            "load lowering produced an incompatible scalar result type");
    }
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerStore(MachineInstr &Instruction) {
  if (Instruction.opcode() != opcode(GenericOpcode::G_STORE)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "store lowering received an unsupported opcode");
  }
  if (Instruction.operandCount() != 2 || Instruction.memOperands().size() != 1 ||
      Instruction.memOperands().front() == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "store lowering requires one value, one address, and one memory operand");
  }

  const MachineOperand *ValueOperand = Instruction.operandAt(0);
  const MachineOperand *AddressOperand = Instruction.operandAt(1);
  const Register *Value = ValueOperand == nullptr ? nullptr : ValueOperand->getIf<Register>();
  const Register *Address = AddressOperand == nullptr ? nullptr : AddressOperand->getIf<Register>();
  if (Value == nullptr || Address == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "store lowering requires register value and address operands");
  }
  auto ValueType = MRI.getType(*Value);
  if (!ValueType)
    return std::unexpected(ValueType.error());
  if (!ValueType->isScalarValue()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "store lowering does not support vector value types");
  }

  const MachineMemOperand &Memory = *Instruction.memOperands().front();
  const MachineMemOperandDesc &Description = Memory.description();
  if (Memory.isAtomic())
    return makeUnexpected(ErrorCode::Unsupported,
                          "store lowering does not support atomic operations");
  if (!Description.Size.isKnown() || Description.Size.isScalable() || !Description.MemoryType ||
      Description.MemoryType->isVector()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "store lowering requires a fixed scalar memory operand");
  }
  const LowLevelType MemoryType = *Description.MemoryType;
  const std::uint64_t StoreBits = MemoryType.minimumBitWidth();
  const std::uint64_t StoreSizeBits = Description.Size.minimumBytes() * 8U;
  if (StoreBits == 0 || StoreBits > StoreSizeBits) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "store lowering has an inconsistent memory type and size");
  }

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  const auto BuildStore = [&](Register StoredValue, Register Pointer,
                              const MachineMemOperand &StoreMemory) -> Expected<void> {
    auto Store = MIRBuilder.buildInstr(opcode(GenericOpcode::G_STORE));
    if (!Store)
      return std::unexpected(Store.error());
    if (auto Added = Store->addUse(StoredValue); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Store->addUse(Pointer); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Store->addMemOperand(StoreMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = Store->build(); !Built)
      return std::unexpected(Built.error());
    return {};
  };

  Register Source = *Value;
  LowLevelType SourceType = *ValueType;
  if (StoreBits != StoreSizeBits) {
    if (!MemoryType.isCategoryNeutralScalar() || !SourceType.isCategoryNeutralScalar()) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "non-byte-sized pointer stores are not supported");
    }
    auto WideMemoryType = LowLevelType::makeCategoryNeutralScalar(StoreSizeBits);
    if (!WideMemoryType)
      return std::unexpected(WideMemoryType.error());
    if (StoreSizeBits > SourceType.minimumBitWidth()) {
      auto Extended =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_ANYEXT, *WideMemoryType, {Source});
      if (!Extended)
        return std::unexpected(Extended.error());
      Source = *Extended;
      SourceType = *WideMemoryType;
    }
    auto MaskValue =
        APInt::getLowBitsSet(SourceType.scalarBitWidth(), static_cast<std::uint32_t>(StoreBits));
    if (!MaskValue)
      return std::unexpected(MaskValue.error());
    auto Mask = buildConstantRegister(MIRBuilder, SourceType, std::move(*MaskValue));
    if (!Mask)
      return std::unexpected(Mask.error());
    auto ZeroExtended =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, SourceType, {Source, *Mask});
    if (!ZeroExtended)
      return std::unexpected(ZeroExtended.error());
    auto WideMemory = deriveMemoryOperand(Function, Memory, *WideMemoryType, 0);
    if (!WideMemory)
      return std::unexpected(WideMemory.error());
    if (auto Built = BuildStore(*ZeroExtended, *Address, **WideMemory); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }

  std::uint64_t LargeSplitBits = 0;
  std::uint64_t SmallSplitBits = 0;
  if (!std::has_single_bit(StoreBits)) {
    LargeSplitBits = std::bit_floor(StoreBits);
    SmallSplitBits = StoreBits - LargeSplitBits;
  } else {
    if (StoreBits < 16U) {
      return makeUnexpected(ErrorCode::Unsupported,
                            "store lowering cannot split a byte-sized memory operation");
    }
    LargeSplitBits = StoreBits / 2U;
    SmallSplitBits = LargeSplitBits;
  }
  if (LargeSplitBits == 0 || SmallSplitBits == 0 || LargeSplitBits % 8U != 0 ||
      SmallSplitBits % 8U != 0) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "store lowering cannot split the requested memory width into bytes");
  }

  const std::uint64_t AnyExtBits = std::bit_ceil(StoreBits);
  if (AnyExtBits == 0 || AnyExtBits > LowLevelType::MaxBitWidth) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "store lowering value width cannot be represented by the machine IR");
  }
  auto NewSourceType = LowLevelType::makeCategoryNeutralScalar(AnyExtBits);
  auto LargeMemoryType = LowLevelType::makeCategoryNeutralScalar(LargeSplitBits);
  auto SmallMemoryType = LowLevelType::makeCategoryNeutralScalar(SmallSplitBits);
  if (!NewSourceType)
    return std::unexpected(NewSourceType.error());
  if (!LargeMemoryType)
    return std::unexpected(LargeMemoryType.error());
  if (!SmallMemoryType)
    return std::unexpected(SmallMemoryType.error());

  if (SourceType.isPointer()) {
    auto IntegerPointerType = LowLevelType::makeCategoryNeutralScalar(SourceType.scalarBitWidth());
    if (!IntegerPointerType)
      return std::unexpected(IntegerPointerType.error());
    auto Converted = buildResultInstruction(MIRBuilder, GenericOpcode::G_PTRTOINT,
                                            *IntegerPointerType, {Source});
    if (!Converted)
      return std::unexpected(Converted.error());
    Source = *Converted;
    SourceType = *IntegerPointerType;
  }
  auto Extended = MIRBuilder.buildAnyExtOrTrunc(*NewSourceType, Source);
  if (!Extended)
    return std::unexpected(Extended.error());
  auto ExtendedValue = resultRegister(**Extended);
  if (!ExtendedValue)
    return std::unexpected(ExtendedValue.error());
  auto ShiftAmount =
      buildConstantRegister(MIRBuilder, *NewSourceType, static_cast<std::int64_t>(LargeSplitBits));
  if (!ShiftAmount)
    return std::unexpected(ShiftAmount.error());
  auto SmallValue = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *NewSourceType,
                                           {*ExtendedValue, *ShiftAmount});
  if (!SmallValue)
    return std::unexpected(SmallValue.error());
  auto SmallAddress = buildObjectPointerOffset(MIRBuilder, *Address, LargeSplitBits / 8U);
  if (!SmallAddress)
    return std::unexpected(SmallAddress.error());
  auto LargeMemory = deriveMemoryOperand(Function, Memory, *LargeMemoryType, 0);
  auto SmallMemory = deriveMemoryOperand(Function, Memory, *SmallMemoryType, LargeSplitBits / 8U);
  if (!LargeMemory)
    return std::unexpected(LargeMemory.error());
  if (!SmallMemory)
    return std::unexpected(SmallMemory.error());
  if (auto Built = BuildStore(*ExtendedValue, *Address, **LargeMemory); !Built)
    return std::unexpected(Built.error());
  if (auto Built = BuildStore(*SmallValue, *SmallAddress, **SmallMemory); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lower(MachineInstr &Instruction,
                                      const LegalizationDecision &Decision) {
  if (Decision.Action != LegalizationAction::Lower)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "lower received a non-lower legalization action");
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_ABS:
    return lowerAbsToAddXor(Instruction);
  case GenericOpcode::G_LOAD:
  case GenericOpcode::G_SEXTLOAD:
  case GenericOpcode::G_ZEXTLOAD:
    return lowerLoad(Instruction);
  case GenericOpcode::G_STORE:
    return lowerStore(Instruction);
  case GenericOpcode::G_SMIN:
  case GenericOpcode::G_SMAX:
  case GenericOpcode::G_UMIN:
  case GenericOpcode::G_UMAX:
    return lowerMinMax(Instruction);
  case GenericOpcode::G_SCMP:
  case GenericOpcode::G_UCMP:
    return lowerThreewayCompare(Instruction);
  case GenericOpcode::G_SDIVREM:
  case GenericOpcode::G_UDIVREM:
    return lowerDivRem(Instruction);
  case GenericOpcode::G_SMULH:
  case GenericOpcode::G_UMULH:
    return lowerHighMultiply(Instruction);
  case GenericOpcode::G_SMULO:
  case GenericOpcode::G_UMULO:
    return lowerMulo(Instruction);
  case GenericOpcode::G_CTLZ_ZERO_POISON:
  case GenericOpcode::G_CTTZ_ZERO_POISON:
  case GenericOpcode::G_CTLZ:
  case GenericOpcode::G_CTTZ:
  case GenericOpcode::G_CTPOP:
  case GenericOpcode::G_CTLS:
    return lowerBitCount(Instruction);
  case GenericOpcode::G_BSWAP:
    return lowerBswap(Instruction);
  case GenericOpcode::G_FSHL:
  case GenericOpcode::G_FSHR:
    return lowerFunnelShift(Instruction);
  case GenericOpcode::G_ROTL:
  case GenericOpcode::G_ROTR:
    return lowerRotate(Instruction);
  case GenericOpcode::G_DYN_STACKALLOC:
    return lowerDynStackAlloc(Instruction);
  case GenericOpcode::G_STACKSAVE:
    return lowerStackSave(Instruction);
  case GenericOpcode::G_STACKRESTORE:
    return lowerStackRestore(Instruction);
  case GenericOpcode::G_MEMCPY_INLINE:
    return lowerMemcpyInline(Instruction);
  case GenericOpcode::G_ABDS:
  case GenericOpcode::G_ABDU: {
    const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_ABDS);
    const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
    auto Type = MRI.getType(Destination);
    if (!Type)
      return std::unexpected(Type.error());

    const GenericOpcode MinOpcode = IsSigned ? GenericOpcode::G_SMIN : GenericOpcode::G_UMIN;
    const GenericOpcode MaxOpcode = IsSigned ? GenericOpcode::G_SMAX : GenericOpcode::G_UMAX;
    auto MinLegal = Info.isLegal(LegalizationQuery(opcode(MinOpcode), {*Type}));
    if (!MinLegal)
      return std::unexpected(MinLegal.error());
    auto MaxLegal = Info.isLegal(LegalizationQuery(opcode(MaxOpcode), {*Type}));
    if (!MaxLegal)
      return std::unexpected(MaxLegal.error());
    if (*MinLegal && *MaxLegal)
      return lowerAbsDiffToMinMax(Instruction);
    return lowerAbsDiffToSelect(Instruction);
  }
  case GenericOpcode::G_SADDO:
  case GenericOpcode::G_SSUBO:
  case GenericOpcode::G_SADDE:
  case GenericOpcode::G_SSUBE:
    return lowerSignedAddSubOverflow(Instruction);
  case GenericOpcode::G_UADDSAT:
  case GenericOpcode::G_SADDSAT:
  case GenericOpcode::G_USUBSAT:
  case GenericOpcode::G_SSUBSAT: {
    const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
    const bool IsSigned =
        Generic == GenericOpcode::G_SADDSAT || Generic == GenericOpcode::G_SSUBSAT;
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    if (Destination == nullptr)
      return makeUnexpected(ErrorCode::VerificationError,
                            "saturating add/sub lowering requires a register destination");
    auto Type = MRI.getType(*Destination);
    if (!Type)
      return std::unexpected(Type.error());

    const GenericOpcode MinOpcode = IsSigned ? GenericOpcode::G_SMIN : GenericOpcode::G_UMIN;
    auto MinLegal = Info.isLegal(LegalizationQuery(opcode(MinOpcode), {*Type}));
    if (!MinLegal)
      return std::unexpected(MinLegal.error());
    if (!IsSigned && *MinLegal)
      return lowerSaturatingAddSubToMinMax(Instruction);
    if (IsSigned) {
      auto MaxLegal = Info.isLegal(LegalizationQuery(opcode(GenericOpcode::G_SMAX), {*Type}));
      if (!MaxLegal)
        return std::unexpected(MaxLegal.error());
      if (*MinLegal && *MaxLegal)
        return lowerSaturatingAddSubToMinMax(Instruction);
    }
    return lowerSaturatingAddSubToOverflow(Instruction);
  }
  case GenericOpcode::G_SSHLSAT:
  case GenericOpcode::G_USHLSAT:
    return lowerSaturatingShiftLeft(Instruction);
  case GenericOpcode::G_SEXT_INREG: {
    const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
    const Register Source = *Instruction.operandAt(1)->getIf<Register>();
    auto SizeInBits = signedImmediate(*Instruction.operandAt(2));
    if (!SizeInBits)
      return std::unexpected(SizeInBits.error());
    auto Type = MRI.getType(Destination);
    if (!Type)
      return std::unexpected(Type.error());

    auto Temporary = MRI.createVirtualRegister(*Type);
    if (!Temporary)
      return std::unexpected(Temporary.error());

    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    const std::int64_t Shift = static_cast<std::int64_t>(Type->scalarBitWidth()) - *SizeInBits;
    auto ShiftAmount = buildConstantRegister(MIRBuilder, *Type, Shift);
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());
    if (auto Built = MIRBuilder.buildShl(*Temporary, Source, *ShiftAmount); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildAShr(Destination, *Temporary, *ShiftAmount); !Built)
      return std::unexpected(Built.error());

    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_VAARG:
    return lowerVAArg(Instruction);
  case GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS: {
    const Register OldValue = *Instruction.operandAt(0)->getIf<Register>();
    const Register Success = *Instruction.operandAt(1)->getIf<Register>();
    const Register Address = *Instruction.operandAt(2)->getIf<Register>();
    const Register Compare = *Instruction.operandAt(3)->getIf<Register>();
    const Register NewValue = *Instruction.operandAt(4)->getIf<Register>();
    auto NewOldValue = MRI.cloneVirtualRegister(OldValue);
    if (!NewOldValue)
      return std::unexpected(NewOldValue.error());

    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    auto Atomic = MIRBuilder.buildInstr(opcode(GenericOpcode::G_ATOMIC_CMPXCHG));
    if (!Atomic)
      return std::unexpected(Atomic.error());
    if (auto Added = Atomic->addDef(*NewOldValue); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Atomic->addUse(Address); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Atomic->addUse(Compare); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Atomic->addUse(NewValue); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Atomic->addMemOperand(*Instruction.memOperands().front()); !Added)
      return std::unexpected(Added.error());
    if (auto Built = Atomic->build(); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildICmp(IntegerPredicate::Equal, Success, *NewOldValue, Compare);
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildCopy(OldValue, *NewOldValue); !Built)
      return std::unexpected(Built.error());

    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_ATOMICRMW_SUB: {
    const Register Result = *Instruction.operandAt(0)->getIf<Register>();
    const Register Address = *Instruction.operandAt(1)->getIf<Register>();
    const Register Value = *Instruction.operandAt(2)->getIf<Register>();
    auto ValueType = MRI.getType(Value);
    if (!ValueType)
      return std::unexpected(ValueType.error());

    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    auto Zero = buildConstantRegister(MIRBuilder, *ValueType, 0);
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Negated =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *ValueType, {*Zero, Value});
    if (!Negated)
      return std::unexpected(Negated.error());

    auto AtomicAdd = MIRBuilder.buildInstr(opcode(GenericOpcode::G_ATOMICRMW_ADD));
    if (!AtomicAdd)
      return std::unexpected(AtomicAdd.error());
    if (auto Added = AtomicAdd->addDef(Result); !Added)
      return std::unexpected(Added.error());
    if (auto Added = AtomicAdd->addUse(Address); !Added)
      return std::unexpected(Added.error());
    if (auto Added = AtomicAdd->addUse(*Negated); !Added)
      return std::unexpected(Added.error());
    if (auto Added = AtomicAdd->addMemOperand(*Instruction.memOperands().front()); !Added)
      return std::unexpected(Added.error());
    if (auto Built = AtomicAdd->build(); !Built)
      return std::unexpected(Built.error());

    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_UADDO: {
    const Register Result = *Instruction.operandAt(0)->getIf<Register>();
    const Register CarryOut = *Instruction.operandAt(1)->getIf<Register>();
    const Register Left = *Instruction.operandAt(2)->getIf<Register>();
    const Register Right = *Instruction.operandAt(3)->getIf<Register>();
    auto NewResult = MRI.cloneVirtualRegister(Result);
    if (!NewResult)
      return std::unexpected(NewResult.error());
    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    if (auto Built = MIRBuilder.buildAdd(*NewResult, Left, Right); !Built)
      return std::unexpected(Built.error());
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::UnsignedLessThan, CarryOut, *NewResult, Right);
        !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildCopy(Result, *NewResult); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_UADDE: {
    const Register Result = *Instruction.operandAt(0)->getIf<Register>();
    const Register CarryOut = *Instruction.operandAt(1)->getIf<Register>();
    const Register Left = *Instruction.operandAt(2)->getIf<Register>();
    const Register Right = *Instruction.operandAt(3)->getIf<Register>();
    const Register CarryIn = *Instruction.operandAt(4)->getIf<Register>();
    auto ResultType = MRI.getType(Result);
    auto ConditionType = MRI.getType(CarryOut);
    if (!ResultType)
      return std::unexpected(ResultType.error());
    if (!ConditionType)
      return std::unexpected(ConditionType.error());
    auto NewResult = MRI.cloneVirtualRegister(Result);
    auto TemporaryResult = MRI.createVirtualRegister(*ResultType);
    auto Carry = MRI.createVirtualRegister(*ConditionType);
    auto ExtendedCarryIn = MRI.createVirtualRegister(*ResultType);
    auto ResultIsZero = MRI.createVirtualRegister(*ConditionType);
    auto SecondCarry = MRI.createVirtualRegister(*ConditionType);
    if (!NewResult)
      return std::unexpected(NewResult.error());
    if (!TemporaryResult)
      return std::unexpected(TemporaryResult.error());
    if (!Carry)
      return std::unexpected(Carry.error());
    if (!ExtendedCarryIn)
      return std::unexpected(ExtendedCarryIn.error());
    if (!ResultIsZero)
      return std::unexpected(ResultIsZero.error());
    if (!SecondCarry)
      return std::unexpected(SecondCarry.error());
    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    if (auto Built = MIRBuilder.buildAdd(*TemporaryResult, Left, Right); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildICmp(IntegerPredicate::UnsignedLessThan, *Carry,
                                          *TemporaryResult, Left);
        !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildZExt(*ExtendedCarryIn, CarryIn); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildAdd(*NewResult, *TemporaryResult, *ExtendedCarryIn); !Built)
      return std::unexpected(Built.error());
    auto Zero = buildConstantRegister(MIRBuilder, *ResultType, 0);
    if (!Zero)
      return std::unexpected(Zero.error());
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::Equal, *ResultIsZero, *NewResult, *Zero);
        !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildAnd(*SecondCarry, *ResultIsZero, CarryIn); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildOr(CarryOut, *Carry, *SecondCarry); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildCopy(Result, *NewResult); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_USUBO: {
    const Register Result = *Instruction.operandAt(0)->getIf<Register>();
    const Register BorrowOut = *Instruction.operandAt(1)->getIf<Register>();
    const Register Left = *Instruction.operandAt(2)->getIf<Register>();
    const Register Right = *Instruction.operandAt(3)->getIf<Register>();
    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    if (auto Built = MIRBuilder.buildSub(Result, Left, Right); !Built)
      return std::unexpected(Built.error());
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::UnsignedLessThan, BorrowOut, Left, Right);
        !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }
  case GenericOpcode::G_USUBE: {
    const Register Result = *Instruction.operandAt(0)->getIf<Register>();
    const Register BorrowOut = *Instruction.operandAt(1)->getIf<Register>();
    const Register Left = *Instruction.operandAt(2)->getIf<Register>();
    const Register Right = *Instruction.operandAt(3)->getIf<Register>();
    const Register BorrowIn = *Instruction.operandAt(4)->getIf<Register>();
    auto ResultType = MRI.getType(Result);
    auto ConditionType = MRI.getType(BorrowOut);
    if (!ResultType)
      return std::unexpected(ResultType.error());
    if (!ConditionType)
      return std::unexpected(ConditionType.error());
    auto TemporaryResult = MRI.createVirtualRegister(*ResultType);
    auto Borrow = MRI.createVirtualRegister(*ConditionType);
    auto ExtendedBorrowIn = MRI.createVirtualRegister(*ResultType);
    auto TemporaryResultIsZero = MRI.createVirtualRegister(*ConditionType);
    auto SecondBorrow = MRI.createVirtualRegister(*ConditionType);
    if (!TemporaryResult)
      return std::unexpected(TemporaryResult.error());
    if (!Borrow)
      return std::unexpected(Borrow.error());
    if (!ExtendedBorrowIn)
      return std::unexpected(ExtendedBorrowIn.error());
    if (!TemporaryResultIsZero)
      return std::unexpected(TemporaryResultIsZero.error());
    if (!SecondBorrow)
      return std::unexpected(SecondBorrow.error());
    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());
    if (auto Built = MIRBuilder.buildSub(*TemporaryResult, Left, Right); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildICmp(IntegerPredicate::UnsignedGreaterThan, *Borrow,
                                          *TemporaryResult, Left);
        !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildZExt(*ExtendedBorrowIn, BorrowIn); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildSub(Result, *TemporaryResult, *ExtendedBorrowIn); !Built)
      return std::unexpected(Built.error());
    auto Zero = buildConstantRegister(MIRBuilder, *ResultType, 0);
    if (!Zero)
      return std::unexpected(Zero.error());
    if (auto Built = MIRBuilder.buildICmp(IntegerPredicate::Equal, *TemporaryResultIsZero,
                                          *TemporaryResult, *Zero);
        !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildAnd(*SecondBorrow, *TemporaryResultIsZero, BorrowIn); !Built)
      return std::unexpected(Built.error());
    if (auto Built = MIRBuilder.buildOr(BorrowOut, *Borrow, *SecondBorrow); !Built)
      return std::unexpected(Built.error());
    return Instruction.eraseFromParent();
  }
  default: {
    const InstructionDescriptor *Descriptor =
        MIRBuilder.target().instructionDescriptor(Instruction.opcode());
    const std::string_view Name =
        Descriptor == nullptr ? std::string_view{"<unknown>"} : Descriptor->Name;
    return makeUnexpected(ErrorCode::Unsupported,
                          "generic lowering is not implemented for opcode '" + std::string(Name) +
                              "'");
  }
  }
}

Expected<void> LegalizerHelper::lowerDynStackAlloc(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_DYN_STACKALLOC requires destination, size, and alignment operands");
  if (MIRBuilder.target().stackGrowthDirection() != StackGrowthDirection::Down)
    return makeUnexpected(ErrorCode::Unsupported,
                          "dynamic stack allocation requires a downward-growing stack");

  const PhysicalRegister StackPointer = MIRBuilder.target().stackPointerRegisterToSaveRestore();
  if (!StackPointer.isValid())
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a stack pointer for dynamic allocation");

  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *SizeOperand = Instruction.operandAt(1);
  const MachineOperand *AlignmentOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *AllocationSize =
      SizeOperand == nullptr ? nullptr : SizeOperand->getIf<Register>();
  if (Destination == nullptr || AllocationSize == nullptr || AlignmentOperand == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "G_DYN_STACKALLOC has invalid operands");

  auto PointerType = MRI.getType(*Destination);
  auto AllocationSizeType = MRI.getType(*AllocationSize);
  auto AlignmentValue = signedImmediate(*AlignmentOperand);
  if (!PointerType)
    return std::unexpected(PointerType.error());
  if (!AllocationSizeType)
    return std::unexpected(AllocationSizeType.error());
  if (!AlignmentValue)
    return std::unexpected(AlignmentValue.error());
  if (!PointerType->isPointer() || !AllocationSizeType->isCategoryNeutralScalar() ||
      PointerType->scalarBitWidth() != AllocationSizeType->scalarBitWidth()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_DYN_STACKALLOC requires pointer-width integer allocation size");
  }
  if (*AlignmentValue <= 0 || !std::has_single_bit(static_cast<std::uint64_t>(*AlignmentValue))) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_DYN_STACKALLOC alignment must be a positive power of two");
  }

  auto StackPointerCopy = MRI.createVirtualRegister(*PointerType);
  auto IntegerStackPointer = MRI.createVirtualRegister(*AllocationSizeType);
  auto Allocation = MRI.createVirtualRegister(*AllocationSizeType);
  if (!StackPointerCopy)
    return std::unexpected(StackPointerCopy.error());
  if (!IntegerStackPointer)
    return std::unexpected(IntegerStackPointer.error());
  if (!Allocation)
    return std::unexpected(Allocation.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildCopy(*StackPointerCopy, Register::fromPhysical(StackPointer));
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildPtrToInt(*IntegerStackPointer, *StackPointerCopy); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildSub(*Allocation, *IntegerStackPointer, *AllocationSize); !Built)
    return std::unexpected(Built.error());

  Register AlignedAllocation = *Allocation;
  if (*AlignmentValue > 1) {
    auto LowBits = APInt::fromUnsigned(AllocationSizeType->scalarBitWidth(),
                                       static_cast<std::uint64_t>(*AlignmentValue) - 1U);
    if (!LowBits)
      return std::unexpected(LowBits.error());
    auto AlignmentMask = LowBits->bitwiseNot();
    if (!AlignmentMask)
      return std::unexpected(AlignmentMask.error());
    auto Mask = buildConstantRegister(MIRBuilder, *AllocationSizeType, std::move(*AlignmentMask));
    if (!Mask)
      return std::unexpected(Mask.error());
    auto MaskedAllocation = MRI.createVirtualRegister(*AllocationSizeType);
    if (!MaskedAllocation)
      return std::unexpected(MaskedAllocation.error());
    if (auto Built = MIRBuilder.buildAnd(*MaskedAllocation, *Allocation, *Mask); !Built)
      return std::unexpected(Built.error());
    AlignedAllocation = *MaskedAllocation;
  }

  auto TargetPointer = MRI.createVirtualRegister(*PointerType);
  if (!TargetPointer)
    return std::unexpected(TargetPointer.error());
  if (auto Built = MIRBuilder.buildIntToPtr(*TargetPointer, AlignedAllocation); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildCopy(Register::fromPhysical(StackPointer), *TargetPointer);
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildCopy(*Destination, *TargetPointer); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerStackSave(MachineInstr &Instruction) {
  const PhysicalRegister StackPointer = MIRBuilder.target().stackPointerRegisterToSaveRestore();
  if (!StackPointer.isValid())
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a stack pointer for stack save");
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Instruction.operandCount() != 1 || Destination == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_STACKSAVE requires one register destination");
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildCopy(*Destination, Register::fromPhysical(StackPointer));
      !Built) {
    return std::unexpected(Built.error());
  }
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerStackRestore(MachineInstr &Instruction) {
  const PhysicalRegister StackPointer = MIRBuilder.target().stackPointerRegisterToSaveRestore();
  if (!StackPointer.isValid())
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a stack pointer for stack restore");
  const MachineOperand *SourceOperand = Instruction.operandAt(0);
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Instruction.operandCount() != 1 || Source == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_STACKRESTORE requires one register source");
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildCopy(Register::fromPhysical(StackPointer), *Source); !Built)
    return std::unexpected(Built.error());
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerMemcpyInline(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 3 || Instruction.memOperands().size() != 2 ||
      Instruction.memOperands()[0] == nullptr || Instruction.memOperands()[1] == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_MEMCPY_INLINE requires destination, source, length, and two MMOs");
  }

  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *SourceOperand = Instruction.operandAt(1);
  const MachineOperand *LengthOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  const Register *Length = LengthOperand == nullptr ? nullptr : LengthOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr || Length == nullptr ||
      !Destination->isVirtual() || !Source->isVirtual() || !Length->isVirtual()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_MEMCPY_INLINE requires virtual-register operands");
  }

  const MachineMemOperand &StoreMemory = *Instruction.memOperands()[0];
  const MachineMemOperand &LoadMemory = *Instruction.memOperands()[1];
  if (!StoreMemory.isStore() || StoreMemory.isLoad() || !LoadMemory.isLoad() ||
      LoadMemory.isStore() || StoreMemory.isAtomic() || LoadMemory.isAtomic()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "G_MEMCPY_INLINE requires non-atomic store and load memory operands");
  }

  const std::optional<ValueAndVReg> ConstantLength =
      getIConstantVRegValWithLookThrough(*Length, MRI);
  if (!ConstantLength || ConstantLength->Value.bitWidth() == 0 ||
      ConstantLength->Value.bitWidth() > 64) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "G_MEMCPY_INLINE requires a constant length no wider than 64 bits");
  }
  const std::uint64_t KnownLength = ConstantLength->Value.words().front();
  if (KnownLength == 0) {
    return Instruction.eraseFromParent();
  }

  auto ByteType = LowLevelType::makeCategoryNeutralScalar(8);
  if (!ByteType)
    return std::unexpected(ByteType.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  MachineFunction &Owner = MIRBuilder.function();
  for (std::uint64_t Offset = 0; Offset != KnownLength; ++Offset) {
    auto LoadAddress = buildObjectPointerOffset(MIRBuilder, *Source, Offset);
    if (!LoadAddress)
      return std::unexpected(LoadAddress.error());
    auto ByteLoadMemory = deriveMemoryOperand(Owner, LoadMemory, *ByteType, Offset);
    if (!ByteLoadMemory)
      return std::unexpected(ByteLoadMemory.error());
    auto LoadedByte = MRI.createVirtualRegister(*ByteType);
    if (!LoadedByte)
      return std::unexpected(LoadedByte.error());
    auto Load = MIRBuilder.buildInstr(opcode(GenericOpcode::G_LOAD));
    if (!Load)
      return std::unexpected(Load.error());
    if (auto Added = Load->addDef(*LoadedByte); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Load->addUse(*LoadAddress); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Load->addMemOperand(**ByteLoadMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = Load->build(); !Built)
      return std::unexpected(Built.error());

    auto StoreAddress = buildObjectPointerOffset(MIRBuilder, *Destination, Offset);
    if (!StoreAddress)
      return std::unexpected(StoreAddress.error());
    auto ByteStoreMemory = deriveMemoryOperand(Owner, StoreMemory, *ByteType, Offset);
    if (!ByteStoreMemory)
      return std::unexpected(ByteStoreMemory.error());
    auto Store = MIRBuilder.buildInstr(opcode(GenericOpcode::G_STORE));
    if (!Store)
      return std::unexpected(Store.error());
    if (auto Added = Store->addUse(*LoadedByte); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Store->addUse(*StoreAddress); !Added)
      return std::unexpected(Added.error());
    if (auto Added = Store->addMemOperand(**ByteStoreMemory); !Added)
      return std::unexpected(Added.error());
    if (auto Built = Store->build(); !Built)
      return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerAbsToAddXor(MachineInstr &Instruction) {
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Source = *Instruction.operandAt(1)->getIf<Register>();
  auto Type = MRI.getType(Destination);
  if (!Type)
    return std::unexpected(Type.error());

  auto Shift = MRI.createVirtualRegister(*Type);
  auto Add = MRI.createVirtualRegister(*Type);
  if (!Shift)
    return std::unexpected(Shift.error());
  if (!Add)
    return std::unexpected(Add.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto ShiftAmount = buildConstantRegister(MIRBuilder, *Type,
                                           static_cast<std::int64_t>(Type->scalarBitWidth() - 1U));
  if (!ShiftAmount)
    return std::unexpected(ShiftAmount.error());
  if (auto Built = MIRBuilder.buildAShr(*Shift, Source, *ShiftAmount); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildAdd(*Add, Source, *Shift); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildXor(Destination, *Add, *Shift); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerAbsDiffToSelect(MachineInstr &Instruction) {
  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_ABDS);
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Left = *Instruction.operandAt(1)->getIf<Register>();
  const Register Right = *Instruction.operandAt(2)->getIf<Register>();
  auto Type = MRI.getType(Left);
  if (!Type)
    return std::unexpected(Type.error());
  auto ConditionType = LowLevelType::makeCategoryNeutralScalar(1);
  if (!ConditionType)
    return std::unexpected(ConditionType.error());

  auto LeftSub = MRI.createVirtualRegister(*Type);
  auto RightSub = MRI.createVirtualRegister(*Type);
  auto Condition = MRI.createVirtualRegister(*ConditionType);
  if (!LeftSub)
    return std::unexpected(LeftSub.error());
  if (!RightSub)
    return std::unexpected(RightSub.error());
  if (!Condition)
    return std::unexpected(Condition.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildSub(*LeftSub, Left, Right); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildSub(*RightSub, Right, Left); !Built)
    return std::unexpected(Built.error());
  const IntegerPredicate Predicate =
      IsSigned ? IntegerPredicate::SignedGreaterThan : IntegerPredicate::UnsignedGreaterThan;
  if (auto Built = MIRBuilder.buildICmp(Predicate, *Condition, Left, Right); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {Destination},
                                         {*Condition, *LeftSub, *RightSub});
      !Built) {
    return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerAbsDiffToMinMax(MachineInstr &Instruction) {
  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_ABDS);
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Left = *Instruction.operandAt(1)->getIf<Register>();
  const Register Right = *Instruction.operandAt(2)->getIf<Register>();
  auto Type = MRI.getType(Left);
  if (!Type)
    return std::unexpected(Type.error());

  auto Maximum = MRI.createVirtualRegister(*Type);
  auto Minimum = MRI.createVirtualRegister(*Type);
  if (!Maximum)
    return std::unexpected(Maximum.error());
  if (!Minimum)
    return std::unexpected(Minimum.error());

  const GenericOpcode MaxOpcode = IsSigned ? GenericOpcode::G_SMAX : GenericOpcode::G_UMAX;
  const GenericOpcode MinOpcode = IsSigned ? GenericOpcode::G_SMIN : GenericOpcode::G_UMIN;
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(MaxOpcode), {*Maximum}, {Left, Right}); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(MinOpcode), {*Minimum}, {Left, Right}); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildSub(Destination, *Maximum, *Minimum); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerMinMax(MachineInstr &Instruction) {
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Left = *Instruction.operandAt(1)->getIf<Register>();
  const Register Right = *Instruction.operandAt(2)->getIf<Register>();

  auto DestinationType = MRI.getType(Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto ConditionType = booleanTypeFor(*DestinationType);
  if (!ConditionType)
    return std::unexpected(ConditionType.error());

  auto Condition = MRI.createVirtualRegister(*ConditionType);
  if (!Condition)
    return std::unexpected(Condition.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  const auto Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (auto Built = MIRBuilder.buildICmp(minMaxToCompare(Generic), *Condition, Left, Right); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {Destination},
                                         {*Condition, Left, Right});
      !Built) {
    return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerThreewayCompare(MachineInstr &Instruction) {
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Left = *Instruction.operandAt(1)->getIf<Register>();
  const Register Right = *Instruction.operandAt(2)->getIf<Register>();
  auto DestinationType = MRI.getType(Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto ConditionType = booleanTypeFor(*DestinationType);
  if (!ConditionType)
    return std::unexpected(ConditionType.error());

  auto IsGreater = MRI.createVirtualRegister(*ConditionType);
  auto IsLess = MRI.createVirtualRegister(*ConditionType);
  if (!IsGreater)
    return std::unexpected(IsGreater.error());
  if (!IsLess)
    return std::unexpected(IsLess.error());

  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_SCMP);
  const IntegerPredicate GreaterPredicate =
      IsSigned ? IntegerPredicate::SignedGreaterThan : IntegerPredicate::UnsignedGreaterThan;
  const IntegerPredicate LessPredicate =
      IsSigned ? IntegerPredicate::SignedLessThan : IntegerPredicate::UnsignedLessThan;

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = MIRBuilder.buildICmp(GreaterPredicate, *IsGreater, Left, Right); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildICmp(LessPredicate, *IsLess, Left, Right); !Built)
    return std::unexpected(Built.error());

  const BooleanContent Contents =
      MIRBuilder.target().getBooleanContents(DestinationType->isVector(), false);
  if (Contents == BooleanContent::Undefined) {
    auto Zero = buildConstantRegister(MIRBuilder, *DestinationType, 0);
    auto One = buildConstantRegister(MIRBuilder, *DestinationType, 1);
    auto MinusOne = buildConstantRegister(MIRBuilder, *DestinationType, -1);
    if (!Zero)
      return std::unexpected(Zero.error());
    if (!One)
      return std::unexpected(One.error());
    if (!MinusOne)
      return std::unexpected(MinusOne.error());
    auto ZeroOrOne = buildResultInstruction(MIRBuilder, GenericOpcode::G_SELECT, *DestinationType,
                                            {*IsGreater, *One, *Zero});
    if (!ZeroOrOne)
      return std::unexpected(ZeroOrOne.error());
    if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {Destination},
                                           {*IsLess, *MinusOne, *ZeroOrOne});
        !Built) {
      return std::unexpected(Built.error());
    }
  } else {
    if (Contents == BooleanContent::ZeroOrNegativeOne)
      std::swap(IsGreater, IsLess);
    const GenericOpcode ExtensionOpcode = Contents == BooleanContent::ZeroOrNegativeOne
                                              ? GenericOpcode::G_SEXT
                                              : GenericOpcode::G_ZEXT;
    auto ExtendedGreater = MRI.createVirtualRegister(*DestinationType);
    auto ExtendedLess = MRI.createVirtualRegister(*DestinationType);
    if (!ExtendedGreater)
      return std::unexpected(ExtendedGreater.error());
    if (!ExtendedLess)
      return std::unexpected(ExtendedLess.error());
    if (auto Built =
            MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*ExtendedGreater}, {*IsGreater});
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*ExtendedLess}, {*IsLess});
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildSub(Destination, *ExtendedGreater, *ExtendedLess); !Built)
      return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerDivRem(MachineInstr &Instruction) {
  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_SDIVREM);
  const Register Quotient = *Instruction.operandAt(0)->getIf<Register>();
  const Register Remainder = *Instruction.operandAt(1)->getIf<Register>();
  const Register Dividend = *Instruction.operandAt(2)->getIf<Register>();
  const Register Divisor = *Instruction.operandAt(3)->getIf<Register>();

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  const GenericOpcode DivideOpcode = IsSigned ? GenericOpcode::G_SDIV : GenericOpcode::G_UDIV;
  const GenericOpcode RemainderOpcode = IsSigned ? GenericOpcode::G_SREM : GenericOpcode::G_UREM;
  if (auto Built = MIRBuilder.buildInstr(opcode(DivideOpcode), {Quotient}, {Dividend, Divisor});
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildInstr(opcode(RemainderOpcode), {Remainder}, {Dividend, Divisor});
      !Built) {
    return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerHighMultiply(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "high multiply lowering requires a destination and two sources");
  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_SMULH);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *FirstSourceOperand = Instruction.operandAt(1);
  const MachineOperand *SecondSourceOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *FirstSource =
      FirstSourceOperand == nullptr ? nullptr : FirstSourceOperand->getIf<Register>();
  const Register *SecondSource =
      SecondSourceOperand == nullptr ? nullptr : SecondSourceOperand->getIf<Register>();
  if (Destination == nullptr || FirstSource == nullptr || SecondSource == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "high multiply lowering requires register operands");
  auto OriginalType = MRI.getType(*Destination);
  if (!OriginalType)
    return std::unexpected(OriginalType.error());
  if (!OriginalType->isCategoryNeutralScalar() || OriginalType->scalarBitWidth() == 0 ||
      OriginalType->scalarBitWidth() > LowLevelType::MaxBitWidth / 2U) {
    return makeUnexpected(
        ErrorCode::Unsupported,
        "high multiply lowering requires a scalar type with a representable double width");
  }
  const std::uint32_t SizeInBits = OriginalType->scalarBitWidth();
  auto WideType = LowLevelType::makeCategoryNeutralScalar(SizeInBits * 2U);
  if (!WideType)
    return std::unexpected(WideType.error());

  auto WideFirstSource = MRI.createVirtualRegister(*WideType);
  auto WideSecondSource = MRI.createVirtualRegister(*WideType);
  if (!WideFirstSource)
    return std::unexpected(WideFirstSource.error());
  if (!WideSecondSource)
    return std::unexpected(WideSecondSource.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = IsSigned ? MIRBuilder.buildSExt(*WideFirstSource, *FirstSource)
                            : MIRBuilder.buildZExt(*WideFirstSource, *FirstSource);
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = IsSigned ? MIRBuilder.buildSExt(*WideSecondSource, *SecondSource)
                            : MIRBuilder.buildZExt(*WideSecondSource, *SecondSource);
      !Built) {
    return std::unexpected(Built.error());
  }
  auto Product = buildResultInstruction(MIRBuilder, GenericOpcode::G_MUL, *WideType,
                                        {*WideFirstSource, *WideSecondSource});
  if (!Product)
    return std::unexpected(Product.error());
  auto ShiftAmount = buildConstantRegister(MIRBuilder, *WideType, SizeInBits);
  if (!ShiftAmount)
    return std::unexpected(ShiftAmount.error());
  auto Shifted =
      buildResultInstruction(MIRBuilder, IsSigned ? GenericOpcode::G_ASHR : GenericOpcode::G_LSHR,
                             *WideType, {*Product, *ShiftAmount});
  if (!Shifted)
    return std::unexpected(Shifted.error());
  if (auto Built = MIRBuilder.buildTrunc(*Destination, *Shifted); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerMulo(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 4)
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply-overflow lowering requires four register operands");

  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_SMULO);
  const Register *ResultOperand = Instruction.operandAt(0)->getIf<Register>();
  const Register *OverflowOperand = Instruction.operandAt(1)->getIf<Register>();
  const Register *LeftOperand = Instruction.operandAt(2)->getIf<Register>();
  const Register *RightOperand = Instruction.operandAt(3)->getIf<Register>();
  if (ResultOperand == nullptr || OverflowOperand == nullptr || LeftOperand == nullptr ||
      RightOperand == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "multiply-overflow lowering requires register operands");
  }
  const Register Result = *ResultOperand;
  const Register Overflow = *OverflowOperand;
  const Register Left = *LeftOperand;
  const Register Right = *RightOperand;
  auto ResultType = MRI.getType(Result);
  if (!ResultType)
    return std::unexpected(ResultType.error());
  if (!ResultType->isCategoryNeutralScalar() || ResultType->scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "multiply-overflow lowering requires a scalar result type");

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  Observer.changingInstr(Instruction);
  if (auto Changed = Instruction.setOpcode(opcode(GenericOpcode::G_MUL)); !Changed)
    return std::unexpected(Changed.error());
  if (auto Changed = Instruction.eraseOperand(1); !Changed)
    return std::unexpected(Changed.error());
  Observer.changedInstr(Instruction);

  auto HighPart =
      buildResultInstruction(MIRBuilder, IsSigned ? GenericOpcode::G_SMULH : GenericOpcode::G_UMULH,
                             *ResultType, {Left, Right});
  if (!HighPart)
    return std::unexpected(HighPart.error());
  auto Zero = buildConstantRegister(MIRBuilder, *ResultType, 0);
  if (!Zero)
    return std::unexpected(Zero.error());

  if (auto Point = MIRBuilder.setInsertPointAfter(Instruction); !Point)
    return std::unexpected(Point.error());
  if (IsSigned) {
    auto ShiftAmount =
        buildConstantRegister(MIRBuilder, *ResultType, ResultType->scalarBitWidth() - 1U);
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());
    auto Shifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, *ResultType,
                                          {Result, *ShiftAmount});
    if (!Shifted)
      return std::unexpected(Shifted.error());
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::NotEqual, Overflow, *HighPart, *Shifted);
        !Built) {
      return std::unexpected(Built.error());
    }
  } else if (auto Built =
                 MIRBuilder.buildICmp(IntegerPredicate::NotEqual, Overflow, *HighPart, *Zero);
             !Built) {
    return std::unexpected(Built.error());
  }
  return {};
}

Expected<void> LegalizerHelper::lowerBitCount(MachineInstr &Instruction) {
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  MachineOperand *DestinationOperand = Instruction.operandAt(0);
  MachineOperand *SourceOperand = Instruction.operandAt(1);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Source = SourceOperand == nullptr ? nullptr : SourceOperand->getIf<Register>();
  if (Destination == nullptr || Source == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "bit-count lowering requires register operands");
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  auto SourceType = MRI.getType(*Source);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!DestinationType->isCategoryNeutralScalar() || !SourceType->isCategoryNeutralScalar())
    return makeUnexpected(ErrorCode::Unsupported,
                          "non-scalar bit-count lowering is not implemented");
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  const auto IsSupported = [this](GenericOpcode Opcode, LowLevelType ResultType,
                                  LowLevelType OperandType) -> Expected<bool> {
    auto Decision = Info.getDecision(LegalizationQuery(opcode(Opcode), {ResultType, OperandType}));
    if (!Decision)
      return std::unexpected(Decision.error());
    return Decision->Action == LegalizationAction::Legal ||
           Decision->Action == LegalizationAction::Libcall ||
           Decision->Action == LegalizationAction::Custom;
  };
  const auto EraseOriginal = [&Instruction]() -> Expected<void> {
    return Instruction.eraseFromParent();
  };

  switch (Generic) {
  case GenericOpcode::G_CTLZ_ZERO_POISON:
  case GenericOpcode::G_CTTZ_ZERO_POISON: {
    const GenericOpcode Replacement = Generic == GenericOpcode::G_CTLZ_ZERO_POISON
                                          ? GenericOpcode::G_CTLZ
                                          : GenericOpcode::G_CTTZ;
    Observer.changingInstr(Instruction);
    if (auto Changed = Instruction.setOpcode(opcode(Replacement)); !Changed)
      return std::unexpected(Changed.error());
    Observer.changedInstr(Instruction);
    return {};
  }

  case GenericOpcode::G_CTLZ: {
    const std::uint32_t Size = SourceType->scalarBitWidth();
    auto PoisonSupported =
        IsSupported(GenericOpcode::G_CTLZ_ZERO_POISON, *DestinationType, *SourceType);
    if (!PoisonSupported)
      return std::unexpected(PoisonSupported.error());
    if (*PoisonSupported) {
      auto PoisonCount = buildResultInstruction(MIRBuilder, GenericOpcode::G_CTLZ_ZERO_POISON,
                                                *DestinationType, {*Source});
      if (!PoisonCount)
        return std::unexpected(PoisonCount.error());
      auto Zero = buildConstantRegister(MIRBuilder, *SourceType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto ConditionType = booleanTypeFor(*SourceType);
      if (!ConditionType)
        return std::unexpected(ConditionType.error());
      auto IsZero =
          buildICmpRegister(MIRBuilder, IntegerPredicate::Equal, *ConditionType, *Source, *Zero);
      if (!IsZero)
        return std::unexpected(IsZero.error());
      auto SizeConstant =
          buildConstantRegister(MIRBuilder, *DestinationType, static_cast<std::int64_t>(Size));
      if (!SizeConstant)
        return std::unexpected(SizeConstant.error());
      if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {*Destination},
                                             {*IsZero, *SizeConstant, *PoisonCount});
          !Built) {
        return std::unexpected(Built.error());
      }
      return EraseOriginal();
    }

    Register Value = *Source;
    const std::uint32_t ExpandedSize = std::bit_ceil(Size);
    for (std::uint32_t Shift = 1; Shift <= ExpandedSize / 2U; Shift <<= 1U) {
      auto ShiftAmount =
          buildConstantRegister(MIRBuilder, *SourceType, static_cast<std::int64_t>(Shift));
      if (!ShiftAmount)
        return std::unexpected(ShiftAmount.error());
      auto Shifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *SourceType,
                                            {Value, *ShiftAmount});
      if (!Shifted)
        return std::unexpected(Shifted.error());
      auto Propagated =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, *SourceType, {Value, *Shifted});
      if (!Propagated)
        return std::unexpected(Propagated.error());
      Value = *Propagated;
    }
    auto Population =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_CTPOP, *DestinationType, {Value});
    if (!Population)
      return std::unexpected(Population.error());
    auto SizeConstant =
        buildConstantRegister(MIRBuilder, *DestinationType, static_cast<std::int64_t>(Size));
    if (!SizeConstant)
      return std::unexpected(SizeConstant.error());
    if (auto Built = MIRBuilder.buildSub(*Destination, *SizeConstant, *Population); !Built)
      return std::unexpected(Built.error());
    return EraseOriginal();
  }

  case GenericOpcode::G_CTTZ: {
    const std::uint32_t Size = SourceType->scalarBitWidth();
    auto PoisonSupported =
        IsSupported(GenericOpcode::G_CTTZ_ZERO_POISON, *DestinationType, *SourceType);
    if (!PoisonSupported)
      return std::unexpected(PoisonSupported.error());
    if (*PoisonSupported) {
      auto PoisonCount = buildResultInstruction(MIRBuilder, GenericOpcode::G_CTTZ_ZERO_POISON,
                                                *DestinationType, {*Source});
      if (!PoisonCount)
        return std::unexpected(PoisonCount.error());
      auto Zero = buildConstantRegister(MIRBuilder, *SourceType, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto ConditionType = booleanTypeFor(*DestinationType);
      if (!ConditionType)
        return std::unexpected(ConditionType.error());
      auto IsZero =
          buildICmpRegister(MIRBuilder, IntegerPredicate::Equal, *ConditionType, *Source, *Zero);
      if (!IsZero)
        return std::unexpected(IsZero.error());
      auto SizeConstant =
          buildConstantRegister(MIRBuilder, *DestinationType, static_cast<std::int64_t>(Size));
      if (!SizeConstant)
        return std::unexpected(SizeConstant.error());
      if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {*Destination},
                                             {*IsZero, *SizeConstant, *PoisonCount});
          !Built) {
        return std::unexpected(Built.error());
      }
      return EraseOriginal();
    }

    auto MinusOne = buildConstantRegister(MIRBuilder, *SourceType, -1);
    if (!MinusOne)
      return std::unexpected(MinusOne.error());
    auto Inverted =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_XOR, *SourceType, {*Source, *MinusOne});
    if (!Inverted)
      return std::unexpected(Inverted.error());
    auto Decremented =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, *SourceType, {*Source, *MinusOne});
    if (!Decremented)
      return std::unexpected(Decremented.error());
    auto Masked = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *SourceType,
                                         {*Inverted, *Decremented});
    if (!Masked)
      return std::unexpected(Masked.error());

    auto PopulationSupported = IsSupported(GenericOpcode::G_CTPOP, *SourceType, *SourceType);
    if (!PopulationSupported)
      return std::unexpected(PopulationSupported.error());
    auto LeadingZeroSupported = IsSupported(GenericOpcode::G_CTLZ, *SourceType, *SourceType);
    if (!LeadingZeroSupported)
      return std::unexpected(LeadingZeroSupported.error());
    if (!*PopulationSupported && *LeadingZeroSupported) {
      auto SizeConstant =
          buildConstantRegister(MIRBuilder, *SourceType, static_cast<std::int64_t>(Size));
      if (!SizeConstant)
        return std::unexpected(SizeConstant.error());
      auto LeadingZeros =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_CTLZ, *SourceType, {*Masked});
      if (!LeadingZeros)
        return std::unexpected(LeadingZeros.error());
      if (auto Built = MIRBuilder.buildSub(*Destination, *SizeConstant, *LeadingZeros); !Built)
        return std::unexpected(Built.error());
      return EraseOriginal();
    }

    Observer.changingInstr(Instruction);
    if (auto Changed = Instruction.setOpcode(opcode(GenericOpcode::G_CTPOP)); !Changed)
      return std::unexpected(Changed.error());
    if (auto Changed = SourceOperand->setRegister(*Masked); !Changed)
      return std::unexpected(Changed.error());
    Observer.changedInstr(Instruction);
    return {};
  }

  case GenericOpcode::G_CTPOP: {
    const std::uint32_t Size = SourceType->scalarBitWidth();
    if (Size > 128 || Size % 8U != 0)
      return makeUnexpected(
          ErrorCode::Unsupported,
          "population-count lowering requires a byte-sized scalar up to 128 bits");

    auto One = buildConstantRegister(MIRBuilder, *SourceType, 1);
    if (!One)
      return std::unexpected(One.error());
    auto ShiftedByOne =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *SourceType, {*Source, *One});
    if (!ShiftedByOne)
      return std::unexpected(ShiftedByOne.error());
    auto PairMaskValue = repeatedByte(Size, 0x55);
    if (!PairMaskValue)
      return std::unexpected(PairMaskValue.error());
    auto PairMask = buildConstantRegister(MIRBuilder, *SourceType, std::move(*PairMaskValue));
    if (!PairMask)
      return std::unexpected(PairMask.error());
    auto HighPairBits = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *SourceType,
                                               {*ShiftedByOne, *PairMask});
    if (!HighPairBits)
      return std::unexpected(HighPairBits.error());
    auto PairCounts = buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *SourceType,
                                             {*Source, *HighPairBits});
    if (!PairCounts)
      return std::unexpected(PairCounts.error());

    auto Two = buildConstantRegister(MIRBuilder, *SourceType, 2);
    if (!Two)
      return std::unexpected(Two.error());
    auto ShiftedPairs =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *SourceType, {*PairCounts, *Two});
    if (!ShiftedPairs)
      return std::unexpected(ShiftedPairs.error());
    auto NibbleMaskValue = repeatedByte(Size, 0x33);
    if (!NibbleMaskValue)
      return std::unexpected(NibbleMaskValue.error());
    auto NibbleMask = buildConstantRegister(MIRBuilder, *SourceType, std::move(*NibbleMaskValue));
    if (!NibbleMask)
      return std::unexpected(NibbleMask.error());
    auto HighNibbleCounts = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *SourceType,
                                                   {*ShiftedPairs, *NibbleMask});
    if (!HighNibbleCounts)
      return std::unexpected(HighNibbleCounts.error());
    auto LowNibbleCounts = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *SourceType,
                                                  {*PairCounts, *NibbleMask});
    if (!LowNibbleCounts)
      return std::unexpected(LowNibbleCounts.error());
    auto NibbleCounts = buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, *SourceType,
                                               {*HighNibbleCounts, *LowNibbleCounts});
    if (!NibbleCounts)
      return std::unexpected(NibbleCounts.error());

    auto Four = buildConstantRegister(MIRBuilder, *SourceType, 4);
    if (!Four)
      return std::unexpected(Four.error());
    auto ShiftedNibbles = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *SourceType,
                                                 {*NibbleCounts, *Four});
    if (!ShiftedNibbles)
      return std::unexpected(ShiftedNibbles.error());
    auto DirtyByteCounts = buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, *SourceType,
                                                  {*ShiftedNibbles, *NibbleCounts});
    if (!DirtyByteCounts)
      return std::unexpected(DirtyByteCounts.error());
    auto ByteMaskValue = repeatedByte(Size, 0x0f);
    if (!ByteMaskValue)
      return std::unexpected(ByteMaskValue.error());
    auto ByteMask = buildConstantRegister(MIRBuilder, *SourceType, std::move(*ByteMaskValue));
    if (!ByteMask)
      return std::unexpected(ByteMask.error());
    auto ByteCounts = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *SourceType,
                                             {*DirtyByteCounts, *ByteMask});
    if (!ByteCounts)
      return std::unexpected(ByteCounts.error());

    if (Size == 16) {
      auto Eight = buildConstantRegister(MIRBuilder, *SourceType, 8);
      if (!Eight)
        return std::unexpected(Eight.error());
      auto HighByteCount = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *SourceType,
                                                  {*ByteCounts, *Eight});
      if (!HighByteCount)
        return std::unexpected(HighByteCount.error());
      auto Sum = buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, *SourceType,
                                        {*ByteCounts, *HighByteCount});
      if (!Sum)
        return std::unexpected(Sum.error());
      auto ResultMaskValue = APInt::fromUnsigned(Size, 0xff);
      if (!ResultMaskValue)
        return std::unexpected(ResultMaskValue.error());
      auto ResultMask = buildConstantRegister(MIRBuilder, *SourceType, std::move(*ResultMaskValue));
      if (!ResultMask)
        return std::unexpected(ResultMask.error());
      if (auto Built = MIRBuilder.buildAnd(*Destination, *Sum, *ResultMask); !Built)
        return std::unexpected(Built.error());
      return EraseOriginal();
    }

    auto MultiplyMaskValue = repeatedByte(Size, 0x01);
    if (!MultiplyMaskValue)
      return std::unexpected(MultiplyMaskValue.error());
    auto MultiplyMask =
        buildConstantRegister(MIRBuilder, *SourceType, std::move(*MultiplyMaskValue));
    if (!MultiplyMask)
      return std::unexpected(MultiplyMask.error());
    auto FinalShift =
        buildConstantRegister(MIRBuilder, *SourceType, static_cast<std::int64_t>(Size - 8U));
    if (!FinalShift)
      return std::unexpected(FinalShift.error());

    auto MultiplyDecision =
        Info.getDecision(LegalizationQuery(opcode(GenericOpcode::G_MUL), {*SourceType}));
    if (!MultiplyDecision)
      return std::unexpected(MultiplyDecision.error());
    const bool IsMultiplySupported = MultiplyDecision->Action == LegalizationAction::Legal ||
                                     MultiplyDecision->Action == LegalizationAction::WidenScalar ||
                                     MultiplyDecision->Action == LegalizationAction::Custom;
    if (IsMultiplySupported) {
      auto Product = buildResultInstruction(MIRBuilder, GenericOpcode::G_MUL, *SourceType,
                                            {*ByteCounts, *MultiplyMask});
      if (!Product)
        return std::unexpected(Product.error());
      if (auto Built = MIRBuilder.buildLShr(*Destination, *Product, *FinalShift); !Built)
        return std::unexpected(Built.error());
    } else {
      Register Sum = *ByteCounts;
      for (std::uint32_t Shift = 8; Shift < Size; Shift *= 2U) {
        auto ShiftAmount =
            buildConstantRegister(MIRBuilder, *SourceType, static_cast<std::int64_t>(Shift));
        if (!ShiftAmount)
          return std::unexpected(ShiftAmount.error());
        auto Shifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *SourceType,
                                              {Sum, *ShiftAmount});
        if (!Shifted)
          return std::unexpected(Shifted.error());
        auto NewSum =
            buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, *SourceType, {Sum, *Shifted});
        if (!NewSum)
          return std::unexpected(NewSum.error());
        Sum = *NewSum;
      }
      if (auto Built = MIRBuilder.buildLShr(*Destination, Sum, *FinalShift); !Built)
        return std::unexpected(Built.error());
    }
    return EraseOriginal();
  }

  case GenericOpcode::G_CTLS: {
    auto SignIndex = buildConstantRegister(
        MIRBuilder, *SourceType, static_cast<std::int64_t>(SourceType->scalarBitWidth() - 1U));
    if (!SignIndex)
      return std::unexpected(SignIndex.error());
    auto One = buildConstantRegister(MIRBuilder, *DestinationType, 1);
    if (!One)
      return std::unexpected(One.error());
    auto Sign = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, *SourceType,
                                       {*Source, *SignIndex});
    if (!Sign)
      return std::unexpected(Sign.error());
    auto DifferentBits =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_XOR, *SourceType, {*Source, *Sign});
    if (!DifferentBits)
      return std::unexpected(DifferentBits.error());
    auto LeadingZeros = buildResultInstruction(MIRBuilder, GenericOpcode::G_CTLZ, *DestinationType,
                                               {*DifferentBits});
    if (!LeadingZeros)
      return std::unexpected(LeadingZeros.error());
    if (auto Built = MIRBuilder.buildSub(*Destination, *LeadingZeros, *One); !Built)
      return std::unexpected(Built.error());
    return EraseOriginal();
  }

  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "bit-count lowering received an unexpected opcode");
  }
}

Expected<void> LegalizerHelper::lowerBswap(MachineInstr &Instruction) {
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Source = *Instruction.operandAt(1)->getIf<Register>();
  auto Type = MRI.getType(Source);
  if (!Type)
    return std::unexpected(Type.error());

  const std::uint32_t SizeInBytes = (Type->scalarBitWidth() + 7U) / 8U;
  const std::uint32_t BaseShiftAmount = (SizeInBytes - 1U) * 8U;
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto ShiftAmount = buildConstantRegister(MIRBuilder, *Type, BaseShiftAmount);
  if (!ShiftAmount)
    return std::unexpected(ShiftAmount.error());
  auto LowByteShiftedLeft =
      buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *Type, {Source, *ShiftAmount});
  auto HighByteShiftedRight =
      buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *Type, {Source, *ShiftAmount});
  if (!LowByteShiftedLeft)
    return std::unexpected(LowByteShiftedLeft.error());
  if (!HighByteShiftedRight)
    return std::unexpected(HighByteShiftedRight.error());

  Register Result;
  if (SizeInBytes == 2U) {
    if (auto Built = MIRBuilder.buildOr(Destination, *HighByteShiftedRight, *LowByteShiftedLeft);
        !Built) {
      return std::unexpected(Built.error());
    }
    Result = Destination;
  } else {
    auto OuterBytes = buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, *Type,
                                             {*HighByteShiftedRight, *LowByteShiftedLeft});
    if (!OuterBytes)
      return std::unexpected(OuterBytes.error());
    Result = *OuterBytes;
  }

  for (std::uint32_t Index = 1; Index < SizeInBytes / 2U; ++Index) {
    const std::uint32_t MaskBits = std::uint32_t{0xff} << (Index * 8U);
    const std::int64_t MaskValue = static_cast<std::int32_t>(MaskBits);
    auto Mask = buildConstantRegister(MIRBuilder, *Type, MaskValue);
    auto InnerShiftAmount = buildConstantRegister(MIRBuilder, *Type, BaseShiftAmount - 16U * Index);
    if (!Mask)
      return std::unexpected(Mask.error());
    if (!InnerShiftAmount)
      return std::unexpected(InnerShiftAmount.error());

    auto LowByte = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *Type, {Source, *Mask});
    if (!LowByte)
      return std::unexpected(LowByte.error());
    auto LowByteShifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *Type,
                                                 {*LowByte, *InnerShiftAmount});
    if (!LowByteShifted)
      return std::unexpected(LowByteShifted.error());
    auto WithLowByte =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, *Type, {Result, *LowByteShifted});
    if (!WithLowByte)
      return std::unexpected(WithLowByte.error());

    auto SourceShifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *Type,
                                                {Source, *InnerShiftAmount});
    if (!SourceShifted)
      return std::unexpected(SourceShifted.error());
    auto HighByte =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *Type, {*SourceShifted, *Mask});
    if (!HighByte)
      return std::unexpected(HighByte.error());

    if (Index + 1U == SizeInBytes / 2U) {
      if (auto Built = MIRBuilder.buildOr(Destination, *WithLowByte, *HighByte); !Built)
        return std::unexpected(Built.error());
      Result = Destination;
    } else {
      auto Combined =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_OR, *Type, {*WithLowByte, *HighByte});
      if (!Combined)
        return std::unexpected(Combined.error());
      Result = *Combined;
    }
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerFunnelShift(MachineInstr &Instruction) {
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Left = *Instruction.operandAt(1)->getIf<Register>();
  const Register Right = *Instruction.operandAt(2)->getIf<Register>();
  const Register Amount = *Instruction.operandAt(3)->getIf<Register>();
  auto DestinationType = MRI.getType(Destination);
  auto AmountType = MRI.getType(Amount);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!AmountType)
    return std::unexpected(AmountType.error());

  const std::uint32_t ElementWidth = DestinationType->scalarBitWidth();
  const bool IsLeft = Instruction.opcode() == opcode(GenericOpcode::G_FSHL);
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  auto Mask =
      buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(ElementWidth - 1U));
  if (!Mask)
    return std::unexpected(Mask.error());

  Register ShiftAmount;
  Register InverseShiftAmount;
  if (std::has_single_bit(ElementWidth)) {
    auto Shift =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *AmountType, {Amount, *Mask});
    auto AllOnes = buildConstantRegister(MIRBuilder, *AmountType, -1);
    if (!Shift)
      return std::unexpected(Shift.error());
    if (!AllOnes)
      return std::unexpected(AllOnes.error());
    auto Inverted =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_XOR, *AmountType, {Amount, *AllOnes});
    if (!Inverted)
      return std::unexpected(Inverted.error());
    auto Inverse =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *AmountType, {*Inverted, *Mask});
    if (!Inverse)
      return std::unexpected(Inverse.error());
    ShiftAmount = *Shift;
    InverseShiftAmount = *Inverse;
  } else {
    auto Width =
        buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(ElementWidth));
    if (!Width)
      return std::unexpected(Width.error());
    auto Shift =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_UREM, *AmountType, {Amount, *Width});
    if (!Shift)
      return std::unexpected(Shift.error());
    auto Inverse =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *AmountType, {*Mask, *Shift});
    if (!Inverse)
      return std::unexpected(Inverse.error());
    ShiftAmount = *Shift;
    InverseShiftAmount = *Inverse;
  }

  auto One = buildConstantRegister(MIRBuilder, *AmountType, 1);
  if (!One)
    return std::unexpected(One.error());

  Register ShiftedLeft;
  Register ShiftedRight;
  if (IsLeft) {
    auto ShiftLeft = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *DestinationType,
                                            {Left, ShiftAmount});
    if (!ShiftLeft)
      return std::unexpected(ShiftLeft.error());
    auto FirstShiftRight =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *DestinationType, {Right, *One});
    if (!FirstShiftRight)
      return std::unexpected(FirstShiftRight.error());
    auto ShiftRight = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *DestinationType,
                                             {*FirstShiftRight, InverseShiftAmount});
    if (!ShiftRight)
      return std::unexpected(ShiftRight.error());
    ShiftedLeft = *ShiftLeft;
    ShiftedRight = *ShiftRight;
  } else {
    auto FirstShiftLeft =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *DestinationType, {Left, *One});
    if (!FirstShiftLeft)
      return std::unexpected(FirstShiftLeft.error());
    auto ShiftLeft = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *DestinationType,
                                            {*FirstShiftLeft, InverseShiftAmount});
    if (!ShiftLeft)
      return std::unexpected(ShiftLeft.error());
    auto ShiftRight = buildResultInstruction(MIRBuilder, GenericOpcode::G_LSHR, *DestinationType,
                                             {Right, ShiftAmount});
    if (!ShiftRight)
      return std::unexpected(ShiftRight.error());
    ShiftedLeft = *ShiftLeft;
    ShiftedRight = *ShiftRight;
  }

  auto Result = MIRBuilder.buildInstr(opcode(GenericOpcode::G_OR));
  if (!Result)
    return std::unexpected(Result.error());
  if (auto Added = Result->addDef(Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Result->addUse(ShiftedLeft); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Result->addUse(ShiftedRight); !Added)
    return std::unexpected(Added.error());
  if (auto Flags = Result->setFlags({MachineInstrFlag::Disjoint}); !Flags)
    return std::unexpected(Flags.error());
  if (auto Built = Result->build(); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerRotate(MachineInstr &Instruction) {
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Source = *Instruction.operandAt(1)->getIf<Register>();
  const Register Amount = *Instruction.operandAt(2)->getIf<Register>();
  auto DestinationType = MRI.getType(Destination);
  auto AmountType = MRI.getType(Amount);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!AmountType)
    return std::unexpected(AmountType.error());

  const std::uint32_t ElementWidth = DestinationType->scalarBitWidth();
  const bool IsLeft = Instruction.opcode() == opcode(GenericOpcode::G_ROTL);
  const GenericOpcode ShiftOpcode = IsLeft ? GenericOpcode::G_SHL : GenericOpcode::G_LSHR;
  const GenericOpcode ReverseShiftOpcode = IsLeft ? GenericOpcode::G_LSHR : GenericOpcode::G_SHL;

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Zero = buildConstantRegister(MIRBuilder, *AmountType, 0);
  auto WidthMinusOne =
      buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(ElementWidth - 1U));
  if (!Zero)
    return std::unexpected(Zero.error());
  if (!WidthMinusOne)
    return std::unexpected(WidthMinusOne.error());

  Register Shifted;
  Register ReverseShifted;
  if (std::has_single_bit(ElementWidth)) {
    auto NegativeAmount =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *AmountType, {*Zero, Amount});
    auto ShiftAmount = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *AmountType,
                                              {Amount, *WidthMinusOne});
    if (!NegativeAmount)
      return std::unexpected(NegativeAmount.error());
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());
    auto Shift =
        buildResultInstruction(MIRBuilder, ShiftOpcode, *DestinationType, {Source, *ShiftAmount});
    auto ReverseAmount = buildResultInstruction(MIRBuilder, GenericOpcode::G_AND, *AmountType,
                                                {*NegativeAmount, *WidthMinusOne});
    if (!Shift)
      return std::unexpected(Shift.error());
    if (!ReverseAmount)
      return std::unexpected(ReverseAmount.error());
    auto ReverseShift = buildResultInstruction(MIRBuilder, ReverseShiftOpcode, *DestinationType,
                                               {Source, *ReverseAmount});
    if (!ReverseShift)
      return std::unexpected(ReverseShift.error());
    Shifted = *Shift;
    ReverseShifted = *ReverseShift;
  } else {
    auto Width =
        buildConstantRegister(MIRBuilder, *AmountType, static_cast<std::int64_t>(ElementWidth));
    if (!Width)
      return std::unexpected(Width.error());
    auto ShiftAmount =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_UREM, *AmountType, {Amount, *Width});
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());
    auto Shift =
        buildResultInstruction(MIRBuilder, ShiftOpcode, *DestinationType, {Source, *ShiftAmount});
    auto ReverseAmount = buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *AmountType,
                                                {*WidthMinusOne, *ShiftAmount});
    auto One = buildConstantRegister(MIRBuilder, *AmountType, 1);
    if (!Shift)
      return std::unexpected(Shift.error());
    if (!ReverseAmount)
      return std::unexpected(ReverseAmount.error());
    if (!One)
      return std::unexpected(One.error());
    auto Inner =
        buildResultInstruction(MIRBuilder, ReverseShiftOpcode, *DestinationType, {Source, *One});
    if (!Inner)
      return std::unexpected(Inner.error());
    auto ReverseShift = buildResultInstruction(MIRBuilder, ReverseShiftOpcode, *DestinationType,
                                               {*Inner, *ReverseAmount});
    if (!ReverseShift)
      return std::unexpected(ReverseShift.error());
    Shifted = *Shift;
    ReverseShifted = *ReverseShift;
  }

  auto Result = MIRBuilder.buildInstr(opcode(GenericOpcode::G_OR));
  if (!Result)
    return std::unexpected(Result.error());
  if (auto Added = Result->addDef(Destination); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Result->addUse(Shifted); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Result->addUse(ReverseShifted); !Added)
    return std::unexpected(Added.error());
  if (auto Flags = Result->setFlags({MachineInstrFlag::Disjoint}); !Flags)
    return std::unexpected(Flags.error());
  if (auto Built = Result->build(); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::libcall(MachineInstr &Instruction) {
  if (Libcalls == nullptr || Libcalls->CallLowering == nullptr) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "libcall legalization requires target call lowering information");
  }

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  auto *DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || !DestinationOperand->isDef()) {
    return makeUnexpected(ErrorCode::VerificationError, "libcall requires a register destination");
  }
  auto DestinationType = MRI.getType(*Destination);
  if (!DestinationType)
    return std::unexpected(DestinationType.error());
  if (!DestinationType->isCategoryNeutralScalar()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "libcall requires a category-neutral scalar destination");
  }

  const bool IsConversion =
      Generic == GenericOpcode::G_FPEXT || Generic == GenericOpcode::G_FPTRUNC ||
      Generic == GenericOpcode::G_FPTOSI || Generic == GenericOpcode::G_FPTOUI ||
      Generic == GenericOpcode::G_SITOFP || Generic == GenericOpcode::G_UITOFP;
  const bool IsFloatingBinary = Generic == GenericOpcode::G_FREM;
  if (Instruction.operandCount() < 2 || (IsConversion && Instruction.operandCount() != 2)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "libcall has an invalid number of operands");
  }

  const MachineOperand *FirstSourceOperand = Instruction.operandAt(1);
  const Register *FirstSource =
      FirstSourceOperand == nullptr ? nullptr : FirstSourceOperand->getIf<Register>();
  if (FirstSource == nullptr || !FirstSourceOperand->isUse()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "libcall requires register source operands");
  }
  auto SourceType = MRI.getType(*FirstSource);
  if (!SourceType)
    return std::unexpected(SourceType.error());
  if (!SourceType->isCategoryNeutralScalar()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "libcall requires category-neutral scalar sources");
  }

  Expected<std::string> Name =
      IsConversion       ? conversionLibcallName(Generic, DestinationType->scalarBitWidth(),
                                                 SourceType->scalarBitWidth())
      : IsFloatingBinary ? floatingBinaryLibcallName(Generic, DestinationType->scalarBitWidth())
                         : integerLibcallName(Generic, DestinationType->scalarBitWidth());
  if (!Name)
    return std::unexpected(Name.error());

  const bool DestinationIsFloat =
      Generic == GenericOpcode::G_FPEXT || Generic == GenericOpcode::G_FPTRUNC ||
      Generic == GenericOpcode::G_SITOFP || Generic == GenericOpcode::G_UITOFP || IsFloatingBinary;
  const bool SourceIsFloat =
      Generic == GenericOpcode::G_FPEXT || Generic == GenericOpcode::G_FPTRUNC ||
      Generic == GenericOpcode::G_FPTOSI || Generic == GenericOpcode::G_FPTOUI || IsFloatingBinary;
  const ABIValueCategory DestinationCategory =
      DestinationIsFloat ? ABIValueCategory::FloatingPoint : ABIValueCategory::Integer;
  const ABIValueCategory SourceCategory =
      SourceIsFloat ? ABIValueCategory::FloatingPoint : ABIValueCategory::Integer;
  auto ExtensionFor = [&](ABIValueCategory Category, bool IsResult,
                          std::uint32_t BitWidth) noexcept {
    if (Category != ABIValueCategory::Integer || BitWidth >= 64)
      return ABIExtensionKind::None;
    if (!IsConversion)
      return integerLibcallExtension(Generic, BitWidth);
    if ((IsResult && Generic == GenericOpcode::G_FPTOSI) ||
        (!IsResult && Generic == GenericOpcode::G_SITOFP)) {
      return ABIExtensionKind::SignExtend;
    }
    return ABIExtensionKind::ZeroExtend;
  };

  auto FragmentType = LowLevelType::makeCategoryNeutralScalar(64);
  if (!FragmentType)
    return std::unexpected(FragmentType.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  auto AppendValue = [&](Register Value, LowLevelType Type, ABIValueCategory Category,
                         ABIExtensionKind Extension, std::uint32_t OriginalValue, bool IsResult,
                         std::vector<ABIValue> &Values,
                         std::vector<Register> *Fragments) -> Expected<void> {
    const std::uint32_t BitWidth = Type.scalarBitWidth();
    auto OriginalAlignment = Alignment::create((BitWidth + 7U) / 8U);
    if (!OriginalAlignment)
      return std::unexpected(OriginalAlignment.error());
    if (BitWidth != 128) {
      auto ABI = libcallABIValue(Value, Type, Category, OriginalValue, 0, 1, 0, *OriginalAlignment,
                                 Extension);
      if (!ABI)
        return std::unexpected(ABI.error());
      Values.push_back(*ABI);
      if (Fragments != nullptr)
        Fragments->push_back(Value);
      return {};
    }

    std::vector<Register> PartRegisters;
    PartRegisters.reserve(2);
    for (std::size_t Index = 0; Index != 2; ++Index) {
      auto Part = MRI.createVirtualRegister(*FragmentType);
      if (!Part)
        return std::unexpected(Part.error());
      PartRegisters.push_back(*Part);
    }
    if (!IsResult) {
      auto Unmerged = MIRBuilder.buildUnmergeValues(PartRegisters, Value);
      if (!Unmerged)
        return std::unexpected(Unmerged.error());
    }
    for (std::uint16_t Index = 0; Index != 2; ++Index) {
      auto ABI = libcallABIValue(PartRegisters[Index], *FragmentType, ABIValueCategory::Integer,
                                 OriginalValue, Index, 2, static_cast<std::uint32_t>(Index) * 8U,
                                 *OriginalAlignment, ABIExtensionKind::None);
      if (!ABI)
        return std::unexpected(ABI.error());
      Values.push_back(*ABI);
    }
    if (Fragments != nullptr)
      *Fragments = std::move(PartRegisters);
    return {};
  };

  std::vector<ABIValue> Arguments;
  Arguments.reserve((Instruction.operandCount() - 1U) *
                    (SourceType->scalarBitWidth() == 128 ? 2U : 1U));
  std::uint32_t OriginalArgument = 0;
  for (std::size_t Index = 1; Index != Instruction.operandCount(); ++Index) {
    const MachineOperand *Operand = Instruction.operandAt(Index);
    const Register *Source = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Source == nullptr || !Operand->isUse()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "libcall requires register source operands");
    }
    auto ArgumentType = MRI.getType(*Source);
    if (!ArgumentType)
      return std::unexpected(ArgumentType.error());
    if (*ArgumentType != *SourceType) {
      return makeUnexpected(ErrorCode::VerificationError, "libcall source types must match");
    }
    if (!IsConversion && *ArgumentType != *DestinationType) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "binary libcall source and destination types must match");
    }
    const ABIExtensionKind Extension =
        ExtensionFor(SourceCategory, false, ArgumentType->scalarBitWidth());
    if (auto Appended = AppendValue(*Source, *ArgumentType, SourceCategory, Extension,
                                    OriginalArgument++, false, Arguments, nullptr);
        !Appended) {
      return std::unexpected(Appended.error());
    }
  }

  std::vector<ABIValue> Results;
  std::vector<Register> ResultFragments;
  const ABIExtensionKind ResultExtension =
      ExtensionFor(DestinationCategory, true, DestinationType->scalarBitWidth());
  if (auto Appended = AppendValue(*Destination, *DestinationType, DestinationCategory,
                                  ResultExtension, 0, true, Results, &ResultFragments);
      !Appended) {
    return std::unexpected(Appended.error());
  }

  auto Callee = MachineCallTarget::externalSymbol(std::move(*Name));
  if (!Callee)
    return std::unexpected(Callee.error());
  CallLoweringRequest Request;
  Request.Convention = Libcalls->Convention;
  Request.Callee = std::move(*Callee);
  Request.Arguments = std::move(Arguments);
  Request.Results = std::move(Results);
  Request.FixedArgumentCount = OriginalArgument;
  if (auto Lowered = Libcalls->CallLowering->lowerCall(MIRBuilder, Request); !Lowered)
    return std::unexpected(Lowered.error());

  if (DestinationType->scalarBitWidth() == 128) {
    auto Merged = MIRBuilder.buildMergeLikeInstr(*Destination, ResultFragments);
    if (!Merged)
      return std::unexpected(Merged.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerSignedAddSubOverflow(MachineInstr &Instruction) {
  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  if (Generic == GenericOpcode::G_SADDE || Generic == GenericOpcode::G_SSUBE) {
    if (Instruction.operandCount() != 5)
      return makeUnexpected(ErrorCode::VerificationError,
                            "signed extended add/sub lowering requires five register operands");

    const bool IsAdd = Generic == GenericOpcode::G_SADDE;
    const MachineOperand *DestinationOperand = Instruction.operandAt(0);
    const MachineOperand *OverflowOperand = Instruction.operandAt(1);
    const MachineOperand *LeftOperand = Instruction.operandAt(2);
    const MachineOperand *RightOperand = Instruction.operandAt(3);
    const MachineOperand *CarryInOperand = Instruction.operandAt(4);
    const Register *Destination =
        DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
    const Register *Overflow =
        OverflowOperand == nullptr ? nullptr : OverflowOperand->getIf<Register>();
    const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
    const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
    const Register *CarryIn =
        CarryInOperand == nullptr ? nullptr : CarryInOperand->getIf<Register>();
    if (Destination == nullptr || Overflow == nullptr || Left == nullptr || Right == nullptr ||
        CarryIn == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "signed extended add/sub lowering requires register operands");
    }

    auto Type = MRI.getType(*Destination);
    if (!Type)
      return std::unexpected(Type.error());
    auto Result = MRI.cloneVirtualRegister(*Destination);
    auto ExtendedCarry = MRI.createVirtualRegister(*Type);
    auto FirstXor = MRI.createVirtualRegister(*Type);
    auto SecondXor = MRI.createVirtualRegister(*Type);
    auto OverflowBits = MRI.createVirtualRegister(*Type);
    if (!Result)
      return std::unexpected(Result.error());
    if (!ExtendedCarry)
      return std::unexpected(ExtendedCarry.error());
    if (!FirstXor)
      return std::unexpected(FirstXor.error());
    if (!SecondXor)
      return std::unexpected(SecondXor.error());
    if (!OverflowBits)
      return std::unexpected(OverflowBits.error());

    if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
      return std::unexpected(Point.error());

    if (IsAdd) {
      auto Temporary = MRI.createVirtualRegister(*Type);
      if (!Temporary)
        return std::unexpected(Temporary.error());
      if (auto Built = MIRBuilder.buildAdd(*Temporary, *Left, *Right); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildZExt(*ExtendedCarry, *CarryIn); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildAdd(*Result, *Temporary, *ExtendedCarry); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildCopy(*Destination, *Result); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildXor(*FirstXor, *Result, *Left); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildXor(*SecondXor, *Result, *Right); !Built)
        return std::unexpected(Built.error());
    } else {
      auto RightWithCarry = MRI.createVirtualRegister(*Type);
      if (!RightWithCarry)
        return std::unexpected(RightWithCarry.error());
      if (auto Built = MIRBuilder.buildZExt(*ExtendedCarry, *CarryIn); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildAdd(*RightWithCarry, *Right, *ExtendedCarry); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildSub(*Result, *Left, *RightWithCarry); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildCopy(*Destination, *Result); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildXor(*FirstXor, *Left, *Right); !Built)
        return std::unexpected(Built.error());
      if (auto Built = MIRBuilder.buildXor(*SecondXor, *Left, *Result); !Built)
        return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildAnd(*OverflowBits, *FirstXor, *SecondXor); !Built)
      return std::unexpected(Built.error());
    auto Zero = buildConstantRegister(MIRBuilder, *Type, 0);
    if (!Zero)
      return std::unexpected(Zero.error());
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::SignedLessThan, *Overflow, *OverflowBits, *Zero);
        !Built) {
      return std::unexpected(Built.error());
    }

    return Instruction.eraseFromParent();
  }

  const bool IsAdd = Instruction.opcode() == opcode(GenericOpcode::G_SADDO);
  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Overflow = *Instruction.operandAt(1)->getIf<Register>();
  const Register Left = *Instruction.operandAt(2)->getIf<Register>();
  const Register Right = *Instruction.operandAt(3)->getIf<Register>();
  auto Type = MRI.getType(Destination);
  auto BooleanType = MRI.getType(Overflow);
  if (!Type)
    return std::unexpected(Type.error());
  if (!BooleanType)
    return std::unexpected(BooleanType.error());

  auto Result = MRI.cloneVirtualRegister(Destination);
  auto FirstComparison = MRI.createVirtualRegister(*BooleanType);
  auto SecondComparison = MRI.createVirtualRegister(*BooleanType);
  if (!Result)
    return std::unexpected(Result.error());
  if (!FirstComparison)
    return std::unexpected(FirstComparison.error());
  if (!SecondComparison)
    return std::unexpected(SecondComparison.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built = IsAdd ? MIRBuilder.buildAdd(*Result, Left, Right)
                         : MIRBuilder.buildSub(*Result, Left, Right);
      !Built) {
    return std::unexpected(Built.error());
  }
  auto Zero = buildConstantRegister(MIRBuilder, *Type, 0);
  if (!Zero)
    return std::unexpected(Zero.error());
  if (IsAdd) {
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::SignedLessThan, *FirstComparison, *Result, Left);
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::SignedLessThan, *SecondComparison, Right, *Zero);
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildXor(Overflow, *SecondComparison, *FirstComparison); !Built)
      return std::unexpected(Built.error());
  } else {
    if (auto Built =
            MIRBuilder.buildICmp(IntegerPredicate::SignedLessThan, *FirstComparison, Left, Right);
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildICmp(IntegerPredicate::SignedLessThan, *SecondComparison,
                                          *Result, *Zero);
        !Built) {
      return std::unexpected(Built.error());
    }
    if (auto Built = MIRBuilder.buildXor(Overflow, *FirstComparison, *SecondComparison); !Built)
      return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildCopy(Destination, *Result); !Built)
    return std::unexpected(Built.error());

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerSaturatingAddSubToMinMax(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "saturating add/sub min/max lowering requires three register operands");

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  const bool IsSigned = Generic == GenericOpcode::G_SADDSAT || Generic == GenericOpcode::G_SSUBSAT;
  const bool IsAdd = Generic == GenericOpcode::G_UADDSAT || Generic == GenericOpcode::G_SADDSAT;
  const GenericOpcode ArithmeticOpcode = IsAdd ? GenericOpcode::G_ADD : GenericOpcode::G_SUB;
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(1);
  const MachineOperand *RightOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (Destination == nullptr || Left == nullptr || Right == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "saturating add/sub min/max lowering requires register operands");

  auto Type = MRI.getType(*Destination);
  if (!Type)
    return std::unexpected(Type.error());
  if (!Type->isCategoryNeutralScalar() || Type->scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "saturating add/sub min/max lowering requires a scalar integer type");
  const std::uint32_t BitWidth = Type->scalarBitWidth();

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (IsSigned) {
    auto MaximumValue = APInt::getLowBitsSet(BitWidth, BitWidth - 1U);
    auto MinimumValue = oneBitSet(BitWidth, BitWidth - 1U);
    if (!MaximumValue)
      return std::unexpected(MaximumValue.error());
    if (!MinimumValue)
      return std::unexpected(MinimumValue.error());
    auto Maximum = buildConstantRegister(MIRBuilder, *Type, std::move(*MaximumValue));
    auto Minimum = buildConstantRegister(MIRBuilder, *Type, std::move(*MinimumValue));
    if (!Maximum)
      return std::unexpected(Maximum.error());
    if (!Minimum)
      return std::unexpected(Minimum.error());

    Register LowerBound;
    Register UpperBound;
    if (IsAdd) {
      auto Zero = buildConstantRegister(MIRBuilder, *Type, 0);
      if (!Zero)
        return std::unexpected(Zero.error());
      auto LeftMaximum =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SMAX, *Type, {*Left, *Zero});
      if (!LeftMaximum)
        return std::unexpected(LeftMaximum.error());
      auto High =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *Type, {*Maximum, *LeftMaximum});
      if (!High)
        return std::unexpected(High.error());
      auto LeftMinimum =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SMIN, *Type, {*Left, *Zero});
      if (!LeftMinimum)
        return std::unexpected(LeftMinimum.error());
      auto Low =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *Type, {*Minimum, *LeftMinimum});
      if (!Low)
        return std::unexpected(Low.error());
      LowerBound = *Low;
      UpperBound = *High;
    } else {
      auto NegativeOne = buildConstantRegister(MIRBuilder, *Type, -1);
      if (!NegativeOne)
        return std::unexpected(NegativeOne.error());
      auto LeftMaximum =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SMAX, *Type, {*Left, *NegativeOne});
      if (!LeftMaximum)
        return std::unexpected(LeftMaximum.error());
      auto Low =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *Type, {*LeftMaximum, *Maximum});
      if (!Low)
        return std::unexpected(Low.error());
      auto LeftMinimum =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SMIN, *Type, {*Left, *NegativeOne});
      if (!LeftMinimum)
        return std::unexpected(LeftMinimum.error());
      auto High =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_SUB, *Type, {*LeftMinimum, *Minimum});
      if (!High)
        return std::unexpected(High.error());
      LowerBound = *Low;
      UpperBound = *High;
    }

    auto LowerClamped =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_SMAX, *Type, {LowerBound, *Right});
    if (!LowerClamped)
      return std::unexpected(LowerClamped.error());
    auto ClampedRight = buildResultInstruction(MIRBuilder, GenericOpcode::G_SMIN, *Type,
                                               {*LowerClamped, UpperBound});
    if (!ClampedRight)
      return std::unexpected(ClampedRight.error());
    if (auto Built =
            MIRBuilder.buildInstr(opcode(ArithmeticOpcode), {*Destination}, {*Left, *ClampedRight});
        !Built) {
      return std::unexpected(Built.error());
    }
  } else {
    Register Available;
    if (IsAdd) {
      auto AllBits = APInt::getLowBitsSet(BitWidth, BitWidth);
      if (!AllBits)
        return std::unexpected(AllBits.error());
      auto AllOnes = buildConstantRegister(MIRBuilder, *Type, std::move(*AllBits));
      if (!AllOnes)
        return std::unexpected(AllOnes.error());
      auto Inverted =
          buildResultInstruction(MIRBuilder, GenericOpcode::G_XOR, *Type, {*Left, *AllOnes});
      if (!Inverted)
        return std::unexpected(Inverted.error());
      Available = *Inverted;
    } else {
      Available = *Left;
    }
    auto Minimum =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_UMIN, *Type, {Available, *Right});
    if (!Minimum)
      return std::unexpected(Minimum.error());
    if (auto Built =
            MIRBuilder.buildInstr(opcode(ArithmeticOpcode), {*Destination}, {*Left, *Minimum});
        !Built) {
      return std::unexpected(Built.error());
    }
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerSaturatingAddSubToOverflow(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "saturating add/sub overflow lowering requires three register operands");

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  const bool IsSigned = Generic == GenericOpcode::G_SADDSAT || Generic == GenericOpcode::G_SSUBSAT;
  const bool IsAdd = Generic == GenericOpcode::G_UADDSAT || Generic == GenericOpcode::G_SADDSAT;
  const GenericOpcode OverflowOpcode =
      IsAdd ? (IsSigned ? GenericOpcode::G_SADDO : GenericOpcode::G_UADDO)
            : (IsSigned ? GenericOpcode::G_SSUBO : GenericOpcode::G_USUBO);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(1);
  const MachineOperand *RightOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (Destination == nullptr || Left == nullptr || Right == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "saturating add/sub overflow lowering requires register operands");

  auto Type = MRI.getType(*Destination);
  if (!Type)
    return std::unexpected(Type.error());
  if (!Type->isCategoryNeutralScalar() || Type->scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "saturating add/sub overflow lowering requires a scalar integer type");
  auto BooleanType = booleanTypeFor(*Type);
  if (!BooleanType)
    return std::unexpected(BooleanType.error());
  auto Temporary = MRI.createVirtualRegister(*Type);
  auto Overflow = MRI.createVirtualRegister(*BooleanType);
  if (!Temporary)
    return std::unexpected(Temporary.error());
  if (!Overflow)
    return std::unexpected(Overflow.error());

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  if (auto Built =
          MIRBuilder.buildInstr(opcode(OverflowOpcode), {*Temporary, *Overflow}, {*Left, *Right});
      !Built) {
    return std::unexpected(Built.error());
  }

  Register Clamp;
  if (IsSigned) {
    const std::uint32_t BitWidth = Type->scalarBitWidth();
    auto ShiftAmount =
        buildConstantRegister(MIRBuilder, *Type, static_cast<std::int64_t>(BitWidth - 1U));
    auto MinimumValue = oneBitSet(BitWidth, BitWidth - 1U);
    if (!ShiftAmount)
      return std::unexpected(ShiftAmount.error());
    if (!MinimumValue)
      return std::unexpected(MinimumValue.error());
    auto Sign = buildResultInstruction(MIRBuilder, GenericOpcode::G_ASHR, *Type,
                                       {*Temporary, *ShiftAmount});
    auto Minimum = buildConstantRegister(MIRBuilder, *Type, std::move(*MinimumValue));
    if (!Sign)
      return std::unexpected(Sign.error());
    if (!Minimum)
      return std::unexpected(Minimum.error());
    auto Saturated =
        buildResultInstruction(MIRBuilder, GenericOpcode::G_ADD, *Type, {*Sign, *Minimum});
    if (!Saturated)
      return std::unexpected(Saturated.error());
    Clamp = *Saturated;
  } else if (IsAdd) {
    auto AllBits = APInt::getLowBitsSet(Type->scalarBitWidth(), Type->scalarBitWidth());
    if (!AllBits)
      return std::unexpected(AllBits.error());
    auto Saturated = buildConstantRegister(MIRBuilder, *Type, std::move(*AllBits));
    if (!Saturated)
      return std::unexpected(Saturated.error());
    Clamp = *Saturated;
  } else {
    auto Saturated = buildConstantRegister(MIRBuilder, *Type, 0);
    if (!Saturated)
      return std::unexpected(Saturated.error());
    Clamp = *Saturated;
  }

  if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {*Destination},
                                         {*Overflow, Clamp, *Temporary});
      !Built) {
    return std::unexpected(Built.error());
  }
  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::lowerSaturatingShiftLeft(MachineInstr &Instruction) {
  if (Instruction.operandCount() != 3)
    return makeUnexpected(ErrorCode::VerificationError,
                          "saturating shift-left lowering requires three register operands");

  const bool IsSigned = Instruction.opcode() == opcode(GenericOpcode::G_SSHLSAT);
  const MachineOperand *DestinationOperand = Instruction.operandAt(0);
  const MachineOperand *LeftOperand = Instruction.operandAt(1);
  const MachineOperand *RightOperand = Instruction.operandAt(2);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  const Register *Left = LeftOperand == nullptr ? nullptr : LeftOperand->getIf<Register>();
  const Register *Right = RightOperand == nullptr ? nullptr : RightOperand->getIf<Register>();
  if (Destination == nullptr || Left == nullptr || Right == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "saturating shift-left lowering requires register operands");

  auto Type = MRI.getType(*Destination);
  if (!Type)
    return std::unexpected(Type.error());
  if (!Type->isCategoryNeutralScalar() || Type->scalarBitWidth() == 0)
    return makeUnexpected(ErrorCode::Unsupported,
                          "saturating shift-left lowering requires a scalar integer type");
  auto BooleanType = booleanTypeFor(*Type);
  if (!BooleanType)
    return std::unexpected(BooleanType.error());
  const std::uint32_t BitWidth = Type->scalarBitWidth();

  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Shifted = buildResultInstruction(MIRBuilder, GenericOpcode::G_SHL, *Type, {*Left, *Right});
  if (!Shifted)
    return std::unexpected(Shifted.error());
  auto Restored =
      buildResultInstruction(MIRBuilder, IsSigned ? GenericOpcode::G_ASHR : GenericOpcode::G_LSHR,
                             *Type, {*Shifted, *Right});
  if (!Restored)
    return std::unexpected(Restored.error());

  Register Saturated;
  if (IsSigned) {
    auto MinimumValue = oneBitSet(BitWidth, BitWidth - 1U);
    auto MaximumValue = APInt::getLowBitsSet(BitWidth, BitWidth - 1U);
    if (!MinimumValue)
      return std::unexpected(MinimumValue.error());
    if (!MaximumValue)
      return std::unexpected(MaximumValue.error());
    auto Minimum = buildConstantRegister(MIRBuilder, *Type, std::move(*MinimumValue));
    auto Maximum = buildConstantRegister(MIRBuilder, *Type, std::move(*MaximumValue));
    auto Zero = buildConstantRegister(MIRBuilder, *Type, 0);
    if (!Minimum)
      return std::unexpected(Minimum.error());
    if (!Maximum)
      return std::unexpected(Maximum.error());
    if (!Zero)
      return std::unexpected(Zero.error());
    auto Negative =
        buildICmpRegister(MIRBuilder, IntegerPredicate::SignedLessThan, *BooleanType, *Left, *Zero);
    if (!Negative)
      return std::unexpected(Negative.error());
    auto SignSaturated = buildResultInstruction(MIRBuilder, GenericOpcode::G_SELECT, *Type,
                                                {*Negative, *Minimum, *Maximum});
    if (!SignSaturated)
      return std::unexpected(SignSaturated.error());
    Saturated = *SignSaturated;
  } else {
    auto AllBits = APInt::getLowBitsSet(BitWidth, BitWidth);
    if (!AllBits)
      return std::unexpected(AllBits.error());
    auto Maximum = buildConstantRegister(MIRBuilder, *Type, std::move(*AllBits));
    if (!Maximum)
      return std::unexpected(Maximum.error());
    Saturated = *Maximum;
  }

  auto Overflow =
      buildICmpRegister(MIRBuilder, IntegerPredicate::NotEqual, *BooleanType, *Left, *Restored);
  if (!Overflow)
    return std::unexpected(Overflow.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(GenericOpcode::G_SELECT), {*Destination},
                                         {*Overflow, Saturated, *Shifted});
      !Built) {
    return std::unexpected(Built.error());
  }

  return Instruction.eraseFromParent();
}

Expected<void> LegalizerHelper::legalize(MachineInstr &Instruction,
                                         const LegalizationDecision &Decision) {
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  switch (Decision.Action) {
  case LegalizationAction::WidenScalar:
    return widenScalar(Instruction, Decision.TypeIndex, Decision.NewType);
  case LegalizationAction::NarrowScalar:
    return narrowScalar(Instruction, Decision.TypeIndex, Decision.NewType);
  case LegalizationAction::Custom:
    return Info.legalizeCustom(*this, Instruction, Decision);
  case LegalizationAction::Lower:
    return lower(Instruction, Decision);
  case LegalizationAction::Libcall:
    return libcall(Instruction);
  case LegalizationAction::FewerElements:
    return fewerElements(Instruction, Decision.TypeIndex, Decision.NewType);
  case LegalizationAction::Legal:
    return {};
  case LegalizationAction::Unsupported:
  case LegalizationAction::MoreElements:
  case LegalizationAction::Bitcast:
  case LegalizationAction::ChangeElementType:
  case LegalizationAction::ChangeAddressSpace:
    return makeUnexpected(ErrorCode::Unsupported,
                          "legalizer helper does not implement the requested action");
  }
  return makeUnexpected(ErrorCode::InternalError, "unknown legalization action");
}

} // namespace rcc
