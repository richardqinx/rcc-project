// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVMCCodeEmitter.h"

#include "rcc/CodeGen/Register.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/Target/RISCV/RISCVFixupKinds.h"
#include "rcc/Target/RISCV/RISCVInstPrinter.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rcc::riscv {
namespace {

[[nodiscard]] Expected<void> requireOperandCount(const mc::MCInst &Instruction, std::size_t Count) {
  if (Instruction.operands().size() != Count) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V instruction has an unexpected encoding operand count");
  }
  return {};
}

[[nodiscard]] Expected<std::uint32_t> registerEncoding(const mc::MCInst &Instruction,
                                                       std::size_t Index,
                                                       const TargetDescription &Target) {
  if (Index >= Instruction.operands().size()) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V encoding references an absent register operand");
  }
  const mc::MCRegister *Register = Instruction.operands()[Index].registerValue();
  if (Register == nullptr) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V encoding requires a register operand");
  }
  auto Physical = PhysicalRegister::create(Register->value());
  if (!Physical)
    return std::unexpected(Physical.error());
  const PhysicalRegisterDesc *Description = Target.registers().physicalRegister(*Physical);
  if (Description == nullptr || Description->Encoding >= 32) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V encoding references an unknown physical register");
  }
  return Description->Encoding;
}

[[nodiscard]] Expected<void> validateRegisterOperands(const mc::MCInst &Instruction,
                                                      const InstructionDescriptor &Descriptor,
                                                      const TargetDescription &Target) {
  for (std::size_t Index = 0; Index < Instruction.operands().size(); ++Index) {
    const mc::MCRegister *Register = Instruction.operands()[Index].registerValue();
    if (Register == nullptr)
      continue;
    if (Index >= Descriptor.Operands.size()) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "RISC-V register operand has no target descriptor");
    }
    auto Physical = PhysicalRegister::create(Register->value());
    if (!Physical)
      return std::unexpected(Physical.error());
    const OperandDescriptor &Operand = Descriptor.Operands[Index];
    if (Operand.RegClass &&
        !Target.registers().registerClassContains(*Operand.RegClass, *Physical)) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "RISC-V register is outside its required register class");
    }
    if (Operand.RegBank && !Target.registers().bankContainsRegister(*Operand.RegBank, *Physical)) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "RISC-V register is outside its required register bank");
    }
  }
  return {};
}

[[nodiscard]] Expected<std::uint32_t> compactRegisterEncoding(const mc::MCInst &Instruction,
                                                              std::size_t Index,
                                                              const TargetDescription &Target) {
  auto Encoding = registerEncoding(Instruction, Index, Target);
  if (!Encoding)
    return std::unexpected(Encoding.error());
  if (*Encoding < 8 || *Encoding > 15) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "compressed RISC-V instruction requires an x8-x15 register");
  }
  return *Encoding - 8U;
}

[[nodiscard]] bool fitsSigned(std::int64_t Value, unsigned Bits) noexcept {
  const std::int64_t Minimum = -(std::int64_t{1} << (Bits - 1U));
  const std::int64_t Maximum = (std::int64_t{1} << (Bits - 1U)) - 1;
  return Value >= Minimum && Value <= Maximum;
}

[[nodiscard]] bool fitsUnsigned(std::int64_t Value, unsigned Bits) noexcept {
  return Value >= 0 && static_cast<std::uint64_t>(Value) < (std::uint64_t{1} << Bits);
}

[[nodiscard]] Expected<void> requireSigned(std::int64_t Value, unsigned Bits,
                                           std::string_view Name) {
  if (!fitsSigned(Value, Bits)) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          std::string(Name) + " immediate is out of range");
  }
  return {};
}

[[nodiscard]] Expected<void> requireUnsigned(std::int64_t Value, unsigned Bits,
                                             std::string_view Name) {
  if (!fitsUnsigned(Value, Bits)) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          std::string(Name) + " immediate is out of range");
  }
  return {};
}

[[nodiscard]] Expected<void> requireRoundingMode(std::int64_t Value) {
  if ((Value >= 0 && Value <= 4) || Value == 7)
    return {};
  return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                        "RISC-V rounding mode is not an ISA-defined value");
}

[[nodiscard]] Expected<void> requireAligned(std::int64_t Value, std::uint64_t Alignment,
                                            std::string_view Name) {
  if (Value % static_cast<std::int64_t>(Alignment) != 0) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          std::string(Name) + " immediate is not aligned");
  }
  return {};
}

[[nodiscard]] Expected<std::uint32_t> expressionTargetKind(const mc::MCOperand &Operand) {
  const mc::MCExprRef *Expression = Operand.expressionValue();
  if (Expression == nullptr)
    return 0U;
  if (!*Expression) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V instruction has a null expression operand");
  }
  auto Value = mc::evaluateAsRelocatable(**Expression);
  if (!Value)
    return std::unexpected(Value.error());
  return Value->TargetKind;
}

