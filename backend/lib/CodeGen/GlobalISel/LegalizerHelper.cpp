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

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <optional>
#include <span>
#include <utility>

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

} // namespace

LegalizerHelper::LegalizerHelper(MachineFunction &Function, const LegalizerInfo &Info,
                                 GISelChangeObserver &Observer, MachineIRBuilder &MIRBuilder)
    : MIRBuilder(MIRBuilder), Observer(Observer), Function(Function), MRI(Function.registerInfo()),
      Info(Info) {}

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
    } else {
      auto Built = MIRBuilder.buildUndef(GCDType);
      if (!Built)
        return std::unexpected(Built.error());
      auto Pad = resultRegister(**Built);
      if (!Pad)
        return std::unexpected(Pad.error());
      PadRegister = *Pad;
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
      } else {
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

  auto WideSource = MRI.createVirtualRegister(WideType);
  if (!WideSource)
    return std::unexpected(WideSource.error());
  if (auto Point = MIRBuilder.setInsertPointBefore(Instruction); !Point)
    return std::unexpected(Point.error());
  auto Extension = MIRBuilder.buildInstr(opcode(ExtensionOpcode));
  if (!Extension)
    return std::unexpected(Extension.error());
  if (auto Added = Extension->addDef(*WideSource); !Added)
    return std::unexpected(Added.error());
  if (auto Added = Extension->addUse(*Source); !Added)
    return std::unexpected(Added.error());
  if (auto Built = Extension->build(); !Built)
    return std::unexpected(Built.error());
  return Operand->setRegister(*WideSource);
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

Expected<void> LegalizerHelper::narrowScalar(MachineInstr &Instruction, std::size_t TypeIndex,
                                             LowLevelType NarrowType) {
  switch (static_cast<GenericOpcode>(Instruction.opcode().value())) {
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
  case LegalizationAction::Legal:
    return {};
  case LegalizationAction::Unsupported:
  case LegalizationAction::Libcall:
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
