// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GlobalISel/GISelValueTracking.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalISel/Utils.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::optional<Register> registerOperand(const MachineInstr &Instruction,
                                                      std::size_t Index) noexcept {
  const MachineOperand *Operand = Instruction.operandAt(Index);
  return Operand == nullptr ? std::nullopt : Operand->registerValue();
}

[[nodiscard]] Expected<APInt> zero(std::uint32_t BitWidth) {
  return APInt::fromUnsigned(BitWidth, 0);
}

[[nodiscard]] Expected<APInt> resizeMask(const APInt &Value, std::uint32_t BitWidth) {
  if (Value.bitWidth() == BitWidth)
    return Value;
  if (Value.bitWidth() == 0)
    return zero(BitWidth);
  if (Value.bitWidth() < BitWidth)
    return Value.zext(BitWidth);
  return Value.trunc(BitWidth);
}

[[nodiscard]] Expected<APInt> highBitsMask(std::uint32_t BitWidth, std::uint32_t LowBitCount) {
  auto LowBits = APInt::getLowBitsSet(BitWidth, LowBitCount);
  if (!LowBits)
    return std::unexpected(LowBits.error());
  return LowBits->bitwiseNot();
}

[[nodiscard]] Expected<APInt> mergeMasks(const APInt &Left, const APInt &Right) {
  return Left.bitwiseOr(Right);
}

[[nodiscard]] Expected<APInt> intersectMasks(const APInt &Left, const APInt &Right) {
  return Left.bitwiseAnd(Right);
}

[[nodiscard]] Expected<APInt> constantAtWidth(const APInt &Value, std::uint32_t BitWidth) {
  return resizeMask(Value, BitWidth);
}

[[nodiscard]] std::optional<std::uint32_t>
constantShiftAmount(Register Reg, const MachineRegisterInfo &MRI, std::uint32_t BitWidth) {
  const auto Constant = getIConstantVRegValWithLookThrough(Reg, MRI);
  if (!Constant || Constant->Value.activeBitWidth() > 32)
    return std::nullopt;
  const std::uint64_t Amount = Constant->Value.word(0);
  if (Amount >= BitWidth)
    return std::nullopt;
  return static_cast<std::uint32_t>(Amount);
}

[[nodiscard]] Expected<APInt> shiftMaskLeft(const APInt &Value, std::uint32_t Amount) {
  std::vector<std::uint64_t> Words(Value.words().size(), 0);
  for (std::uint32_t Bit = 0; Bit + Amount < Value.bitWidth(); ++Bit) {
    if (Value.testBit(Bit)) {
      const std::uint32_t DestinationBit = Bit + Amount;
      Words[DestinationBit / 64U] |= std::uint64_t{1} << (DestinationBit % 64U);
    }
  }
  return APInt::create(Value.bitWidth(), Words);
}

[[nodiscard]] Expected<APInt> shiftMaskRight(const APInt &Value, std::uint32_t Amount) {
  std::vector<std::uint64_t> Words(Value.words().size(), 0);
  for (std::uint32_t Bit = Amount; Bit < Value.bitWidth(); ++Bit) {
    if (Value.testBit(Bit)) {
      const std::uint32_t DestinationBit = Bit - Amount;
      Words[DestinationBit / 64U] |= std::uint64_t{1} << (DestinationBit % 64U);
    }
  }
  return APInt::create(Value.bitWidth(), Words);
}

[[nodiscard]] std::uint32_t constantNumSignBits(const APInt &Value) noexcept {
  if (Value.bitWidth() == 0)
    return 1;
  if (Value.isNegative())
    return static_cast<std::uint32_t>(Value.countlOne());
  auto Inverted = Value.bitwiseNot();
  return Inverted ? static_cast<std::uint32_t>(Inverted->countlOne()) : 1U;
}

} // namespace

GISelValueTracking::GISelValueTracking(MachineFunction &Function, const TargetDescription &Target,
                                       unsigned MaxDepth) noexcept
    : Function(Function), MRI(Function.registerInfo()), Target(Target), MaxDepth(MaxDepth) {}

Expected<GISelValueTracking::KnownBits>
GISelValueTracking::makeUnknown(std::uint32_t BitWidth) const {
  auto Zero = zero(BitWidth);
  if (!Zero)
    return std::unexpected(Zero.error());
  auto One = zero(BitWidth);
  if (!One)
    return std::unexpected(One.error());
  return KnownBits{.Zero = std::move(*Zero), .One = std::move(*One)};
}