[[nodiscard]] Expected<std::int64_t> immediateOrFixup(const mc::MCInst &Instruction,
                                                      std::size_t Index, mc::MCFixupKind Kind,
                                                      bool PCRelative,
                                                      mc::MCEncodedInstruction &Result) {
  if (Index >= Instruction.operands().size()) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V encoding references an absent immediate operand");
  }
  const mc::MCOperand &Operand = Instruction.operands()[Index];
  if (const std::int64_t *Immediate = Operand.immediateValue(); Immediate != nullptr)
    return *Immediate;
  const mc::MCExprRef *Expression = Operand.expressionValue();
  if (Expression == nullptr || !*Expression) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V encoding requires an immediate or expression operand");
  }
  if (const auto Absolute = (*Expression)->evaluateAbsolute())
    return *Absolute;
  if (Kind == mc::fixup_kind::None) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V instruction does not permit a symbolic immediate here");
  }
  Result.Fixups.push_back(mc::MCFixup{.Offset = 0,
                                      .Value = *Expression,
                                      .Kind = Kind,
                                      .PCRelative = PCRelative,
                                      .LinkerRelaxable = false});
  return 0;
}

[[nodiscard]] Expected<mc::MCFixupKind> uFixupKind(const mc::MCOperand &Operand) {
  auto Kind = expressionTargetKind(Operand);
  if (!Kind)
    return std::unexpected(Kind.error());
  if (*Kind == 0)
    return fixupKind(RISCVFixupKind::High20);
  if (*Kind == static_cast<std::uint32_t>(RISCVMCExprKind::AbsoluteHigh))
    return fixupKind(RISCVFixupKind::High20);
  if (*Kind == static_cast<std::uint32_t>(RISCVMCExprKind::PCRelativeHigh))
    return fixupKind(RISCVFixupKind::PCRelativeHigh20);
  if (*Kind == static_cast<std::uint32_t>(RISCVMCExprKind::CallPLT))
    return fixupKind(RISCVFixupKind::CallPLT);
  return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                        "RISC-V U-type operand has an incompatible target expression");
}

[[nodiscard]] Expected<mc::MCFixupKind> lowFixupKind(const mc::MCOperand &Operand, bool Store) {
  auto Kind = expressionTargetKind(Operand);
  if (!Kind)
    return std::unexpected(Kind.error());
  if (*Kind == 0)
    return fixupKind(Store ? RISCVFixupKind::Low12S : RISCVFixupKind::Low12I);
  if (*Kind == static_cast<std::uint32_t>(RISCVMCExprKind::AbsoluteLow))
    return fixupKind(Store ? RISCVFixupKind::Low12S : RISCVFixupKind::Low12I);
  if (*Kind == static_cast<std::uint32_t>(RISCVMCExprKind::PCRelativeLow)) {
    return fixupKind(Store ? RISCVFixupKind::PCRelativeLow12S : RISCVFixupKind::PCRelativeLow12I);
  }
  return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                        "RISC-V low immediate has an incompatible target expression");
}

[[nodiscard]] Expected<void> requirePlainExpression(const mc::MCOperand &Operand,
                                                    std::string_view Name) {
  auto Kind = expressionTargetKind(Operand);
  if (!Kind)
    return std::unexpected(Kind.error());
  if (*Kind != 0) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          std::string(Name) + " does not accept a target expression modifier");
  }
  return {};
}

[[nodiscard]] std::uint32_t encodeBranchImmediate(std::int64_t Value) noexcept {
  const auto Bits = static_cast<std::uint32_t>(Value);
  return ((Bits >> 12U) & 1U) << 31U | ((Bits >> 5U) & 0x3fU) << 25U | ((Bits >> 1U) & 0xfU) << 8U |
         ((Bits >> 11U) & 1U) << 7U;
}

