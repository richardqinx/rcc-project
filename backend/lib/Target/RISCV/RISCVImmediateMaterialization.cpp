// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVImmediateMaterialization.h"

#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVSubtarget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace rcc::riscv {
namespace {

[[nodiscard]] constexpr bool fitsSigned(std::int64_t Value, std::uint32_t Bits) noexcept {
  const std::int64_t Minimum = -(std::int64_t{1} << (Bits - 1U));
  const std::int64_t Maximum = (std::int64_t{1} << (Bits - 1U)) - 1;
  return Value >= Minimum && Value <= Maximum;
}

[[nodiscard]] constexpr std::int64_t signExtend(std::uint64_t Value, std::uint32_t Bits) noexcept {
  if (Bits == 64)
    return std::bit_cast<std::int64_t>(Value);
  const std::uint64_t Mask = (std::uint64_t{1} << Bits) - 1U;
  const std::uint64_t Sign = std::uint64_t{1} << (Bits - 1U);
  Value &= Mask;
  if ((Value & Sign) != 0)
    Value |= ~Mask;
  return std::bit_cast<std::int64_t>(Value);
}

[[nodiscard]] constexpr std::uint64_t trailingBitMask(std::uint32_t Bits) noexcept {
  return Bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << Bits) - 1U;
}

[[nodiscard]] constexpr std::int64_t arithmeticShiftRight(std::uint64_t Value,
                                                          std::uint32_t Amount) noexcept {
  return signExtend(Value >> Amount, 64U - Amount);
}

void appendImmediate(ImmediateMaterializationSequence &Sequence, InstructionOpcode Opcode,
                     std::int64_t Immediate, ImmediateMaterializationOperandKind OperandKind) {
  Sequence.push_back({.Opcode = Opcode, .Immediate = Immediate, .OperandKind = OperandKind});
}

void generateBaseSequence(std::int64_t Value, ImmediateMaterializationSequence &Sequence) {
  const std::uint64_t Bits = std::bit_cast<std::uint64_t>(Value);
  if (fitsSigned(Value, 32)) {
    const std::int64_t High20 = static_cast<std::int64_t>(((Bits + 0x800U) >> 12U) & 0xFFFFFU);
    const std::int64_t Low12 = signExtend(Bits, 12);
    if (High20 != 0) {
      appendImmediate(Sequence, InstructionOpcode::LUI, High20,
                      ImmediateMaterializationOperandKind::Immediate);
    }
    if (Low12 != 0 || High20 == 0) {
      InstructionOpcode AddOpcode = InstructionOpcode::ADDI;
      if (High20 != 0) {
        const std::int64_t LUIResult = signExtend(static_cast<std::uint64_t>(High20) << 12U, 32);
        if (!fitsSigned(LUIResult + Low12, 32))
          AddOpcode = InstructionOpcode::ADDIW;
      }
      appendImmediate(Sequence, AddOpcode, Low12,
                      ImmediateMaterializationOperandKind::RegisterImmediate);
    }
    return;
  }

  const std::int64_t Low12 = signExtend(Bits, 12);
  const std::uint64_t WithoutLow12 = Bits - static_cast<std::uint64_t>(Low12);
  std::int64_t Remaining = std::bit_cast<std::int64_t>(WithoutLow12);
  std::uint32_t ShiftAmount = 0;
  if (!fitsSigned(Remaining, 32)) {
    ShiftAmount = static_cast<std::uint32_t>(std::countr_zero(WithoutLow12));
    Remaining = arithmeticShiftRight(WithoutLow12, ShiftAmount);
    if (ShiftAmount > 12 && !fitsSigned(Remaining, 12)) {
      const std::int64_t ShiftedRemaining =
          std::bit_cast<std::int64_t>(std::bit_cast<std::uint64_t>(Remaining) << 12U);
      if (fitsSigned(ShiftedRemaining, 32)) {
        ShiftAmount -= 12;
        Remaining = ShiftedRemaining;
      }
    }
  }

  generateBaseSequence(Remaining, Sequence);
  if (ShiftAmount != 0) {
    appendImmediate(Sequence, InstructionOpcode::SLLI, static_cast<std::int64_t>(ShiftAmount),
                    ImmediateMaterializationOperandKind::RegisterImmediate);
  }
  if (Low12 != 0) {
    appendImmediate(Sequence, InstructionOpcode::ADDI, Low12,
                    ImmediateMaterializationOperandKind::RegisterImmediate);
  }
}

void considerLeadingZeroSequence(std::int64_t Value, ImmediateMaterializationSequence &Sequence) {
  const std::uint64_t Bits = std::bit_cast<std::uint64_t>(Value);
  const std::uint32_t LeadingZeros = static_cast<std::uint32_t>(std::countl_zero(Bits));
  const std::uint64_t Shifted = Bits << LeadingZeros;
  const auto Consider = [&](std::uint64_t Candidate) {
    ImmediateMaterializationSequence Temporary;
    generateBaseSequence(std::bit_cast<std::int64_t>(Candidate), Temporary);
    const bool Improves = Temporary.size() + 1U < Sequence.size();
    const bool InitializesSequence = Sequence.empty() && Temporary.size() < 8;
    if (!Improves && !InitializesSequence)
      return;
    appendImmediate(Temporary, InstructionOpcode::SRLI, static_cast<std::int64_t>(LeadingZeros),
                    ImmediateMaterializationOperandKind::RegisterImmediate);
    Sequence = std::move(Temporary);
  };

  Consider(Shifted | trailingBitMask(LeadingZeros));
  Consider(Shifted & ~trailingBitMask(LeadingZeros));
}

