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

[[nodiscard]] Expected<ABIValue>
integerABIValue(Register Value, LowLevelType Type, std::uint32_t OriginalValue,
                std::uint16_t FragmentIndex, std::uint16_t FragmentCount,
                std::uint32_t FragmentOffsetBytes, Alignment OriginalAlignment,
                ABIExtensionKind Extension) {
  auto OneByte = Alignment::create(1);
  if (!OneByte)
    return std::unexpected(OneByte.error());
  return ABIValue{
      .Value = Value,
      .Type = Type,
      .Category = ABIValueCategory::Integer,
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

    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "G_MERGE_VALUES became detached during widening");
    return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "G_MERGE_VALUES became detached during widening");
  return Block->removeInstruction(Instruction);
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

    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "G_UNMERGE_VALUES became detached during widening");
    return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "G_UNMERGE_VALUES became detached during widening");
  return Block->removeInstruction(Instruction);
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
  if (TypeIndex != 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "add/sub overflow widening requires type index zero");

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  GenericOpcode ArithmeticOpcode;
  GenericOpcode ExtensionOpcode;
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
  default:
    return makeUnexpected(ErrorCode::InternalError,
                          "unexpected opcode during add/sub overflow widening");
  }

  const Register Destination = *Instruction.operandAt(0)->getIf<Register>();
  const Register Overflow = *Instruction.operandAt(1)->getIf<Register>();
  const Register Left = *Instruction.operandAt(2)->getIf<Register>();
  const Register Right = *Instruction.operandAt(3)->getIf<Register>();
  auto OriginalType = MRI.getType(Destination);
  if (!OriginalType)
    return std::unexpected(OriginalType.error());

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
  if (auto Built = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*WideLeft}, {Left}); !Built)
    return std::unexpected(Built.error());
  if (auto Built = MIRBuilder.buildInstr(opcode(ExtensionOpcode), {*WideRight}, {Right}); !Built)
    return std::unexpected(Built.error());
  if (auto Built =
          MIRBuilder.buildInstr(opcode(ArithmeticOpcode), {*WideResult}, {*WideLeft, *WideRight});
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
          MIRBuilder.buildICmp(IntegerPredicate::NotEqual, Overflow, *WideResult, *ExtendedResult);
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = MIRBuilder.buildTrunc(Destination, *WideResult); !Built)
    return std::unexpected(Built.error());

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "add/sub overflow instruction became detached during widening");
  return Block->removeInstruction(Instruction);
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
    return widenScalarAddSubOverflow(Instruction, TypeIndex, WideType);

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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "add/sub instruction became detached during narrowing");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "extension instruction became detached during narrowing");
  return Block->removeInstruction(Instruction);
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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "basic scalar instruction became detached during narrowing");
  return Block->removeInstruction(Instruction);
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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "G_SELECT became detached during narrowing");
  return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "constant shift became detached during narrowing");
    return Block->removeInstruction(Instruction);
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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "constant shift became detached during narrowing");
  return Block->removeInstruction(Instruction);
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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "shift became detached during narrowing");
  return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "constant multiway shift became detached");
    return Block->removeInstruction(Instruction);
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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "constant multiway shift became detached");
  return Block->removeInstruction(Instruction);
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
  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "multiway shift became detached");
  return Block->removeInstruction(Instruction);
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

    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "G_IMPLICIT_DEF became detached during narrowing");
    return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "G_CONSTANT became detached during narrowing");
    return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError,
                            "G_SEXT_INREG became detached during narrowing");
    return Block->removeInstruction(Instruction);
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
  default:
    return makeUnexpected(ErrorCode::Unsupported,
                          "scalar narrowing is not implemented for this opcode");
  }
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::VerificationError, "G_VAARG is detached during legalization");
  return Block->removeInstruction(Instruction);
}

Expected<void> LegalizerHelper::lower(MachineInstr &Instruction,
                                      const LegalizationDecision &Decision) {
  if (Decision.Action != LegalizationAction::Lower)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "lower received a non-lower legalization action");
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
  case GenericOpcode::G_ABS:
    return lowerAbsToAddXor(Instruction);
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
  case GenericOpcode::G_ROTL:
  case GenericOpcode::G_ROTR:
    return lowerRotate(Instruction);
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
    return lowerSignedAddSubOverflow(Instruction);
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

    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "G_SEXT_INREG became detached during lowering");
    }
    return Block->removeInstruction(Instruction);
  }
  case GenericOpcode::G_VAARG:
    return lowerVAArg(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "G_UADDO became detached during lowering");
    return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "G_UADDE became detached during lowering");
    return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "G_USUBO became detached during lowering");
    return Block->removeInstruction(Instruction);
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
    MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "G_USUBE became detached during lowering");
    return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "G_ABS became detached during lowering");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "G_ABDS/G_ABDU became detached during select lowering");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "G_ABDS/G_ABDU became detached during min/max lowering");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "integer min/max became detached during lowering");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "three-way comparison became detached during lowering");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "DIVREM became detached during lowering");
  return Block->removeInstruction(Instruction);
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError, "rotate became detached during lowering");
  return Block->removeInstruction(Instruction);
}