[[nodiscard]] std::uint32_t encodeJALImmediate(std::int64_t Value) noexcept {
  const auto Bits = static_cast<std::uint32_t>(Value);
  return ((Bits >> 20U) & 1U) << 31U | ((Bits >> 1U) & 0x3ffU) << 21U |
         ((Bits >> 11U) & 1U) << 20U | ((Bits >> 12U) & 0xffU) << 12U;
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

[[nodiscard]] Expected<std::uint32_t> encodeRType(const mc::MCInst &Instruction,
                                                  const TargetDescription &Target,
                                                  const AssemblyInstructionInfo &Assembly) {
  const auto Format = static_cast<AssemblyFormat>(Assembly.Format.value());
  std::size_t RegisterCount = 0;
  bool HasRoundingMode = false;
  switch (Format) {
  case AssemblyFormat::RD_RS1_RS2:
  case AssemblyFormat::FRD_FRS1_FRS2:
  case AssemblyFormat::RD_FRS1_FRS2:
    RegisterCount = 3;
    break;
  case AssemblyFormat::FRD_FRS1_FRS2_RM:
    RegisterCount = 3;
    HasRoundingMode = true;
    break;
  case AssemblyFormat::FRD_FRS1_RM:
  case AssemblyFormat::RD_FRS1_RM:
  case AssemblyFormat::FRD_RS1_RM:
    RegisterCount = 2;
    HasRoundingMode = true;
    break;
  case AssemblyFormat::RD_FRS1:
  case AssemblyFormat::FRD_RS1:
  case AssemblyFormat::RD_RS1:
    RegisterCount = 2;
    break;
  default:
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "unsupported RISC-V R-type assembly format");
  }
  auto Count = requireOperandCount(Instruction, RegisterCount + (HasRoundingMode ? 1U : 0U));
  if (!Count)
    return std::unexpected(Count.error());

  std::uint32_t Encoding = Assembly.Encoding & ~std::uint32_t{0x000f8f80};
  auto Destination = registerEncoding(Instruction, 0, Target);
  auto Source1 = registerEncoding(Instruction, 1, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  if (!Source1)
    return std::unexpected(Source1.error());
  Encoding |= *Destination << 7U | *Source1 << 15U;
  if (RegisterCount == 3) {
    auto Source2 = registerEncoding(Instruction, 2, Target);
    if (!Source2)
      return std::unexpected(Source2.error());
    Encoding = (Encoding & ~std::uint32_t{0x01f00000}) | *Source2 << 20U;
  }
  return Encoding;
}

[[nodiscard]] Expected<std::uint32_t> encodeRTypeWithResult(const mc::MCInst &Instruction,
                                                            const TargetDescription &Target,
                                                            const AssemblyInstructionInfo &Assembly,
                                                            mc::MCEncodedInstruction &Result) {
  auto Encoding = encodeRType(Instruction, Target, Assembly);
  if (!Encoding)
    return std::unexpected(Encoding.error());
  const auto Format = static_cast<AssemblyFormat>(Assembly.Format.value());
  std::size_t ModeIndex = 0;
  switch (Format) {
  case AssemblyFormat::FRD_FRS1_FRS2_RM:
    ModeIndex = 3;
    break;
  case AssemblyFormat::FRD_FRS1_RM:
  case AssemblyFormat::RD_FRS1_RM:
  case AssemblyFormat::FRD_RS1_RM:
    ModeIndex = 2;
    break;
  default:
    return *Encoding;
  }
  auto Mode = immediateOrFixup(Instruction, ModeIndex, mc::fixup_kind::None, false, Result);
  if (!Mode)
    return std::unexpected(Mode.error());
  auto Range = requireRoundingMode(*Mode);
  if (!Range)
    return std::unexpected(Range.error());
  return (*Encoding & ~std::uint32_t{0x7000}) | static_cast<std::uint32_t>(*Mode) << 12U;
}

[[nodiscard]] Expected<std::uint32_t> encodeIType(const mc::MCInst &Instruction,
                                                  const TargetDescription &Target,
                                                  const AssemblyInstructionInfo &Assembly,
                                                  mc::MCEncodedInstruction &Result) {
  const auto Format = static_cast<AssemblyFormat>(Assembly.Format.value());
  if (Format == AssemblyFormat::NONE) {
    auto Count = requireOperandCount(Instruction, 0);
    if (!Count)
      return std::unexpected(Count.error());
    return Assembly.Encoding;
  }
  if (Format == AssemblyFormat::IMM_IMM) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Predecessor = immediateOrFixup(Instruction, 0, mc::fixup_kind::None, false, Result);
    auto Successor = immediateOrFixup(Instruction, 1, mc::fixup_kind::None, false, Result);
    if (!Predecessor)
      return std::unexpected(Predecessor.error());
    if (!Successor)
      return std::unexpected(Successor.error());
    auto PredRange = requireUnsigned(*Predecessor, 4, "RISC-V fence predecessor");
    auto SuccRange = requireUnsigned(*Successor, 4, "RISC-V fence successor");
    if (!PredRange)
      return std::unexpected(PredRange.error());
    if (!SuccRange)
      return std::unexpected(SuccRange.error());
    return (Assembly.Encoding & ~std::uint32_t{0x0ff00000}) |
           static_cast<std::uint32_t>(*Predecessor) << 24U |
           static_cast<std::uint32_t>(*Successor) << 20U;
  }
  if (Format == AssemblyFormat::RD_RS1) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = registerEncoding(Instruction, 0, Target);
    auto Source = registerEncoding(Instruction, 1, Target);
    if (!Destination)
      return std::unexpected(Destination.error());
    if (!Source)
      return std::unexpected(Source.error());
    return (Assembly.Encoding & ~std::uint32_t{0x000f8f80}) | *Destination << 7U | *Source << 15U;
  }
  if (Format != AssemblyFormat::RD_RS1_IMM && Format != AssemblyFormat::RD_OFFSET_RS1 &&
      Format != AssemblyFormat::FRD_OFFSET_RS1) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "unsupported RISC-V I-type assembly format");
  }
  auto Count = requireOperandCount(Instruction, 3);
  if (!Count)
    return std::unexpected(Count.error());
  auto Destination = registerEncoding(Instruction, 0, Target);
  auto Source = registerEncoding(Instruction, 1, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  if (!Source)
    return std::unexpected(Source.error());
  std::uint32_t Encoding =
      (Assembly.Encoding & ~std::uint32_t{0x000f8f80}) | *Destination << 7U | *Source << 15U;

  const auto Opcode = static_cast<InstructionOpcode>(Instruction.opcode().value());
  const bool Shift64 = Opcode == InstructionOpcode::SLLI || Opcode == InstructionOpcode::SLLI_UW ||
                       Opcode == InstructionOpcode::SRLI || Opcode == InstructionOpcode::SRAI;
  const bool Shift32 = Opcode == InstructionOpcode::SLLIW || Opcode == InstructionOpcode::SRLIW ||
                       Opcode == InstructionOpcode::SRAIW;
  if (Shift64 || Shift32) {
    auto Immediate = immediateOrFixup(Instruction, 2, mc::fixup_kind::None, false, Result);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    auto Range = requireUnsigned(*Immediate, Shift64 ? 6U : 5U, "RISC-V shift");
    if (!Range)
      return std::unexpected(Range.error());
    const std::uint32_t Mask = Shift64 ? 0x03f00000U : 0x01f00000U;
    return (Encoding & ~Mask) | static_cast<std::uint32_t>(*Immediate) << 20U;
  }

  auto Kind = lowFixupKind(Instruction.operands()[2], false);
  if (!Kind)
    return std::unexpected(Kind.error());
  auto Immediate = immediateOrFixup(Instruction, 2, *Kind,
                                    *Kind == fixupKind(RISCVFixupKind::PCRelativeLow12I), Result);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (Instruction.operands()[2].expressionValue() == nullptr) {
    auto Range = requireSigned(*Immediate, 12, "RISC-V I-type");
    if (!Range)
      return std::unexpected(Range.error());
  }
  return (Encoding & ~std::uint32_t{0xfff00000}) | (static_cast<std::uint32_t>(*Immediate) & 0xfffU)
                                                       << 20U;
}

