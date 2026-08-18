// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVAsmBackend.h"

#include "rcc/Target/RISCV/RISCVFixupKinds.h"
#include "rcc/Target/RISCV/RISCVInstPrinter.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace rcc::riscv {
namespace {

enum class RISCVRelocation : std::uint32_t {
  Relocation32 = 1,
  Relocation64 = 2,
  Branch = 16,
  JAL = 17,
  CallPLT = 19,
  GOTHigh20 = 20,
  PCRelativeHigh20 = 23,
  PCRelativeLow12I = 24,
  PCRelativeLow12S = 25,
  High20 = 26,
  Low12I = 27,
  Low12S = 28,
  Add8 = 33,
  Add16 = 34,
  Add32 = 35,
  Add64 = 36,
  Subtract8 = 37,
  Subtract16 = 38,
  Subtract32 = 39,
  Subtract64 = 40,
  RVCBranch = 44,
  RVCJump = 45,
  PCRelative32 = 57,
};

[[nodiscard]] bool isTargetKind(mc::MCFixupKind Kind, RISCVFixupKind TargetKind) noexcept {
  return Kind == fixupKind(TargetKind);
}

[[nodiscard]] Expected<void> requireStorage(std::span<std::uint8_t> Data, std::uint64_t Offset,
                                            std::uint8_t Size) {
  if (Offset > Data.size() || Size > Data.size() - static_cast<std::size_t>(Offset)) {
    return makeUnexpected(ErrorCode::FixupApplicationFailed,
                          "RISC-V fixup exceeds its encoded fragment");
  }
  return {};
}

void writeLittleEndian(std::span<std::uint8_t> Data, std::uint64_t Offset, std::uint64_t Value,
                       std::uint8_t Size) noexcept {
  for (std::uint8_t Index = 0; Index < Size; ++Index)
    Data[static_cast<std::size_t>(Offset) + Index] =
        static_cast<std::uint8_t>(Value >> (Index * 8U));
}

[[nodiscard]] std::uint32_t readWord(std::span<const std::uint8_t> Data,
                                     std::uint64_t Offset) noexcept {
  std::uint32_t Result = 0;
  for (std::uint8_t Index = 0; Index < 4; ++Index)
    Result |= static_cast<std::uint32_t>(Data[static_cast<std::size_t>(Offset) + Index])
              << (Index * 8U);
  return Result;
}

[[nodiscard]] std::uint16_t readHalf(std::span<const std::uint8_t> Data,
                                     std::uint64_t Offset) noexcept {
  return static_cast<std::uint16_t>(Data[static_cast<std::size_t>(Offset)]) |
         static_cast<std::uint16_t>(Data[static_cast<std::size_t>(Offset) + 1U] << 8U);
}

[[nodiscard]] bool fitsSigned(std::int64_t Value, unsigned Bits) noexcept {
  const std::int64_t Minimum = -(std::int64_t{1} << (Bits - 1U));
  const std::int64_t Maximum = (std::int64_t{1} << (Bits - 1U)) - 1;
  return Value >= Minimum && Value <= Maximum;
}

[[nodiscard]] Expected<void> requireDisplacement(std::int64_t Value, unsigned Bits,
                                                 std::string_view Name) {
  if ((Value & 1) != 0 || !fitsSigned(Value, Bits)) {
    return makeUnexpected(ErrorCode::FixupApplicationFailed,
                          std::string(Name) + " displacement is out of range or unaligned");
  }
  return {};
}

[[nodiscard]] std::uint32_t encodeBranchImmediate(std::int64_t Value) noexcept {
  const auto Bits = static_cast<std::uint32_t>(Value);
  return ((Bits >> 12U) & 0x1U) << 31U | ((Bits >> 5U) & 0x3fU) << 25U |
         ((Bits >> 1U) & 0xfU) << 8U | ((Bits >> 11U) & 0x1U) << 7U;
}

[[nodiscard]] std::uint32_t encodeJALImmediate(std::int64_t Value) noexcept {
  const auto Bits = static_cast<std::uint32_t>(Value);
  return ((Bits >> 20U) & 0x1U) << 31U | ((Bits >> 1U) & 0x3ffU) << 21U |
         ((Bits >> 11U) & 0x1U) << 20U | ((Bits >> 12U) & 0xffU) << 12U;
}

[[nodiscard]] std::uint16_t encodeRVCJumpImmediate(std::int64_t Value) noexcept {
  const auto Bits = static_cast<std::uint16_t>(Value);
  return static_cast<std::uint16_t>(((Bits >> 11U) & 1U) << 12U | ((Bits >> 4U) & 1U) << 11U |
                                    ((Bits >> 8U) & 3U) << 9U | ((Bits >> 10U) & 1U) << 8U |
                                    ((Bits >> 6U) & 1U) << 7U | ((Bits >> 7U) & 1U) << 6U |
                                    ((Bits >> 1U) & 7U) << 3U | ((Bits >> 5U) & 1U) << 2U);
}

[[nodiscard]] std::uint16_t encodeRVCBranchImmediate(std::int64_t Value) noexcept {
  const auto Bits = static_cast<std::uint16_t>(Value);
  return static_cast<std::uint16_t>(((Bits >> 8U) & 1U) << 12U | ((Bits >> 3U) & 3U) << 10U |
                                    ((Bits >> 6U) & 3U) << 5U | ((Bits >> 1U) & 3U) << 3U |
                                    ((Bits >> 5U) & 1U) << 2U);
}

[[nodiscard]] Expected<void> requireRelocationTargetKind(const mc::MCValue &Value,
                                                         std::uint32_t Expected,
                                                         std::string_view Name) {
  if (Value.TargetKind != Expected) {
    return makeUnexpected(ErrorCode::RelocationFailed,
                          std::string(Name) + " has an incompatible target expression");
  }
  return {};
}

} // namespace

