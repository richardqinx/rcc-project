// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVInstPrinter.h"

#include "rcc/CodeGen/Register.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCInstPrinter.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace rcc::riscv {
namespace {

struct LinearFormat final {
  std::string_view Name;
  std::size_t OperandCount = 0;
};

constexpr std::array LinearFormats{
    LinearFormat{"none", 0},
    LinearFormat{"rd-imm", 2},
    LinearFormat{"rd-rs1-imm", 3},
    LinearFormat{"rd-rs1-rs2", 3},
    LinearFormat{"rs1-rs2-target", 3},
    LinearFormat{"rd-target", 2},
    LinearFormat{"imm-imm", 2},
    LinearFormat{"rd-csr-rs1", 3},
    LinearFormat{"rd-csr-imm", 3},
    LinearFormat{"frd-frs1-frs2-rm", 4},
    LinearFormat{"frd-frs1-frs2-frs3-rm", 5},
    LinearFormat{"frd-frs1-rm", 3},
    LinearFormat{"frd-frs1-frs2", 3},
    LinearFormat{"rd-frs1-rm", 3},
    LinearFormat{"rd-frs1", 2},
    LinearFormat{"rd-frs1-frs2", 3},
    LinearFormat{"frd-rs1-rm", 3},
    LinearFormat{"frd-rs1", 2},
    LinearFormat{"target", 1},
    LinearFormat{"rs1-target", 2},
    LinearFormat{"rs1", 1},
    LinearFormat{"rd-rs2", 2},
    LinearFormat{"rd-rs1", 2},
};

[[nodiscard]] const AssemblyFormatInfo *assemblyFormat(AssemblyFormatID ID) noexcept {
  const auto Formats = assemblyFormats();
  const auto Position = std::ranges::find(Formats, ID, &AssemblyFormatInfo::ID);
  return Position == Formats.end() ? nullptr : &*Position;
}

[[nodiscard]] std::optional<std::size_t> linearOperandCount(std::string_view Name) noexcept {
  const auto *const Position = std::ranges::find(LinearFormats, Name, &LinearFormat::Name);
  return Position == LinearFormats.end() ? std::nullopt : std::optional(Position->OperandCount);
}

[[nodiscard]] Expected<void> requireOperandCount(const mc::MCInst &Instruction,
                                                 std::size_t ExpectedCount) {
  if (Instruction.operands().size() != ExpectedCount) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V assembly format has an unexpected operand count");
  }
  return {};
}

[[nodiscard]] std::string hexValue(std::uint64_t Value) {
  std::array<char, 16> Digits{};
  const auto [End, Error] = std::to_chars(Digits.data(), Digits.data() + Digits.size(), Value, 16);
  if (Error != std::errc{})
    return {};
  return "0x" + std::string(Digits.data(), End);
}

[[nodiscard]] Expected<std::string> formatOperand(const mc::MCOperand &Operand,
                                                  const TargetDescription &Target,
                                                  const mc::MCInstPrinter &Printer) {
  if (const mc::MCRegister *Register = Operand.registerValue(); Register != nullptr) {
    auto Physical = PhysicalRegister::create(Register->value());
    if (!Physical)
      return std::unexpected(Physical.error());
    const auto Name = Target.registers().physicalRegisterMIRName(*Physical);
    if (!Name) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V MC instruction references an unknown register");
    }
    std::string AssemblyName(*Name);
    if (AssemblyName.ends_with("_f") || AssemblyName.ends_with("_d"))
      AssemblyName.resize(AssemblyName.size() - 2);
    return AssemblyName;
  }
  if (const std::int64_t *Immediate = Operand.immediateValue(); Immediate != nullptr)
    return std::to_string(*Immediate);
  if (const std::uint32_t *Bits = Operand.float32BitsValue(); Bits != nullptr)
    return hexValue(*Bits);
  if (const std::uint64_t *Bits = Operand.float64BitsValue(); Bits != nullptr)
    return hexValue(*Bits);
  if (const mc::MCExprRef *Expression = Operand.expressionValue(); Expression != nullptr) {
    if (!*Expression)
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "RISC-V MC instruction has a null expression operand");
    return mc::printMCExpression(**Expression, Printer);
  }
  return makeUnexpected(ErrorCode::InternalError, "RISC-V MC operand storage is invalid");
}

[[nodiscard]] Expected<std::string> formatRegisterOperand(const mc::MCInst &Instruction,
                                                          std::size_t Index,
                                                          const TargetDescription &Target,
                                                          const mc::MCInstPrinter &Printer) {
  if (Index >= Instruction.operands().size() ||
      Instruction.operands()[Index].registerValue() == nullptr) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V assembly format requires a register operand");
  }
  return formatOperand(Instruction.operands()[Index], Target, Printer);
}

[[nodiscard]] Expected<std::string> formatAnyOperand(const mc::MCInst &Instruction,
                                                     std::size_t Index,
                                                     const TargetDescription &Target,
                                                     const mc::MCInstPrinter &Printer) {
  if (Index >= Instruction.operands().size()) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V assembly format references an absent operand");
  }
  return formatOperand(Instruction.operands()[Index], Target, Printer);
}

[[nodiscard]] Expected<std::string> formatAddress(const mc::MCInst &Instruction,
                                                  std::size_t OffsetIndex, std::size_t BaseIndex,
                                                  const TargetDescription &Target,
                                                  const mc::MCInstPrinter &Printer) {
  auto Offset = formatAnyOperand(Instruction, OffsetIndex, Target, Printer);
  if (!Offset)
    return std::unexpected(Offset.error());
  auto Base = formatRegisterOperand(Instruction, BaseIndex, Target, Printer);
  if (!Base)
    return std::unexpected(Base.error());
  return *Offset + "(" + *Base + ")";
}