[[nodiscard]] Expected<std::uint32_t> encodeSType(const mc::MCInst &Instruction,
                                                  const TargetDescription &Target,
                                                  const AssemblyInstructionInfo &Assembly,
                                                  mc::MCEncodedInstruction &Result) {
  auto Count = requireOperandCount(Instruction, 3);
  if (!Count)
    return std::unexpected(Count.error());
  auto Source2 = registerEncoding(Instruction, 0, Target);
  auto Source1 = registerEncoding(Instruction, 1, Target);
  if (!Source2)
    return std::unexpected(Source2.error());
  if (!Source1)
    return std::unexpected(Source1.error());
  auto Kind = lowFixupKind(Instruction.operands()[2], true);
  if (!Kind)
    return std::unexpected(Kind.error());
  auto Immediate = immediateOrFixup(Instruction, 2, *Kind,
                                    *Kind == fixupKind(RISCVFixupKind::PCRelativeLow12S), Result);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (Instruction.operands()[2].expressionValue() == nullptr) {
    auto Range = requireSigned(*Immediate, 12, "RISC-V S-type");
    if (!Range)
      return std::unexpected(Range.error());
  }
  const std::uint32_t Bits = static_cast<std::uint32_t>(*Immediate) & 0xfffU;
  return (Assembly.Encoding & ~std::uint32_t{0xffff8f80}) | *Source1 << 15U | *Source2 << 20U |
         ((Bits >> 5U) & 0x7fU) << 25U | (Bits & 0x1fU) << 7U;
}

[[nodiscard]] Expected<std::uint32_t> encodeBType(const mc::MCInst &Instruction,
                                                  const TargetDescription &Target,
                                                  const AssemblyInstructionInfo &Assembly,
                                                  mc::MCEncodedInstruction &Result) {
  auto Count = requireOperandCount(Instruction, 3);
  if (!Count)
    return std::unexpected(Count.error());
  auto Source1 = registerEncoding(Instruction, 0, Target);
  auto Source2 = registerEncoding(Instruction, 1, Target);
  if (!Source1)
    return std::unexpected(Source1.error());
  if (!Source2)
    return std::unexpected(Source2.error());
  auto Plain = requirePlainExpression(Instruction.operands()[2], "RISC-V branch");
  if (!Plain)
    return std::unexpected(Plain.error());
  auto Immediate =
      immediateOrFixup(Instruction, 2, fixupKind(RISCVFixupKind::Branch), true, Result);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (Instruction.operands()[2].expressionValue() == nullptr &&
      (!fitsSigned(*Immediate, 13) || (*Immediate & 1) != 0)) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V branch displacement is out of range or unaligned");
  }
  return (Assembly.Encoding & ~std::uint32_t{0xffff8f80}) | *Source1 << 15U | *Source2 << 20U |
         encodeBranchImmediate(*Immediate);
}