Expected<mc::MCFixupKindInfo> RISCVAsmBackend::fixupKindInfo(mc::MCFixupKind Kind) const {
  switch (Kind) {
  case mc::fixup_kind::Data1:
    return mc::MCFixupKindInfo{.StorageSize = 1, .PCRelative = false};
  case mc::fixup_kind::Data2:
    return mc::MCFixupKindInfo{.StorageSize = 2, .PCRelative = false};
  case mc::fixup_kind::Data4:
    return mc::MCFixupKindInfo{.StorageSize = 4, .PCRelative = false};
  case mc::fixup_kind::Data8:
    return mc::MCFixupKindInfo{.StorageSize = 8, .PCRelative = false};
  case mc::fixup_kind::PCRelative4:
    return mc::MCFixupKindInfo{.StorageSize = 4, .PCRelative = true};
  default:
    break;
  }
  if (isTargetKind(Kind, RISCVFixupKind::RVCJump) ||
      isTargetKind(Kind, RISCVFixupKind::RVCBranch)) {
    return mc::MCFixupKindInfo{.StorageSize = 2, .PCRelative = true};
  }
  if (Kind >= fixupKind(RISCVFixupKind::High20) && Kind <= fixupKind(RISCVFixupKind::CallPLT)) {
    const bool PCRelative = Kind >= fixupKind(RISCVFixupKind::PCRelativeHigh20);
    return mc::MCFixupKindInfo{.StorageSize = 4, .PCRelative = PCRelative};
  }
  return makeUnexpected(ErrorCode::InvalidArgument, "unknown RISC-V fixup kind");
}