Expected<GISelValueTracking::KnownBits> GISelValueTracking::getKnownBits(Register Reg,
                                                                         unsigned Depth) const {
  auto Type = MRI.getType(Reg);
  if (!Type)
    return std::unexpected(Type.error());
  const std::uint32_t BitWidth = Type->scalarBitWidth();
  auto Unknown = makeUnknown(BitWidth);
  if (!Unknown)
    return std::unexpected(Unknown.error());
  if (Depth >= MaxDepth)
    return Unknown;

  const MachineInstr *Definition = MRI.getVRegDef(Reg);
  if (Definition == nullptr)
    return Unknown;

  const auto Opcode = static_cast<GenericOpcode>(Definition->opcode().value());
  if (Opcode == GenericOpcode::G_CONSTANT) {
    const MachineOperand *ValueOperand = Definition->operandAt(1);
    const auto *Immediate =
        ValueOperand == nullptr ? nullptr : ValueOperand->getIf<IntegerImmediateOperand>();
    if (Immediate == nullptr)
      return Unknown;
    auto Value = constantAtWidth(Immediate->Value, BitWidth);
    if (!Value)
      return std::unexpected(Value.error());
    auto KnownZero = Value->bitwiseNot();
    if (!KnownZero)
      return std::unexpected(KnownZero.error());
    return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*Value)};
  }

  if (Opcode == GenericOpcode::COPY) {
    const auto Source = registerOperand(*Definition, 1);
    if (!Source || !Source->isVirtual())
      return Unknown;
    auto SourceBits = getKnownBits(*Source, Depth);
    if (!SourceBits)
      return std::unexpected(SourceBits.error());
    auto KnownZero = resizeMask(SourceBits->Zero, BitWidth);
    if (!KnownZero)
      return std::unexpected(KnownZero.error());
    auto KnownOne = resizeMask(SourceBits->One, BitWidth);
    if (!KnownOne)
      return std::unexpected(KnownOne.error());
    return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
  }

  if (Opcode == GenericOpcode::G_ICMP || Opcode == GenericOpcode::G_FCMP) {
    if (!Type->isVector() && BitWidth > 1 &&
        Target.getBooleanContents(false, Opcode == GenericOpcode::G_FCMP) ==
            BooleanContent::ZeroOrOne) {
      auto KnownZero = highBitsMask(BitWidth, 1);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
      Unknown->Zero = std::move(*KnownZero);
    }
    return Unknown;
  }

  switch (Opcode) {
  case GenericOpcode::G_UADDO:
  case GenericOpcode::G_UADDE:
  case GenericOpcode::G_USUBO:
  case GenericOpcode::G_USUBE:
  case GenericOpcode::G_SADDO:
  case GenericOpcode::G_SADDE:
  case GenericOpcode::G_SSUBO:
  case GenericOpcode::G_SSUBE:
  case GenericOpcode::G_UMULO:
  case GenericOpcode::G_SMULO: {
    const auto BooleanResult = registerOperand(*Definition, 1);
    if (BooleanResult && *BooleanResult == Reg && !Type->isVector() && BitWidth > 1 &&
        Target.getBooleanContents(false, false) == BooleanContent::ZeroOrOne) {
      auto KnownZero = highBitsMask(BitWidth, 1);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
      Unknown->Zero = std::move(*KnownZero);
    }
    return Unknown;
  }
  default:
    break;
  }

  const auto Source = registerOperand(*Definition, 1);
  if (!Source)
    return Unknown;

  if (Opcode == GenericOpcode::G_TRUNC || Opcode == GenericOpcode::G_ANYEXT ||
      Opcode == GenericOpcode::G_ZEXT || Opcode == GenericOpcode::G_SEXT ||
      Opcode == GenericOpcode::G_ASSERT_ZEXT || Opcode == GenericOpcode::G_ASSERT_SEXT ||
      Opcode == GenericOpcode::G_SEXT_INREG) {
    auto SourceBits = getKnownBits(*Source, Depth + 1U);
    if (!SourceBits)
      return std::unexpected(SourceBits.error());
    auto SourceType = MRI.getType(*Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    const std::uint32_t SourceWidth = SourceType->scalarBitWidth();
    auto KnownZero = resizeMask(SourceBits->Zero, BitWidth);
    if (!KnownZero)
      return std::unexpected(KnownZero.error());
    auto KnownOne = resizeMask(SourceBits->One, BitWidth);
    if (!KnownOne)
      return std::unexpected(KnownOne.error());

    if (Opcode == GenericOpcode::G_ZEXT && SourceWidth < BitWidth) {
      auto HighZero = highBitsMask(BitWidth, SourceWidth);
      if (!HighZero)
        return std::unexpected(HighZero.error());
      KnownZero = mergeMasks(*KnownZero, *HighZero);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
    }

    std::uint32_t ExtendedFrom = SourceWidth;
    if (Opcode == GenericOpcode::G_ASSERT_ZEXT || Opcode == GenericOpcode::G_ASSERT_SEXT ||
        Opcode == GenericOpcode::G_SEXT_INREG) {
      const MachineOperand *WidthOperand = Definition->operandAt(2);
      const auto *Width = WidthOperand == nullptr ? nullptr : WidthOperand->getIf<std::int64_t>();
      if (Width == nullptr || *Width <= 0 || static_cast<std::uint64_t>(*Width) > BitWidth)
        return Unknown;
      ExtendedFrom = static_cast<std::uint32_t>(*Width);
    }

    if (Opcode == GenericOpcode::G_ASSERT_ZEXT) {
      auto HighZero = highBitsMask(BitWidth, ExtendedFrom);
      if (!HighZero)
        return std::unexpected(HighZero.error());
      KnownZero = mergeMasks(*KnownZero, *HighZero);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
    }

    if ((Opcode == GenericOpcode::G_SEXT || Opcode == GenericOpcode::G_ASSERT_SEXT ||
         Opcode == GenericOpcode::G_SEXT_INREG) &&
        ExtendedFrom < BitWidth) {
      auto HighBits = highBitsMask(BitWidth, ExtendedFrom);
      if (!HighBits)
        return std::unexpected(HighBits.error());
      if (KnownZero->testBit(ExtendedFrom - 1U)) {
        KnownZero = mergeMasks(*KnownZero, *HighBits);
        if (!KnownZero)
          return std::unexpected(KnownZero.error());
      } else if (KnownOne->testBit(ExtendedFrom - 1U)) {
        KnownOne = mergeMasks(*KnownOne, *HighBits);
        if (!KnownOne)
          return std::unexpected(KnownOne.error());
      }
    }

    return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
  }

  if (Opcode == GenericOpcode::G_AND || Opcode == GenericOpcode::G_OR ||
      Opcode == GenericOpcode::G_XOR) {
    const auto Right = registerOperand(*Definition, 2);
    if (!Right)
      return Unknown;
    auto LeftBits = getKnownBits(*Source, Depth + 1U);
    if (!LeftBits)
      return std::unexpected(LeftBits.error());
    auto RightBits = getKnownBits(*Right, Depth + 1U);
    if (!RightBits)
      return std::unexpected(RightBits.error());

    if (Opcode == GenericOpcode::G_AND) {
      auto KnownZero = mergeMasks(LeftBits->Zero, RightBits->Zero);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
      auto KnownOne = intersectMasks(LeftBits->One, RightBits->One);
      if (!KnownOne)
        return std::unexpected(KnownOne.error());
      return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
    }
    if (Opcode == GenericOpcode::G_OR) {
      auto KnownZero = intersectMasks(LeftBits->Zero, RightBits->Zero);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
      auto KnownOne = mergeMasks(LeftBits->One, RightBits->One);
      if (!KnownOne)
        return std::unexpected(KnownOne.error());
      return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
    }

    auto BothZero = intersectMasks(LeftBits->Zero, RightBits->Zero);
    if (!BothZero)
      return std::unexpected(BothZero.error());
    auto BothOne = intersectMasks(LeftBits->One, RightBits->One);
    if (!BothOne)
      return std::unexpected(BothOne.error());
    auto KnownZero = mergeMasks(*BothZero, *BothOne);
    if (!KnownZero)
      return std::unexpected(KnownZero.error());
    auto LeftZeroRightOne = intersectMasks(LeftBits->Zero, RightBits->One);
    if (!LeftZeroRightOne)
      return std::unexpected(LeftZeroRightOne.error());
    auto LeftOneRightZero = intersectMasks(LeftBits->One, RightBits->Zero);
    if (!LeftOneRightZero)
      return std::unexpected(LeftOneRightZero.error());
    auto KnownOne = mergeMasks(*LeftZeroRightOne, *LeftOneRightZero);
    if (!KnownOne)
      return std::unexpected(KnownOne.error());
    return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
  }

  if (Opcode == GenericOpcode::G_SHL || Opcode == GenericOpcode::G_LSHR ||
      Opcode == GenericOpcode::G_ASHR) {
    const auto AmountReg = registerOperand(*Definition, 2);
    if (!AmountReg)
      return Unknown;
    const auto Amount = constantShiftAmount(*AmountReg, MRI, BitWidth);
    if (!Amount)
      return Unknown;
    auto SourceBits = getKnownBits(*Source, Depth + 1U);
    if (!SourceBits)
      return std::unexpected(SourceBits.error());

    if (Opcode == GenericOpcode::G_SHL) {
      auto KnownZero = shiftMaskLeft(SourceBits->Zero, *Amount);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
      auto LowZero = APInt::getLowBitsSet(BitWidth, *Amount);
      if (!LowZero)
        return std::unexpected(LowZero.error());
      KnownZero = mergeMasks(*KnownZero, *LowZero);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
      auto KnownOne = shiftMaskLeft(SourceBits->One, *Amount);
      if (!KnownOne)
        return std::unexpected(KnownOne.error());
      return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
    }

    auto KnownZero = shiftMaskRight(SourceBits->Zero, *Amount);
    if (!KnownZero)
      return std::unexpected(KnownZero.error());
    auto KnownOne = shiftMaskRight(SourceBits->One, *Amount);
    if (!KnownOne)
      return std::unexpected(KnownOne.error());
    auto HighBits = highBitsMask(BitWidth, BitWidth - *Amount);
    if (!HighBits)
      return std::unexpected(HighBits.error());
    if (Opcode == GenericOpcode::G_LSHR || SourceBits->Zero.testBit(BitWidth - 1U)) {
      KnownZero = mergeMasks(*KnownZero, *HighBits);
      if (!KnownZero)
        return std::unexpected(KnownZero.error());
    } else if (SourceBits->One.testBit(BitWidth - 1U)) {
      KnownOne = mergeMasks(*KnownOne, *HighBits);
      if (!KnownOne)
        return std::unexpected(KnownOne.error());
    }
    return KnownBits{.Zero = std::move(*KnownZero), .One = std::move(*KnownOne)};
  }

  return Unknown;
}