[[nodiscard]] Expected<std::uint32_t> encodeUType(const mc::MCInst &Instruction,
                                                  const TargetDescription &Target,
                                                  const AssemblyInstructionInfo &Assembly,
                                                  mc::MCEncodedInstruction &Result) {
  auto Count = requireOperandCount(Instruction, 2);
  if (!Count)
    return std::unexpected(Count.error());
  auto Destination = registerEncoding(Instruction, 0, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Kind = uFixupKind(Instruction.operands()[1]);
  if (!Kind)
    return std::unexpected(Kind.error());
  const bool PCRelative = *Kind == fixupKind(RISCVFixupKind::PCRelativeHigh20) ||
                          *Kind == fixupKind(RISCVFixupKind::CallPLT);
  auto Immediate = immediateOrFixup(Instruction, 1, *Kind, PCRelative, Result);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (Instruction.operands()[1].expressionValue() == nullptr) {
    auto Range = requireSigned(*Immediate, 20, "RISC-V U-type");
    if (!Range)
      return std::unexpected(Range.error());
  }
  return (Assembly.Encoding & 0x7fU) | *Destination << 7U |
         (static_cast<std::uint32_t>(*Immediate) & 0xfffffU) << 12U;
}

[[nodiscard]] Expected<std::uint32_t> encodeJType(const mc::MCInst &Instruction,
                                                  const TargetDescription &Target,
                                                  const AssemblyInstructionInfo &Assembly,
                                                  mc::MCEncodedInstruction &Result) {
  auto Count = requireOperandCount(Instruction, 2);
  if (!Count)
    return std::unexpected(Count.error());
  auto Destination = registerEncoding(Instruction, 0, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto Plain = requirePlainExpression(Instruction.operands()[1], "RISC-V JAL");
  if (!Plain)
    return std::unexpected(Plain.error());
  auto Immediate = immediateOrFixup(Instruction, 1, fixupKind(RISCVFixupKind::JAL), true, Result);
  if (!Immediate)
    return std::unexpected(Immediate.error());
  if (Instruction.operands()[1].expressionValue() == nullptr &&
      (!fitsSigned(*Immediate, 21) || (*Immediate & 1) != 0)) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V JAL displacement is out of range or unaligned");
  }
  return (Assembly.Encoding & 0x7fU) | *Destination << 7U | encodeJALImmediate(*Immediate);
}

[[nodiscard]] Expected<std::uint32_t> encodeSystem(const mc::MCInst &Instruction,
                                                   const TargetDescription &Target,
                                                   const AssemblyInstructionInfo &Assembly,
                                                   mc::MCEncodedInstruction &Result) {
  const auto Format = static_cast<AssemblyFormat>(Assembly.Format.value());
  if (Format == AssemblyFormat::NONE) {
    auto Count = requireOperandCount(Instruction, 0);
    if (!Count)
      return std::unexpected(Count.error());
    return Assembly.Encoding;
  }
  if (Format != AssemblyFormat::RD_CSR_RS1 && Format != AssemblyFormat::RD_CSR_IMM) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "unsupported RISC-V system assembly format");
  }
  auto Count = requireOperandCount(Instruction, 3);
  if (!Count)
    return std::unexpected(Count.error());
  auto Destination = registerEncoding(Instruction, 0, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  auto CSR = immediateOrFixup(Instruction, 1, mc::fixup_kind::None, false, Result);
  if (!CSR)
    return std::unexpected(CSR.error());
  auto CSRRange = requireUnsigned(*CSR, 12, "RISC-V CSR");
  if (!CSRRange)
    return std::unexpected(CSRRange.error());
  std::uint32_t Source = 0;
  if (Format == AssemblyFormat::RD_CSR_RS1) {
    auto Register = registerEncoding(Instruction, 2, Target);
    if (!Register)
      return std::unexpected(Register.error());
    Source = *Register;
  } else {
    auto Immediate = immediateOrFixup(Instruction, 2, mc::fixup_kind::None, false, Result);
    if (!Immediate)
      return std::unexpected(Immediate.error());
    auto Range = requireUnsigned(*Immediate, 5, "RISC-V CSR immediate");
    if (!Range)
      return std::unexpected(Range.error());
    Source = static_cast<std::uint32_t>(*Immediate);
  }
  return (Assembly.Encoding & ~std::uint32_t{0xffff8f80}) | *Destination << 7U | Source << 15U |
         static_cast<std::uint32_t>(*CSR) << 20U;
}

[[nodiscard]] Expected<std::uint32_t> encodeAMO(const mc::MCInst &Instruction,
                                                const TargetDescription &Target,
                                                const AssemblyInstructionInfo &Assembly) {
  const auto Format = static_cast<AssemblyFormat>(Assembly.Format.value());
  const bool IsLoadReserved = Format == AssemblyFormat::RD_PAREN_RS1;
  if (!IsLoadReserved && Format != AssemblyFormat::RD_RS2_PAREN_RS1) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "unsupported RISC-V atomic assembly format");
  }
  auto Count = requireOperandCount(Instruction, IsLoadReserved ? 2U : 3U);
  if (!Count)
    return std::unexpected(Count.error());
  auto Destination = registerEncoding(Instruction, 0, Target);
  auto Source1 = registerEncoding(Instruction, IsLoadReserved ? 1U : 2U, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  if (!Source1)
    return std::unexpected(Source1.error());
  std::uint32_t Encoding =
      (Assembly.Encoding & ~std::uint32_t{0x000f8f80}) | *Destination << 7U | *Source1 << 15U;
  if (!IsLoadReserved) {
    auto Source2 = registerEncoding(Instruction, 1, Target);
    if (!Source2)
      return std::unexpected(Source2.error());
    Encoding = (Encoding & ~std::uint32_t{0x01f00000}) | *Source2 << 20U;
  }
  return Encoding;
}

[[nodiscard]] Expected<std::uint32_t> encodeR4(const mc::MCInst &Instruction,
                                               const TargetDescription &Target,
                                               const AssemblyInstructionInfo &Assembly,
                                               mc::MCEncodedInstruction &Result) {
  auto Count = requireOperandCount(Instruction, 5);
  if (!Count)
    return std::unexpected(Count.error());
  auto Destination = registerEncoding(Instruction, 0, Target);
  auto Source1 = registerEncoding(Instruction, 1, Target);
  auto Source2 = registerEncoding(Instruction, 2, Target);
  auto Source3 = registerEncoding(Instruction, 3, Target);
  if (!Destination)
    return std::unexpected(Destination.error());
  if (!Source1)
    return std::unexpected(Source1.error());
  if (!Source2)
    return std::unexpected(Source2.error());
  if (!Source3)
    return std::unexpected(Source3.error());
  auto Mode = immediateOrFixup(Instruction, 4, mc::fixup_kind::None, false, Result);
  if (!Mode)
    return std::unexpected(Mode.error());
  auto Range = requireRoundingMode(*Mode);
  if (!Range)
    return std::unexpected(Range.error());
  return (Assembly.Encoding & ~std::uint32_t{0xf9ffff80}) | *Destination << 7U |
         static_cast<std::uint32_t>(*Mode) << 12U | *Source1 << 15U | *Source2 << 20U |
         *Source3 << 27U;
}