Expected<void> RISCVAsmBackend::applyFixup(mc::MCFixupKind Kind, std::span<std::uint8_t> Data,
                                           std::uint64_t Offset, std::int64_t Value) const {
  auto Info = fixupKindInfo(Kind);
  if (!Info)
    return std::unexpected(Info.error());
  auto Storage = requireStorage(Data, Offset, Info->StorageSize);
  if (!Storage)
    return std::unexpected(Storage.error());

  if (Kind == mc::fixup_kind::Data1 || Kind == mc::fixup_kind::Data2 ||
      Kind == mc::fixup_kind::Data4 || Kind == mc::fixup_kind::Data8 ||
      Kind == mc::fixup_kind::PCRelative4) {
    writeLittleEndian(Data, Offset, static_cast<std::uint64_t>(Value), Info->StorageSize);
    return {};
  }

  if (Info->StorageSize == 2) {
    std::uint16_t Instruction = readHalf(Data, Offset);
    if (isTargetKind(Kind, RISCVFixupKind::RVCJump)) {
      auto Range = requireDisplacement(Value, 12, "RISC-V compressed jump");
      if (!Range)
        return std::unexpected(Range.error());
      Instruction = static_cast<std::uint16_t>((Instruction & ~std::uint16_t{0x1ffc}) |
                                               encodeRVCJumpImmediate(Value));
    } else {
      auto Range = requireDisplacement(Value, 9, "RISC-V compressed branch");
      if (!Range)
        return std::unexpected(Range.error());
      Instruction = static_cast<std::uint16_t>((Instruction & ~std::uint16_t{0x1c7c}) |
                                               encodeRVCBranchImmediate(Value));
    }
    writeLittleEndian(Data, Offset, Instruction, 2);
    return {};
  }

  std::uint32_t Instruction = readWord(Data, Offset);
  if (isTargetKind(Kind, RISCVFixupKind::High20) ||
      isTargetKind(Kind, RISCVFixupKind::PCRelativeHigh20) ||
      isTargetKind(Kind, RISCVFixupKind::GOTPCRelativeHigh20) ||
      isTargetKind(Kind, RISCVFixupKind::CallPLT)) {
    const std::uint64_t Adjusted = static_cast<std::uint64_t>(Value) + 0x800U;
    const std::uint32_t High = static_cast<std::uint32_t>(Adjusted >> 12U);
    Instruction = (Instruction & 0xfffU) | (High << 12U);
  } else if (isTargetKind(Kind, RISCVFixupKind::Low12I) ||
             isTargetKind(Kind, RISCVFixupKind::PCRelativeLow12I)) {
    Instruction = (Instruction & 0x000fffffU) | (static_cast<std::uint32_t>(Value) & 0xfffU) << 20U;
  } else if (isTargetKind(Kind, RISCVFixupKind::Low12S) ||
             isTargetKind(Kind, RISCVFixupKind::PCRelativeLow12S)) {
    const std::uint32_t Low = static_cast<std::uint32_t>(Value) & 0xfffU;
    Instruction = (Instruction & ~std::uint32_t{0xfe000f80}) | ((Low >> 5U) & 0x7fU) << 25U |
                  (Low & 0x1fU) << 7U;
  } else if (isTargetKind(Kind, RISCVFixupKind::JAL)) {
    auto Range = requireDisplacement(Value, 21, "RISC-V JAL");
    if (!Range)
      return std::unexpected(Range.error());
    Instruction = (Instruction & 0xfffU) | encodeJALImmediate(Value);
  } else if (isTargetKind(Kind, RISCVFixupKind::Branch)) {
    auto Range = requireDisplacement(Value, 13, "RISC-V branch");
    if (!Range)
      return std::unexpected(Range.error());
    Instruction = (Instruction & ~std::uint32_t{0xfe000f80}) | encodeBranchImmediate(Value);
  } else {
    return makeUnexpected(ErrorCode::FixupApplicationFailed,
                          "RISC-V fixup cannot be applied to an instruction");
  }
  writeLittleEndian(Data, Offset, Instruction, 4);
  return {};
}