Expected<APInt> GISelValueTracking::getKnownZeroes(Register Reg) const {
  auto Known = getKnownBits(Reg);
  if (!Known)
    return std::unexpected(Known.error());
  return std::move(Known->Zero);
}

Expected<std::uint32_t> GISelValueTracking::computeNumSignBits(Register Reg, unsigned Depth) const {
  auto Type = MRI.getType(Reg);
  if (!Type)
    return std::unexpected(Type.error());
  const std::uint32_t BitWidth = Type->scalarBitWidth();
  if (Depth >= MaxDepth)
    return 1U;

  const MachineInstr *Definition = MRI.getVRegDef(Reg);
  if (Definition == nullptr)
    return 1U;
  const auto Opcode = static_cast<GenericOpcode>(Definition->opcode().value());
  std::uint32_t FirstAnswer = 1U;

  if (Opcode == GenericOpcode::G_CONSTANT) {
    const MachineOperand *ValueOperand = Definition->operandAt(1);
    const auto *Immediate =
        ValueOperand == nullptr ? nullptr : ValueOperand->getIf<IntegerImmediateOperand>();
    if (Immediate == nullptr)
      return 1U;
    auto Value = constantAtWidth(Immediate->Value, BitWidth);
    if (!Value)
      return std::unexpected(Value.error());
    return constantNumSignBits(*Value);
  }

  const auto Source = registerOperand(*Definition, 1);
  if (Opcode == GenericOpcode::COPY && Source && Source->isVirtual())
    return computeNumSignBits(*Source, Depth);

  if (Opcode == GenericOpcode::G_SEXT && Source) {
    auto SourceType = MRI.getType(*Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    auto SourceSignBits = computeNumSignBits(*Source, Depth + 1U);
    if (!SourceSignBits)
      return std::unexpected(SourceSignBits.error());
    return std::min(BitWidth, *SourceSignBits + BitWidth - SourceType->scalarBitWidth());
  }

  if ((Opcode == GenericOpcode::G_ASSERT_SEXT || Opcode == GenericOpcode::G_SEXT_INREG) && Source) {
    const MachineOperand *WidthOperand = Definition->operandAt(2);
    const auto *Width = WidthOperand == nullptr ? nullptr : WidthOperand->getIf<std::int64_t>();
    if (Width != nullptr && *Width > 0 && static_cast<std::uint64_t>(*Width) <= BitWidth) {
      auto SourceSignBits = computeNumSignBits(*Source, Depth + 1U);
      if (!SourceSignBits)
        return std::unexpected(SourceSignBits.error());
      return std::max(*SourceSignBits, BitWidth - static_cast<std::uint32_t>(*Width) + 1U);
    }
  }

  if ((Opcode == GenericOpcode::G_AND || Opcode == GenericOpcode::G_OR ||
       Opcode == GenericOpcode::G_XOR) &&
      Source) {
    const auto Right = registerOperand(*Definition, 2);
    if (Right) {
      auto LeftSignBits = computeNumSignBits(*Source, Depth + 1U);
      if (!LeftSignBits)
        return std::unexpected(LeftSignBits.error());
      if (*LeftSignBits != 1) {
        auto RightSignBits = computeNumSignBits(*Right, Depth + 1U);
        if (!RightSignBits)
          return std::unexpected(RightSignBits.error());
        if (*RightSignBits != 1)
          FirstAnswer = std::min(*LeftSignBits, *RightSignBits);
      }
    }
  }

  if (Opcode == GenericOpcode::G_ASHR && Source) {
    const auto Right = registerOperand(*Definition, 2);
    if (Right) {
      auto SourceSignBits = computeNumSignBits(*Source, Depth + 1U);
      if (!SourceSignBits)
        return std::unexpected(SourceSignBits.error());
      FirstAnswer = *SourceSignBits;
      if (const auto Amount = constantShiftAmount(*Right, MRI, BitWidth))
        FirstAnswer = std::min(BitWidth, FirstAnswer + *Amount);
    }
  }

  if (Opcode == GenericOpcode::G_TRUNC && Source) {
    auto SourceType = MRI.getType(*Source);
    if (!SourceType)
      return std::unexpected(SourceType.error());
    const std::uint32_t SourceWidth = SourceType->scalarBitWidth();
    auto SourceSignBits = computeNumSignBits(*Source, Depth + 1U);
    if (!SourceSignBits)
      return std::unexpected(SourceSignBits.error());
    if (SourceWidth > BitWidth && *SourceSignBits > SourceWidth - BitWidth)
      return *SourceSignBits - (SourceWidth - BitWidth);
  }

  if (Opcode == GenericOpcode::G_ICMP || Opcode == GenericOpcode::G_FCMP) {
    if (BitWidth > 1) {
      const BooleanContent Contents =
          Target.getBooleanContents(Type->isVector(), Opcode == GenericOpcode::G_FCMP);
      if (Contents == BooleanContent::ZeroOrNegativeOne)
        return BitWidth;
      if (Contents == BooleanContent::ZeroOrOne)
        return BitWidth - 1U;
    }
  }

  switch (Opcode) {
  case GenericOpcode::G_UADDO:
  case GenericOpcode::G_UADDE:
  case GenericOpcode::G_USUBO:
  case GenericOpcode::G_USUBE:
  case GenericOpcode::G_SADDO:
  case GenericOpcode::G_SADDE:
  case GenericOpcode::G_SSUBO:
  case GenericOpcode::G_SSUBE:
  case GenericOpcode::G_UMULO:
  case GenericOpcode::G_SMULO: {
    const auto BooleanResult = registerOperand(*Definition, 1);
    if (BooleanResult && *BooleanResult == Reg &&
        Target.getBooleanContents(Type->isVector(), false) == BooleanContent::ZeroOrNegativeOne)
      return BitWidth;
    break;
  }
  default:
    break;
  }

  auto Known = getKnownBits(Reg, Depth);
  if (!Known)
    return std::unexpected(Known.error());
  std::uint32_t KnownSignBits = 1;
  if (Known->Zero.testBit(BitWidth - 1U))
    KnownSignBits = static_cast<std::uint32_t>(Known->Zero.countlOne());
  else if (Known->One.testBit(BitWidth - 1U))
    KnownSignBits = static_cast<std::uint32_t>(Known->One.countlOne());
  return std::max(FirstAnswer, std::max(1U, KnownSignBits));
}

} // namespace rcc