[[nodiscard]] Expected<std::uint16_t> encodeCompressed(const mc::MCInst &Instruction,
                                                       const TargetDescription &Target,
                                                       const AssemblyInstructionInfo &Assembly,
                                                       mc::MCEncodedInstruction &Result) {
  const auto Opcode = static_cast<InstructionOpcode>(Instruction.opcode().value());
  std::uint16_t Encoding = static_cast<std::uint16_t>(Assembly.Encoding);
  const auto Immediate = [&](std::size_t Index, mc::MCFixupKind Kind = mc::fixup_kind::None,
                             bool PCRelative = false) {
    return immediateOrFixup(Instruction, Index, Kind, PCRelative, Result);
  };

  if (Opcode == InstructionOpcode::C_ADDI4SPN) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = compactRegisterEncoding(Instruction, 0, Target);
    auto Value = Immediate(1);
    if (!Destination)
      return std::unexpected(Destination.error());
    if (!Value)
      return std::unexpected(Value.error());
    if (!fitsUnsigned(*Value, 10) || *Value == 0) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "C.ADDI4SPN immediate is out of range or zero");
    }
    auto Alignment = requireAligned(*Value, 4, "C.ADDI4SPN");
    if (!Alignment)
      return std::unexpected(Alignment.error());
    const auto Bits = static_cast<std::uint16_t>(*Value);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) |
                                      ((Bits >> 4U) & 3U) << 11U | ((Bits >> 6U) & 0xfU) << 7U |
                                      ((Bits >> 2U) & 1U) << 6U | ((Bits >> 3U) & 1U) << 5U |
                                      *Destination << 2U);
  }

  const bool CompactDoubleLoad =
      Opcode == InstructionOpcode::C_FLD || Opcode == InstructionOpcode::C_LD;
  const bool CompactWordLoad = Opcode == InstructionOpcode::C_LW;
  const bool CompactDoubleStore =
      Opcode == InstructionOpcode::C_FSD || Opcode == InstructionOpcode::C_SD;
  const bool CompactWordStore = Opcode == InstructionOpcode::C_SW;
  if (CompactDoubleLoad || CompactWordLoad || CompactDoubleStore || CompactWordStore) {
    auto Count = requireOperandCount(Instruction, 3);
    if (!Count)
      return std::unexpected(Count.error());
    auto ValueRegister = compactRegisterEncoding(Instruction, 0, Target);
    auto Base = compactRegisterEncoding(Instruction, 1, Target);
    auto Offset = Immediate(2);
    if (!ValueRegister)
      return std::unexpected(ValueRegister.error());
    if (!Base)
      return std::unexpected(Base.error());
    if (!Offset)
      return std::unexpected(Offset.error());
    const bool IsDouble = CompactDoubleLoad || CompactDoubleStore;
    auto Range = requireUnsigned(*Offset, IsDouble ? 8U : 7U, "compressed load/store");
    if (!Range)
      return std::unexpected(Range.error());
    auto Alignment = requireAligned(*Offset, IsDouble ? 8U : 4U, "compressed load/store");
    if (!Alignment)
      return std::unexpected(Alignment.error());
    const auto Bits = static_cast<std::uint16_t>(*Offset);
    std::uint16_t ImmediateBits = static_cast<std::uint16_t>(((Bits >> 3U) & 7U) << 10U);
    ImmediateBits |=
        IsDouble
            ? static_cast<std::uint16_t>(((Bits >> 6U) & 3U) << 5U)
            : static_cast<std::uint16_t>(((Bits >> 2U) & 1U) << 6U | ((Bits >> 6U) & 1U) << 5U);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) | ImmediateBits |
                                      *Base << 7U | *ValueRegister << 2U);
  }

  const bool CI = Opcode == InstructionOpcode::C_ADDI || Opcode == InstructionOpcode::C_ADDIW ||
                  Opcode == InstructionOpcode::C_LI || Opcode == InstructionOpcode::C_LUI ||
                  Opcode == InstructionOpcode::C_SLLI;
  if (CI) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Register = registerEncoding(Instruction, 0, Target);
    auto Value = Immediate(1);
    if (!Register)
      return std::unexpected(Register.error());
    if (!Value)
      return std::unexpected(Value.error());
    const bool IsShift = Opcode == InstructionOpcode::C_SLLI;
    auto Range =
        IsShift ? requireUnsigned(*Value, 6, "C.SLLI") : requireSigned(*Value, 6, "compressed CI");
    if (!Range)
      return std::unexpected(Range.error());
    if (Opcode == InstructionOpcode::C_LUI && *Value == 0) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "C.LUI immediate must be nonzero");
    }
    const auto Bits = static_cast<std::uint16_t>(*Value);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) |
                                      ((Bits >> 5U) & 1U) << 12U | *Register << 7U |
                                      (Bits & 0x1fU) << 2U);
  }

  if (Opcode == InstructionOpcode::C_ADDI16SP) {
    auto Count = requireOperandCount(Instruction, 1);
    if (!Count)
      return std::unexpected(Count.error());
    auto Value = Immediate(0);
    if (!Value)
      return std::unexpected(Value.error());
    if (!fitsSigned(*Value, 10) || *Value == 0) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "C.ADDI16SP immediate is out of range or zero");
    }
    auto Alignment = requireAligned(*Value, 16, "C.ADDI16SP");
    if (!Alignment)
      return std::unexpected(Alignment.error());
    const auto Bits = static_cast<std::uint16_t>(*Value);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x107c}) |
                                      ((Bits >> 9U) & 1U) << 12U | ((Bits >> 4U) & 1U) << 6U |
                                      ((Bits >> 6U) & 1U) << 5U | ((Bits >> 7U) & 3U) << 3U |
                                      ((Bits >> 5U) & 1U) << 2U);
  }

  const bool CompactShift =
      Opcode == InstructionOpcode::C_SRLI || Opcode == InstructionOpcode::C_SRAI;
  if (CompactShift || Opcode == InstructionOpcode::C_ANDI) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Register = compactRegisterEncoding(Instruction, 0, Target);
    auto Value = Immediate(1);
    if (!Register)
      return std::unexpected(Register.error());
    if (!Value)
      return std::unexpected(Value.error());
    auto Range = CompactShift ? requireUnsigned(*Value, 6, "compressed shift")
                              : requireSigned(*Value, 6, "C.ANDI");
    if (!Range)
      return std::unexpected(Range.error());
    const auto Bits = static_cast<std::uint16_t>(*Value);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x13fc}) |
                                      ((Bits >> 5U) & 1U) << 12U | *Register << 7U |
                                      (Bits & 0x1fU) << 2U);
  }

  const bool CompactALU =
      Opcode == InstructionOpcode::C_SUB || Opcode == InstructionOpcode::C_XOR ||
      Opcode == InstructionOpcode::C_OR || Opcode == InstructionOpcode::C_AND ||
      Opcode == InstructionOpcode::C_SUBW || Opcode == InstructionOpcode::C_ADDW;
  if (CompactALU) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = compactRegisterEncoding(Instruction, 0, Target);
    auto Source = compactRegisterEncoding(Instruction, 1, Target);
    if (!Destination)
      return std::unexpected(Destination.error());
    if (!Source)
      return std::unexpected(Source.error());
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x039c}) | *Destination << 7U |
                                      *Source << 2U);
  }

  if (Opcode == InstructionOpcode::C_J) {
    auto Count = requireOperandCount(Instruction, 1);
    if (!Count)
      return std::unexpected(Count.error());
    auto Plain = requirePlainExpression(Instruction.operands()[0], "C.J");
    if (!Plain)
      return std::unexpected(Plain.error());
    auto Value = Immediate(0, fixupKind(RISCVFixupKind::RVCJump), true);
    if (!Value)
      return std::unexpected(Value.error());
    if (Instruction.operands()[0].expressionValue() == nullptr &&
        (!fitsSigned(*Value, 12) || (*Value & 1) != 0)) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "C.J displacement is out of range or unaligned");
    }
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) |
                                      encodeRVCJumpImmediate(*Value));
  }

  if (Opcode == InstructionOpcode::C_BEQZ || Opcode == InstructionOpcode::C_BNEZ) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Source = compactRegisterEncoding(Instruction, 0, Target);
    if (!Source)
      return std::unexpected(Source.error());
    auto Plain = requirePlainExpression(Instruction.operands()[1], "compressed branch");
    if (!Plain)
      return std::unexpected(Plain.error());
    auto Value = Immediate(1, fixupKind(RISCVFixupKind::RVCBranch), true);
    if (!Value)
      return std::unexpected(Value.error());
    if (Instruction.operands()[1].expressionValue() == nullptr &&
        (!fitsSigned(*Value, 9) || (*Value & 1) != 0)) {
      return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                            "compressed branch displacement is out of range or unaligned");
    }
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) | *Source << 7U |
                                      encodeRVCBranchImmediate(*Value));
  }

  const bool StackDoubleLoad =
      Opcode == InstructionOpcode::C_FLDSP || Opcode == InstructionOpcode::C_LDSP;
  const bool StackWordLoad = Opcode == InstructionOpcode::C_LWSP;
  if (StackDoubleLoad || StackWordLoad) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Destination = registerEncoding(Instruction, 0, Target);
    auto Offset = Immediate(1);
    if (!Destination)
      return std::unexpected(Destination.error());
    if (!Offset)
      return std::unexpected(Offset.error());
    auto Range = requireUnsigned(*Offset, StackDoubleLoad ? 9U : 8U, "stack load");
    if (!Range)
      return std::unexpected(Range.error());
    auto Alignment = requireAligned(*Offset, StackDoubleLoad ? 8U : 4U, "stack load");
    if (!Alignment)
      return std::unexpected(Alignment.error());
    const auto Bits = static_cast<std::uint16_t>(*Offset);
    const std::uint16_t ImmediateBits =
        StackDoubleLoad
            ? static_cast<std::uint16_t>(((Bits >> 5U) & 1U) << 12U | ((Bits >> 3U) & 3U) << 5U |
                                         ((Bits >> 6U) & 7U) << 2U)
            : static_cast<std::uint16_t>(((Bits >> 5U) & 1U) << 12U | ((Bits >> 2U) & 7U) << 4U |
                                         ((Bits >> 6U) & 3U) << 2U);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) | ImmediateBits |
                                      *Destination << 7U);
  }

  const bool CRSingle = Opcode == InstructionOpcode::C_JR || Opcode == InstructionOpcode::C_JALR;
  const bool CRDouble = Opcode == InstructionOpcode::C_MV || Opcode == InstructionOpcode::C_ADD;
  if (CRSingle || CRDouble) {
    auto Count = requireOperandCount(Instruction, CRSingle ? 1U : 2U);
    if (!Count)
      return std::unexpected(Count.error());
    auto First = registerEncoding(Instruction, 0, Target);
    if (!First)
      return std::unexpected(First.error());
    Encoding = static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x0f80}) | *First << 7U);
    if (CRDouble) {
      auto Second = registerEncoding(Instruction, 1, Target);
      if (!Second)
        return std::unexpected(Second.error());
      Encoding = static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x007c}) | *Second << 2U);
    }
    return Encoding;
  }

  if (Opcode == InstructionOpcode::C_EBREAK) {
    auto Count = requireOperandCount(Instruction, 0);
    if (!Count)
      return std::unexpected(Count.error());
    return Encoding;
  }

  const bool StackDoubleStore =
      Opcode == InstructionOpcode::C_FSDSP || Opcode == InstructionOpcode::C_SDSP;
  const bool StackWordStore = Opcode == InstructionOpcode::C_SWSP;
  if (StackDoubleStore || StackWordStore) {
    auto Count = requireOperandCount(Instruction, 2);
    if (!Count)
      return std::unexpected(Count.error());
    auto Source = registerEncoding(Instruction, 0, Target);
    auto Offset = Immediate(1);
    if (!Source)
      return std::unexpected(Source.error());
    if (!Offset)
      return std::unexpected(Offset.error());
    auto Range = requireUnsigned(*Offset, StackDoubleStore ? 9U : 8U, "stack store");
    if (!Range)
      return std::unexpected(Range.error());
    auto Alignment = requireAligned(*Offset, StackDoubleStore ? 8U : 4U, "stack store");
    if (!Alignment)
      return std::unexpected(Alignment.error());
    const auto Bits = static_cast<std::uint16_t>(*Offset);
    const std::uint16_t ImmediateBits =
        StackDoubleStore
            ? static_cast<std::uint16_t>(((Bits >> 3U) & 7U) << 10U | ((Bits >> 6U) & 7U) << 7U)
            : static_cast<std::uint16_t>(((Bits >> 2U) & 0xfU) << 9U | ((Bits >> 6U) & 3U) << 7U);
    return static_cast<std::uint16_t>((Encoding & ~std::uint16_t{0x1ffc}) | ImmediateBits |
                                      *Source << 2U);
  }

  return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                        "unsupported compressed RISC-V instruction");
}