[[nodiscard]] std::optional<std::size_t>
twoRegisterBuildCost(std::int64_t Value, const RISCVSubtargetFeatures &Features) {
  const std::uint64_t Bits = std::bit_cast<std::uint64_t>(Value);
  const std::int64_t Low = signExtend(Bits, 32);
  if (Low == 0)
    return std::nullopt;

  const std::uint64_t HighContribution = Bits - static_cast<std::uint64_t>(Low);
  if (HighContribution == 0)
    return std::nullopt;
  const std::uint32_t LowZeros =
      static_cast<std::uint32_t>(std::countr_zero(static_cast<std::uint64_t>(Low)));
  const std::uint32_t HighZeros = static_cast<std::uint32_t>(std::countr_zero(HighContribution));
  if (LowZeros >= 32 || HighZeros < 32 || HighZeros < LowZeros)
    return std::nullopt;
  const std::uint32_t ShiftAmount = HighZeros - LowZeros;
  if (ShiftAmount >= 64 || HighContribution != (static_cast<std::uint64_t>(Low) << ShiftAmount)) {
    return std::nullopt;
  }
  return generateRV64IntegerMaterialization(Low, Features).size() + 2U;
}

} // namespace

ImmediateMaterializationSequence
generateRV64IntegerMaterialization(std::int64_t Value, const RISCVSubtargetFeatures &Features) {
  ImmediateMaterializationSequence Result;
  generateBaseSequence(Value, Result);

  const std::uint64_t Bits = std::bit_cast<std::uint64_t>(Value);
  if ((Bits & 0xFFFU) != 0 && (Bits & 1U) == 0 && Result.size() >= 2) {
    const std::uint32_t TrailingZeros = static_cast<std::uint32_t>(std::countr_zero(Bits));
    const std::int64_t ShiftedValue = arithmeticShiftRight(Bits, TrailingZeros);
    ImmediateMaterializationSequence Temporary;
    generateBaseSequence(ShiftedValue, Temporary);
    const bool PreferCompressibleShift =
        fitsSigned(ShiftedValue, 6) && !Features.has(RISCVSubtargetOption::FusionLUIAddImmediate);
    if (Temporary.size() + 1U < Result.size() || PreferCompressibleShift) {
      appendImmediate(Temporary, InstructionOpcode::SLLI, static_cast<std::int64_t>(TrailingZeros),
                      ImmediateMaterializationOperandKind::RegisterImmediate);
      Result = std::move(Temporary);
    }
  }

  if (Result.size() <= 2)
    return Result;

  if ((Bits & 0xFFFU) != 0 && (Bits & 0x1800U) == 0x1000U) {
    const std::int64_t LastAdd = -static_cast<std::int64_t>(0x800U - (Bits & 0xFFFU));
    const std::int64_t AdjustedValue =
        std::bit_cast<std::int64_t>(Bits - static_cast<std::uint64_t>(LastAdd));
    ImmediateMaterializationSequence Temporary;
    generateBaseSequence(AdjustedValue, Temporary);
    if (Temporary.size() + 1U < Result.size()) {
      appendImmediate(Temporary, InstructionOpcode::ADDI, LastAdd,
                      ImmediateMaterializationOperandKind::RegisterImmediate);
      Result = std::move(Temporary);
    }
  }

  if (Value > 0 && Result.size() > 2) {
    considerLeadingZeroSequence(Value, Result);
  } else if (Value < 0 && Result.size() > 3) {
    ImmediateMaterializationSequence Temporary;
    considerLeadingZeroSequence(std::bit_cast<std::int64_t>(~Bits), Temporary);
    if (!Temporary.empty() && Temporary.size() + 1U < Result.size()) {
      appendImmediate(Temporary, InstructionOpcode::XORI, -1,
                      ImmediateMaterializationOperandKind::RegisterImmediate);
      Result = std::move(Temporary);
    }
  }
  return Result;
}

std::size_t estimateRV64IntegerBuildCost(std::int64_t Value,
                                         const RISCVSubtargetFeatures &Features) {
  return generateRV64IntegerMaterialization(Value, Features).size();
}

bool shouldUseRV64IntegerConstantPool(std::int64_t Value, const RISCVSubtargetFeatures &Features,
                                      std::size_t MaximumBuildCost) {
  if (fitsSigned(Value, 32) || estimateRV64IntegerBuildCost(Value, Features) <= MaximumBuildCost)
    return false;
  const auto TwoRegisterCost = twoRegisterBuildCost(Value, Features);
  return !TwoRegisterCost || *TwoRegisterCost > MaximumBuildCost;
}

} // namespace rcc::riscv