Expected<std::uint32_t> RISCVAsmBackend::relocationType(const mc::MCFixup &Fixup,
                                                        const mc::MCValue &Value) const {
  const auto Relocation = [](RISCVRelocation Type) { return static_cast<std::uint32_t>(Type); };
  switch (Fixup.Kind) {
  case mc::fixup_kind::Data4:
    return Relocation(RISCVRelocation::Relocation32);
  case mc::fixup_kind::Data8:
    return Relocation(RISCVRelocation::Relocation64);
  case mc::fixup_kind::PCRelative4:
    return Relocation(RISCVRelocation::PCRelative32);
  default:
    break;
  }

  if (isTargetKind(Fixup.Kind, RISCVFixupKind::PCRelativeHigh20)) {
    auto Kind = requireRelocationTargetKind(
        Value, static_cast<std::uint32_t>(RISCVMCExprKind::PCRelativeHigh), "R_RISCV_PCREL_HI20");
    if (!Kind)
      return std::unexpected(Kind.error());
    return Relocation(RISCVRelocation::PCRelativeHigh20);
  }
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::GOTPCRelativeHigh20)) {
    auto Kind = requireRelocationTargetKind(
        Value, static_cast<std::uint32_t>(RISCVMCExprKind::GOTPCRelativeHigh), "R_RISCV_GOT_HI20");
    if (!Kind)
      return std::unexpected(Kind.error());
    return Relocation(RISCVRelocation::GOTHigh20);
  }
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::PCRelativeLow12I) ||
      isTargetKind(Fixup.Kind, RISCVFixupKind::PCRelativeLow12S)) {
    auto Kind = requireRelocationTargetKind(
        Value, static_cast<std::uint32_t>(RISCVMCExprKind::PCRelativeLow), "R_RISCV_PCREL_LO12");
    if (!Kind)
      return std::unexpected(Kind.error());
    return Relocation(isTargetKind(Fixup.Kind, RISCVFixupKind::PCRelativeLow12I)
                          ? RISCVRelocation::PCRelativeLow12I
                          : RISCVRelocation::PCRelativeLow12S);
  }
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::CallPLT)) {
    auto Kind = requireRelocationTargetKind(
        Value, static_cast<std::uint32_t>(RISCVMCExprKind::CallPLT), "R_RISCV_CALL_PLT");
    if (!Kind)
      return std::unexpected(Kind.error());
    return Relocation(RISCVRelocation::CallPLT);
  }
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::High20)) {
    if (Value.TargetKind != 0) {
      auto Kind = requireRelocationTargetKind(
          Value, static_cast<std::uint32_t>(RISCVMCExprKind::AbsoluteHigh), "R_RISCV_HI20");
      if (!Kind)
        return std::unexpected(Kind.error());
    }
    return Relocation(RISCVRelocation::High20);
  }
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::Low12I) ||
      isTargetKind(Fixup.Kind, RISCVFixupKind::Low12S)) {
    if (Value.TargetKind != 0) {
      auto Kind = requireRelocationTargetKind(
          Value, static_cast<std::uint32_t>(RISCVMCExprKind::AbsoluteLow), "R_RISCV_LO12");
      if (!Kind)
        return std::unexpected(Kind.error());
    }
    return Relocation(isTargetKind(Fixup.Kind, RISCVFixupKind::Low12I) ? RISCVRelocation::Low12I
                                                                       : RISCVRelocation::Low12S);
  }
  if (Value.TargetKind != 0) {
    return makeUnexpected(ErrorCode::RelocationFailed,
                          "RISC-V relocation has an unsupported target expression");
  }
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::JAL))
    return Relocation(RISCVRelocation::JAL);
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::Branch))
    return Relocation(RISCVRelocation::Branch);
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::RVCJump))
    return Relocation(RISCVRelocation::RVCJump);
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::RVCBranch))
    return Relocation(RISCVRelocation::RVCBranch);
  return makeUnexpected(ErrorCode::RelocationFailed, "RISC-V fixup has no ELF relocation type");
}