void appendLittleEndian(mc::MCEncodedInstruction &Result, std::uint32_t Encoding,
                        std::uint8_t Size) {
  Result.Bytes.reserve(Size);
  for (std::uint8_t Index = 0; Index < Size; ++Index)
    Result.Bytes.push_back(static_cast<std::uint8_t>(Encoding >> (Index * 8U)));
}

} // namespace

Expected<mc::MCEncodedInstruction>
RISCVMCCodeEmitter::encodeInstruction(const mc::MCInst &Instruction) const {
  if (!Instruction.opcode().isValid()) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "cannot encode an invalid RISC-V MC opcode");
  }
  const Opcode OpcodeValue = Opcode::fromValue(Instruction.opcode().value());
  const InstructionDescriptor *Descriptor = Target->instructionDescriptor(OpcodeValue);
  const AssemblyInstructionInfo *Assembly = assemblyInstruction(OpcodeValue);
  if (Descriptor == nullptr || Assembly == nullptr || Assembly->Encoding == 0) {
    return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                          "RISC-V instruction has no binary encoding description");
  }
  auto Registers = validateRegisterOperands(Instruction, *Descriptor, *Target);
  if (!Registers)
    return std::unexpected(Registers.error());

  mc::MCEncodedInstruction Result;
  const auto Category = static_cast<EncodingCategory>(Descriptor->EncodingCategory.value());
  Expected<std::uint32_t> Encoding = makeUnexpected(ErrorCode::InstructionEncodingFailed,
                                                    "RISC-V instruction has no encoding category");
  switch (Category) {
  case EncodingCategory::R:
    Encoding = encodeRTypeWithResult(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::I:
    Encoding = encodeIType(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::S:
    Encoding = encodeSType(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::B:
    Encoding = encodeBType(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::U:
    Encoding = encodeUType(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::J:
    Encoding = encodeJType(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::SYSTEM:
    Encoding = encodeSystem(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::AMO:
    Encoding = encodeAMO(Instruction, *Target, *Assembly);
    break;
  case EncodingCategory::R4:
    Encoding = encodeR4(Instruction, *Target, *Assembly, Result);
    break;
  case EncodingCategory::C: {
    auto Compressed = encodeCompressed(Instruction, *Target, *Assembly, Result);
    if (!Compressed)
      return std::unexpected(Compressed.error());
    appendLittleEndian(Result, *Compressed, 2);
    return Result;
  }
  case EncodingCategory::None:
    break;
  }
  if (!Encoding)
    return std::unexpected(Encoding.error());
  appendLittleEndian(Result, *Encoding, 4);
  return Result;
}

} // namespace rcc::riscv