[[nodiscard]] Expected<std::string> joinOperands(const mc::MCInst &Instruction, std::size_t Count,
                                                 const TargetDescription &Target,
                                                 const mc::MCInstPrinter &Printer) {
  auto CountResult = requireOperandCount(Instruction, Count);
  if (!CountResult)
    return std::unexpected(CountResult.error());
  std::string Result;
  for (std::size_t Index = 0; Index < Count; ++Index) {
    auto Operand = formatAnyOperand(Instruction, Index, Target, Printer);
    if (!Operand)
      return std::unexpected(Operand.error());
    if (!Result.empty())
      Result += ", ";
    Result += *Operand;
  }
  return Result;
}

} // namespace

Expected<std::string> RISCVInstPrinter::printInstruction(const mc::MCInst &Instruction) const {
  const AssemblyInstructionInfo *Assembly =
      assemblyInstruction(Opcode::fromValue(Instruction.opcode().value()));
  if (Assembly == nullptr || Assembly->Mnemonic.empty()) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V instruction has no generated assembly spelling");
  }
  const AssemblyFormatInfo *Format = assemblyFormat(Assembly->Format);
  if (Format == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V instruction references an unknown assembly format");
  }

  std::string Operands;
  if (const auto LinearCount = linearOperandCount(Format->Name)) {
    auto Joined = joinOperands(Instruction, *LinearCount, *Target, *this);
    if (!Joined)
      return std::unexpected(Joined.error());
    Operands = std::move(*Joined);
  } else if (Format->Name == "rd-offset-rs1" || Format->Name == "frd-offset-rs1" ||
             Format->Name == "rs2-offset-rs1" || Format->Name == "frs2-offset-rs1") {
    auto Count = requireOperandCount(Instruction, 3);
    if (!Count)
      return std::unexpected(Count.error());
    auto First = formatRegisterOperand(Instruction, 0, *Target, *this);
    if (!First)
      return std::unexpected(First.error());
    auto Address = formatAddress(Instruction, 2, 1, *Target, *this);
    if (!Address)
      return std::unexpected(Address.error());
    Operands = *First + ", " + *Address;
  } else if (Format->Name == "rd-paren-rs1") {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = formatRegisterOperand(Instruction, 0, *Target, *this);
    if (!Destination)
      return std::unexpected(Destination.error());
    auto Base = formatRegisterOperand(Instruction, 1, *Target, *this);
    if (!Base)
      return std::unexpected(Base.error());
    Operands = *Destination + ", (" + *Base + ")";
  } else if (Format->Name == "rd-rs2-paren-rs1") {
    auto Count = requireOperandCount(Instruction, 3);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = formatRegisterOperand(Instruction, 0, *Target, *this);
    if (!Destination)
      return std::unexpected(Destination.error());
    auto Value = formatRegisterOperand(Instruction, 1, *Target, *this);
    if (!Value)
      return std::unexpected(Value.error());
    auto Base = formatRegisterOperand(Instruction, 2, *Target, *this);
    if (!Base)
      return std::unexpected(Base.error());
    Operands = *Destination + ", " + *Value + ", (" + *Base + ")";
  } else if (Format->Name == "imm-sp") {
    auto Count = requireOperandCount(Instruction, 1);
    if (!Count)
      return std::unexpected(Count.error());
    auto Immediate = formatAnyOperand(Instruction, 0, *Target, *this);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    Operands = "sp, " + *Immediate;
  } else if (Format->Name == "rd-imm-sp") {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = formatRegisterOperand(Instruction, 0, *Target, *this);
    if (!Destination)
      return std::unexpected(Destination.error());
    auto Immediate = formatAnyOperand(Instruction, 1, *Target, *this);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    Operands = *Destination + ", sp, " + *Immediate;
  } else if (Format->Name == "rd-offset-sp" || Format->Name == "frd-offset-sp" ||
             Format->Name == "rs2-offset-sp" || Format->Name == "frs2-offset-sp") {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto First = formatRegisterOperand(Instruction, 0, *Target, *this);
    if (!First)
      return std::unexpected(First.error());
    auto Offset = formatAnyOperand(Instruction, 1, *Target, *this);
    if (!Offset)
      return std::unexpected(Offset.error());
    Operands = *First + ", " + *Offset + "(sp)";
  } else {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "RISC-V assembly printer does not support generated format '" +
                              std::string(Format->Name) + "'");
  }

  std::string Result(Assembly->Mnemonic);
  if (!Operands.empty())
    Result += " " + Operands;
  return Result;
}

Expected<std::string> RISCVInstPrinter::printTargetExpression(std::uint32_t Kind,
                                                              std::string_view Operand) const {
  switch (static_cast<RISCVMCExprKind>(Kind)) {
  case RISCVMCExprKind::CallPLT:
    return std::string(Operand);
  case RISCVMCExprKind::PCRelativeHigh:
    return "%pcrel_hi(" + std::string(Operand) + ")";
  case RISCVMCExprKind::PCRelativeLow:
    return "%pcrel_lo(" + std::string(Operand) + ")";
  case RISCVMCExprKind::AbsoluteHigh:
    return "%hi(" + std::string(Operand) + ")";
  case RISCVMCExprKind::AbsoluteLow:
    return "%lo(" + std::string(Operand) + ")";
  }
  return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                        "RISC-V MC expression has an unknown target kind");
}

} // namespace rcc::riscv