Expected<std::vector<mc::MCFixupRelocation>>
RISCVAsmBackend::relocationsForFixup(const mc::MCFixup &Fixup, const mc::MCValue &Value) const {
  if (!Value.SubtractSymbol)
    return mc::MCAsmBackend::relocationsForFixup(Fixup, Value);
  if (!Value.AddSymbol || Value.TargetKind != 0) {
    return makeUnexpected(ErrorCode::RelocationFailed,
                          "RISC-V symbol difference has an invalid relocation expression");
  }

  RISCVRelocation AddType;
  RISCVRelocation SubtractType;
  switch (Fixup.Kind) {
  case mc::fixup_kind::Data1:
    AddType = RISCVRelocation::Add8;
    SubtractType = RISCVRelocation::Subtract8;
    break;
  case mc::fixup_kind::Data2:
    AddType = RISCVRelocation::Add16;
    SubtractType = RISCVRelocation::Subtract16;
    break;
  case mc::fixup_kind::Data4:
    AddType = RISCVRelocation::Add32;
    SubtractType = RISCVRelocation::Subtract32;
    break;
  case mc::fixup_kind::Data8:
    AddType = RISCVRelocation::Add64;
    SubtractType = RISCVRelocation::Subtract64;
    break;
  default:
    return makeUnexpected(ErrorCode::RelocationFailed,
                          "RISC-V fixup cannot encode a symbol difference");
  }
  return std::vector<mc::MCFixupRelocation>{
      {.Type = static_cast<std::uint32_t>(AddType),
       .Symbol = Value.AddSymbol,
       .Addend = Value.Addend},
      {.Type = static_cast<std::uint32_t>(SubtractType),
       .Symbol = Value.SubtractSymbol,
       .Addend = 0},
  };
}

bool RISCVAsmBackend::mayNeedRelaxation(const mc::MCInst &Instruction) const noexcept {
  const auto Opcode = static_cast<InstructionOpcode>(Instruction.opcode().value());
  return Opcode == InstructionOpcode::C_J || Opcode == InstructionOpcode::C_BEQZ ||
         Opcode == InstructionOpcode::C_BNEZ;
}

Expected<bool> RISCVAsmBackend::fixupNeedsRelaxation(const mc::MCFixup &Fixup,
                                                     std::int64_t Value) const {
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::RVCJump))
    return !fitsSigned(Value, 12) || (Value & 1) != 0;
  if (isTargetKind(Fixup.Kind, RISCVFixupKind::RVCBranch))
    return !fitsSigned(Value, 9) || (Value & 1) != 0;
  return false;
}

Expected<mc::MCInst> RISCVAsmBackend::relaxInstruction(const mc::MCInst &Instruction) const {
  const auto Opcode = static_cast<InstructionOpcode>(Instruction.opcode().value());
  const bool IsJump = Opcode == InstructionOpcode::C_J;
  const bool IsBranch = Opcode == InstructionOpcode::C_BEQZ || Opcode == InstructionOpcode::C_BNEZ;
  const std::size_t ExpectedOperands = IsJump ? 1U : 2U;
  if ((!IsJump && !IsBranch) || Instruction.operands().size() != ExpectedOperands) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V MC relaxation received an unsupported instruction");
  }

  const mc::MCOperand &Target = Instruction.operands().back();
  if (Target.expressionValue() == nullptr) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V MC relaxation requires a symbolic target");
  }
  const InstructionOpcode RelaxedOpcode =
      IsJump
          ? InstructionOpcode::JAL
          : (Opcode == InstructionOpcode::C_BEQZ ? InstructionOpcode::BEQ : InstructionOpcode::BNE);
  mc::MCInst Result(mc::MCOpcode::fromValue(static_cast<std::uint32_t>(RelaxedOpcode)));
  Result.setFlags(Instruction.flags());
  Result.setSourceIdentity(Instruction.sourceIdentity());
  if (IsJump) {
    Result.addOperand(mc::MCOperand::registerOperand(
        mc::MCRegister::fromValue(static_cast<std::uint32_t>(PhysicalRegisterID::X0))));
  } else {
    Result.addOperand(Instruction.operands().front());
    Result.addOperand(mc::MCOperand::registerOperand(
        mc::MCRegister::fromValue(static_cast<std::uint32_t>(PhysicalRegisterID::X0))));
  }
  Result.addOperand(Target);
  return Result;
}

std::uint32_t RISCVAsmBackend::objectFlags() const noexcept {
  constexpr std::uint32_t RVC = 1;
  constexpr std::uint32_t FloatABIDouble = 4;
  return RVC | FloatABIDouble;
}

} // namespace rcc::riscv