Expected<void> LegalizerHelper::libcall(MachineInstr &Instruction) {
  if (Libcalls == nullptr || Libcalls->CallLowering == nullptr) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "libcall legalization requires target call lowering information");
  }

  const GenericOpcode Generic = static_cast<GenericOpcode>(Instruction.opcode().value());
  auto DestinationOperand = Instruction.operandAt(0);
  const Register *Destination =
      DestinationOperand == nullptr ? nullptr : DestinationOperand->getIf<Register>();
  if (Destination == nullptr || !DestinationOperand->isDef()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "integer libcall requires a register destination");
  }
  auto Type = MRI.getType(*Destination);
  if (!Type)
    return std::unexpected(Type.error());
  if (!Type->isCategoryNeutralScalar()) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "integer libcall requires a category-neutral scalar type");
  }

  const std::uint32_t BitWidth = Type->scalarBitWidth();
  auto Name = integerLibcallName(Generic, BitWidth);
  if (!Name)
    return std::unexpected(Name.error());
  auto OriginalAlignment = Alignment::create(BitWidth == 128 ? 16 : BitWidth / 8U);
  if (!OriginalAlignment)
    return std::unexpected(OriginalAlignment.error());
  auto FragmentType = LowLevelType::makeCategoryNeutralScalar(64);
  if (!FragmentType)
    return std::unexpected(FragmentType.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());

  const ABIExtensionKind Extension = integerLibcallExtension(Generic, BitWidth);
  auto AppendValue = [&](Register Value, std::uint32_t OriginalValue, bool IsResult,
                         std::vector<ABIValue> &Values,
                         std::vector<Register> *Fragments) -> Expected<void> {
    if (BitWidth != 128) {
      auto ABI =
          integerABIValue(Value, *Type, OriginalValue, 0, 1, 0, *OriginalAlignment, Extension);
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
      auto ABI = integerABIValue(PartRegisters[Index], *FragmentType, OriginalValue, Index, 2,
                                 static_cast<std::uint32_t>(Index) * 8U, *OriginalAlignment,
                                 ABIExtensionKind::None);
      if (!ABI)
        return std::unexpected(ABI.error());
      Values.push_back(*ABI);
    }
    if (Fragments != nullptr)
      *Fragments = std::move(PartRegisters);
    return {};
  };

  std::vector<ABIValue> Arguments;
  Arguments.reserve((Instruction.operandCount() - 1U) * (BitWidth == 128 ? 2U : 1U));
  std::uint32_t OriginalArgument = 0;
  for (std::size_t Index = 1; Index != Instruction.operandCount(); ++Index) {
    const MachineOperand *Operand = Instruction.operandAt(Index);
    const Register *Source = Operand == nullptr ? nullptr : Operand->getIf<Register>();
    if (Source == nullptr || !Operand->isUse()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer libcall requires register source operands");
    }
    auto SourceType = MRI.getType(*Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    if (*SourceType != *Type) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer libcall source and destination types must match");
    }
    if (auto Appended = AppendValue(*Source, OriginalArgument++, false, Arguments, nullptr);
        !Appended) {
      return std::unexpected(Appended.error());
    }
  }

  std::vector<ABIValue> Results;
  std::vector<Register> ResultFragments;
  if (auto Appended = AppendValue(*Destination, 0, true, Results, &ResultFragments); !Appended)
    return std::unexpected(Appended.error());

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

  if (BitWidth == 128) {
    auto Merged = MIRBuilder.buildMergeLikeInstr(*Destination, ResultFragments);
    if (!Merged)
      return std::unexpected(Merged.error());
  }

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "integer libcall became detached during legalization");
  return Block->removeInstruction(Instruction);
}

Expected<void> LegalizerHelper::lowerSignedAddSubOverflow(MachineInstr &Instruction) {
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

  MachineBasicBlock *Block = Instruction.parent();
  if (Block == nullptr)
    return makeUnexpected(ErrorCode::InternalError,
                          "signed add/sub overflow instruction became detached during lowering");
  return Block->removeInstruction(Instruction);
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
  case LegalizationAction::Legal:
    return {};
  case LegalizationAction::Unsupported:
  case LegalizationAction::MoreElements:
  case LegalizationAction::FewerElements:
  case LegalizationAction::Bitcast:
  case LegalizationAction::ChangeElementType:
  case LegalizationAction::ChangeAddressSpace:
    return makeUnexpected(ErrorCode::Unsupported,
                          "legalizer helper does not implement the requested action");
  }
  return makeUnexpected(ErrorCode::InternalError, "unknown legalization action");
}

} // namespace rcc
