// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/MC/MCInst.h"
#include "rcc/Target/RISCV/RISCVMCCodeEmitter.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] rcc::Expected<rcc::mc::MCInst> fixtureError(std::string_view Message) {
  return rcc::makeUnexpected(rcc::ErrorCode::InternalError, std::string(Message));
}

[[nodiscard]] std::int64_t signExtend(std::uint32_t Value, unsigned Width) {
  const std::uint32_t Mask = (std::uint32_t{1} << Width) - 1U;
  const std::uint32_t Sign = std::uint32_t{1} << (Width - 1U);
  Value &= Mask;
  if ((Value & Sign) == 0)
    return Value;
  return static_cast<std::int64_t>(Value) - (std::int64_t{1} << Width);
}

void addRegister(rcc::mc::MCInst &Instruction, const rcc::InstructionDescriptor &Descriptor,
                 std::uint32_t Encoding) {
  const std::size_t OperandIndex = Instruction.operands().size();
  const rcc::RegisterClassID Class = Descriptor.Operands[OperandIndex].RegClass.value_or(
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR)));
  const auto IsClass = [Class](rcc::riscv::RegisterClass Candidate) {
    return Class.value() == static_cast<std::uint32_t>(Candidate);
  };
  std::uint32_t FirstRegister = 1;
  if (IsClass(rcc::riscv::RegisterClass::FPR32) || IsClass(rcc::riscv::RegisterClass::FPR32C)) {
    FirstRegister = static_cast<std::uint32_t>(rcc::riscv::PhysicalRegisterID::F0_F);
  } else if (IsClass(rcc::riscv::RegisterClass::FPR64) ||
             IsClass(rcc::riscv::RegisterClass::FPR64C)) {
    FirstRegister = static_cast<std::uint32_t>(rcc::riscv::PhysicalRegisterID::F0_D);
  }
  const std::uint32_t ID = Encoding + FirstRegister;
  Instruction.addOperand(rcc::mc::MCOperand::registerOperand(rcc::mc::MCRegister::fromValue(ID)));
}

void addImmediate(rcc::mc::MCInst &Instruction, std::int64_t Value) {
  Instruction.addOperand(rcc::mc::MCOperand::immediate(Value));
}

[[nodiscard]] std::int64_t decodeBranchImmediate(std::uint32_t Encoding) {
  const std::uint32_t Value = ((Encoding >> 31U) & 1U) << 12U | ((Encoding >> 25U) & 0x3fU) << 5U |
                              ((Encoding >> 8U) & 0xfU) << 1U | ((Encoding >> 7U) & 1U) << 11U;
  return signExtend(Value, 13);
}

[[nodiscard]] std::int64_t decodeJALImmediate(std::uint32_t Encoding) {
  const std::uint32_t Value = ((Encoding >> 31U) & 1U) << 20U | ((Encoding >> 21U) & 0x3ffU) << 1U |
                              ((Encoding >> 20U) & 1U) << 11U | ((Encoding >> 12U) & 0xffU) << 12U;
  return signExtend(Value, 21);
}

[[nodiscard]] rcc::Expected<rcc::mc::MCInst>
createStandardFixture(const rcc::InstructionDescriptor &Descriptor,
                      const rcc::riscv::AssemblyInstructionInfo &Assembly) {
  using enum rcc::riscv::AssemblyFormat;
  using enum rcc::riscv::InstructionOpcode;

  const auto Category =
      static_cast<rcc::riscv::EncodingCategory>(Descriptor.EncodingCategory.value());
  const auto Format = static_cast<rcc::riscv::AssemblyFormat>(Assembly.Format.value());
  const auto Opcode =
      static_cast<rcc::riscv::InstructionOpcode>(Assembly.InstructionOpcode.value());
  const std::uint32_t Encoding = Assembly.Encoding;
  const std::uint32_t DestinationEncoding = (Encoding >> 7U) & 0x1fU;
  const std::uint32_t Source1Encoding = (Encoding >> 15U) & 0x1fU;
  const std::uint32_t Source2Encoding = (Encoding >> 20U) & 0x1fU;
  rcc::mc::MCInst Result(rcc::mc::MCOpcode::fromValue(Assembly.InstructionOpcode.value()));

  if (Category == rcc::riscv::EncodingCategory::R) {
    switch (Format) {
    case RD_RS1_RS2:
    case FRD_FRS1_FRS2:
    case RD_FRS1_FRS2:
      addRegister(Result, Descriptor, DestinationEncoding);
      addRegister(Result, Descriptor, Source1Encoding);
      addRegister(Result, Descriptor, Source2Encoding);
      return Result;
    case FRD_FRS1_FRS2_RM:
      addRegister(Result, Descriptor, DestinationEncoding);
      addRegister(Result, Descriptor, Source1Encoding);
      addRegister(Result, Descriptor, Source2Encoding);
      addImmediate(Result, (Encoding >> 12U) & 7U);
      return Result;
    case FRD_FRS1_RM:
    case RD_FRS1_RM:
    case FRD_RS1_RM:
      addRegister(Result, Descriptor, DestinationEncoding);
      addRegister(Result, Descriptor, Source1Encoding);
      addImmediate(Result, (Encoding >> 12U) & 7U);
      return Result;
    case RD_RS1:
    case RD_FRS1:
    case FRD_RS1:
      addRegister(Result, Descriptor, DestinationEncoding);
      addRegister(Result, Descriptor, Source1Encoding);
      return Result;
    default:
      return fixtureError("unsupported R-type reference fixture format");
    }
  }

  if (Category == rcc::riscv::EncodingCategory::I) {
    if (Format == NONE)
      return Result;
    if (Format == IMM_IMM) {
      addImmediate(Result, (Encoding >> 24U) & 0xfU);
      addImmediate(Result, (Encoding >> 20U) & 0xfU);
      return Result;
    }
    if (Format == RD_RS1) {
      addRegister(Result, Descriptor, DestinationEncoding);
      addRegister(Result, Descriptor, Source1Encoding);
      return Result;
    }
    const bool FloatingDestination = Format == FRD_OFFSET_RS1;
    if (Format != RD_RS1_IMM && Format != RD_OFFSET_RS1 && !FloatingDestination)
      return fixtureError("unsupported I-type reference fixture format");
    addRegister(Result, Descriptor, DestinationEncoding);
    addRegister(Result, Descriptor, Source1Encoding);
    const bool Shift64 = Opcode == SLLI || Opcode == SLLI_UW || Opcode == SRLI || Opcode == SRAI;
    const bool Shift32 = Opcode == SLLIW || Opcode == SRLIW || Opcode == SRAIW;
    if (Shift64)
      addImmediate(Result, (Encoding >> 20U) & 0x3fU);
    else if (Shift32)
      addImmediate(Result, (Encoding >> 20U) & 0x1fU);
    else
      addImmediate(Result, signExtend(Encoding >> 20U, 12));
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::S) {
    if (Format != RS2_OFFSET_RS1 && Format != FRS2_OFFSET_RS1)
      return fixtureError("unsupported S-type reference fixture format");
    addRegister(Result, Descriptor, Source2Encoding);
    addRegister(Result, Descriptor, Source1Encoding);
    const std::uint32_t Immediate = ((Encoding >> 25U) & 0x7fU) << 5U | ((Encoding >> 7U) & 0x1fU);
    addImmediate(Result, signExtend(Immediate, 12));
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::B) {
    if (Format != RS1_RS2_TARGET)
      return fixtureError("unsupported B-type reference fixture format");
    addRegister(Result, Descriptor, Source1Encoding);
    addRegister(Result, Descriptor, Source2Encoding);
    addImmediate(Result, decodeBranchImmediate(Encoding));
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::U) {
    if (Format != RD_IMM)
      return fixtureError("unsupported U-type reference fixture format");
    addRegister(Result, Descriptor, DestinationEncoding);
    addImmediate(Result, signExtend(Encoding >> 12U, 20));
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::J) {
    if (Format != RD_TARGET)
      return fixtureError("unsupported J-type reference fixture format");
    addRegister(Result, Descriptor, DestinationEncoding);
    addImmediate(Result, decodeJALImmediate(Encoding));
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::SYSTEM) {
    if (Format == NONE)
      return Result;
    if (Format != RD_CSR_RS1 && Format != RD_CSR_IMM)
      return fixtureError("unsupported system reference fixture format");
    addRegister(Result, Descriptor, DestinationEncoding);
    addImmediate(Result, Encoding >> 20U);
    if (Format == RD_CSR_RS1)
      addRegister(Result, Descriptor, Source1Encoding);
    else
      addImmediate(Result, Source1Encoding);
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::AMO) {
    if (Format != RD_PAREN_RS1 && Format != RD_RS2_PAREN_RS1)
      return fixtureError("unsupported atomic reference fixture format");
    addRegister(Result, Descriptor, DestinationEncoding);
    if (Format == RD_RS2_PAREN_RS1)
      addRegister(Result, Descriptor, Source2Encoding);
    addRegister(Result, Descriptor, Source1Encoding);
    return Result;
  }

  if (Category == rcc::riscv::EncodingCategory::R4) {
    if (Format != FRD_FRS1_FRS2_FRS3_RM)
      return fixtureError("unsupported R4 reference fixture format");
    addRegister(Result, Descriptor, DestinationEncoding);
    addRegister(Result, Descriptor, Source1Encoding);
    addRegister(Result, Descriptor, Source2Encoding);
    addRegister(Result, Descriptor, (Encoding >> 27U) & 0x1fU);
    addImmediate(Result, (Encoding >> 12U) & 7U);
    return Result;
  }

  return fixtureError("unsupported standard reference fixture category");
}

[[nodiscard]] std::int64_t decodeRVCJumpImmediate(std::uint16_t Encoding) {
  const std::uint32_t Value = ((Encoding >> 12U) & 1U) << 11U | ((Encoding >> 11U) & 1U) << 4U |
                              ((Encoding >> 9U) & 3U) << 8U | ((Encoding >> 8U) & 1U) << 10U |
                              ((Encoding >> 7U) & 1U) << 6U | ((Encoding >> 6U) & 1U) << 7U |
                              ((Encoding >> 3U) & 7U) << 1U | ((Encoding >> 2U) & 1U) << 5U;
  return signExtend(Value, 12);
}

[[nodiscard]] std::int64_t decodeRVCBranchImmediate(std::uint16_t Encoding) {
  const std::uint32_t Value = ((Encoding >> 12U) & 1U) << 8U | ((Encoding >> 10U) & 3U) << 3U |
                              ((Encoding >> 5U) & 3U) << 6U | ((Encoding >> 3U) & 3U) << 1U |
                              ((Encoding >> 2U) & 1U) << 5U;
  return signExtend(Value, 9);
}

[[nodiscard]] rcc::Expected<rcc::mc::MCInst>
createCompressedFixture(const rcc::InstructionDescriptor &Descriptor,
                        const rcc::riscv::AssemblyInstructionInfo &Assembly) {
  using enum rcc::riscv::InstructionOpcode;
  const auto Opcode =
      static_cast<rcc::riscv::InstructionOpcode>(Assembly.InstructionOpcode.value());
  const auto Encoding = static_cast<std::uint16_t>(Assembly.Encoding);
  rcc::mc::MCInst Result(rcc::mc::MCOpcode::fromValue(Assembly.InstructionOpcode.value()));

  if (Opcode == C_ADDI4SPN) {
    addRegister(Result, Descriptor, ((Encoding >> 2U) & 7U) + 8U);
    const std::uint32_t Immediate = ((Encoding >> 11U) & 3U) << 4U |
                                    ((Encoding >> 7U) & 0xfU) << 6U |
                                    ((Encoding >> 6U) & 1U) << 2U | ((Encoding >> 5U) & 1U) << 3U;
    addImmediate(Result, Immediate);
    return Result;
  }

  const bool DoubleLoad = Opcode == C_FLD || Opcode == C_LD;
  const bool WordLoad = Opcode == C_LW;
  const bool DoubleStore = Opcode == C_FSD || Opcode == C_SD;
  const bool WordStore = Opcode == C_SW;
  if (DoubleLoad || WordLoad || DoubleStore || WordStore) {
    addRegister(Result, Descriptor, ((Encoding >> 2U) & 7U) + 8U);
    addRegister(Result, Descriptor, ((Encoding >> 7U) & 7U) + 8U);
    const bool Double = DoubleLoad || DoubleStore;
    std::uint32_t Immediate = ((Encoding >> 10U) & 7U) << 3U;
    if (Double) {
      Immediate |= ((Encoding >> 5U) & 3U) << 6U;
    } else {
      Immediate |= ((Encoding >> 6U) & 1U) << 2U | ((Encoding >> 5U) & 1U) << 6U;
    }
    addImmediate(Result, Immediate);
    return Result;
  }

  const bool CI = Opcode == C_ADDI || Opcode == C_ADDIW || Opcode == C_LI || Opcode == C_LUI ||
                  Opcode == C_SLLI;
  if (CI) {
    addRegister(Result, Descriptor, (Encoding >> 7U) & 0x1fU);
    const std::uint32_t Immediate = ((Encoding >> 12U) & 1U) << 5U | ((Encoding >> 2U) & 0x1fU);
    addImmediate(Result, Opcode == C_SLLI ? Immediate : signExtend(Immediate, 6));
    return Result;
  }

  if (Opcode == C_ADDI16SP) {
    const std::uint32_t Immediate = ((Encoding >> 12U) & 1U) << 9U | ((Encoding >> 6U) & 1U) << 4U |
                                    ((Encoding >> 5U) & 1U) << 6U | ((Encoding >> 3U) & 3U) << 7U |
                                    ((Encoding >> 2U) & 1U) << 5U;
    addImmediate(Result, signExtend(Immediate, 10));
    return Result;
  }

  const bool CompactShift = Opcode == C_SRLI || Opcode == C_SRAI;
  if (CompactShift || Opcode == C_ANDI) {
    addRegister(Result, Descriptor, ((Encoding >> 7U) & 7U) + 8U);
    const std::uint32_t Immediate = ((Encoding >> 12U) & 1U) << 5U | ((Encoding >> 2U) & 0x1fU);
    addImmediate(Result, CompactShift ? Immediate : signExtend(Immediate, 6));
    return Result;
  }

  const bool CompactALU = Opcode == C_SUB || Opcode == C_XOR || Opcode == C_OR || Opcode == C_AND ||
                          Opcode == C_SUBW || Opcode == C_ADDW;
  if (CompactALU) {
    addRegister(Result, Descriptor, ((Encoding >> 7U) & 7U) + 8U);
    addRegister(Result, Descriptor, ((Encoding >> 2U) & 7U) + 8U);
    return Result;
  }

  if (Opcode == C_J) {
    addImmediate(Result, decodeRVCJumpImmediate(Encoding));
    return Result;
  }

  if (Opcode == C_BEQZ || Opcode == C_BNEZ) {
    addRegister(Result, Descriptor, ((Encoding >> 7U) & 7U) + 8U);
    addImmediate(Result, decodeRVCBranchImmediate(Encoding));
    return Result;
  }

  const bool DoubleStackLoad = Opcode == C_FLDSP || Opcode == C_LDSP;
  if (DoubleStackLoad || Opcode == C_LWSP) {
    addRegister(Result, Descriptor, (Encoding >> 7U) & 0x1fU);
    const std::uint32_t Immediate =
        DoubleStackLoad ? ((Encoding >> 12U) & 1U) << 5U | ((Encoding >> 5U) & 3U) << 3U |
                              ((Encoding >> 2U) & 7U) << 6U
                        : ((Encoding >> 12U) & 1U) << 5U | ((Encoding >> 4U) & 7U) << 2U |
                              ((Encoding >> 2U) & 3U) << 6U;
    addImmediate(Result, Immediate);
    return Result;
  }

  const bool CRSingle = Opcode == C_JR || Opcode == C_JALR;
  const bool CRDouble = Opcode == C_MV || Opcode == C_ADD;
  if (CRSingle || CRDouble) {
    addRegister(Result, Descriptor, (Encoding >> 7U) & 0x1fU);
    if (CRDouble)
      addRegister(Result, Descriptor, (Encoding >> 2U) & 0x1fU);
    return Result;
  }

  if (Opcode == C_EBREAK)
    return Result;

  const bool DoubleStackStore = Opcode == C_FSDSP || Opcode == C_SDSP;
  if (DoubleStackStore || Opcode == C_SWSP) {
    addRegister(Result, Descriptor, (Encoding >> 2U) & 0x1fU);
    const std::uint32_t Immediate =
        DoubleStackStore ? ((Encoding >> 10U) & 7U) << 3U | ((Encoding >> 7U) & 7U) << 6U
                         : ((Encoding >> 9U) & 0xfU) << 2U | ((Encoding >> 7U) & 3U) << 6U;
    addImmediate(Result, Immediate);
    return Result;
  }

  return fixtureError("unsupported compressed reference fixture opcode");
}

[[nodiscard]] rcc::Expected<rcc::mc::MCInst>
createReferenceFixture(const rcc::InstructionDescriptor &Descriptor,
                       const rcc::riscv::AssemblyInstructionInfo &Assembly) {
  const auto Category =
      static_cast<rcc::riscv::EncodingCategory>(Descriptor.EncodingCategory.value());
  if (Category == rcc::riscv::EncodingCategory::C)
    return createCompressedFixture(Descriptor, Assembly);
  return createStandardFixture(Descriptor, Assembly);
}

[[nodiscard]] std::vector<std::uint8_t> referenceBytes(std::uint32_t Encoding, std::uint8_t Size) {
  std::vector<std::uint8_t> Result;
  Result.reserve(Size);
  for (std::uint8_t Index = 0; Index < Size; ++Index)
    Result.push_back(static_cast<std::uint8_t>(Encoding >> (Index * 8U)));
  return Result;
}

} // namespace

int main() {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target) {
    std::cerr << Target.error().message() << '\n';
    return 1;
  }
  const std::span<const rcc::riscv::AssemblyInstructionInfo> Instructions =
      rcc::riscv::assemblyInstructions();
  if (Instructions.size() != 211) {
    std::cerr << "RISC-V encoding coverage does not contain all 211 instructions\n";
    return 1;
  }

  rcc::riscv::RISCVMCCodeEmitter Emitter(*Target);
  for (const rcc::riscv::AssemblyInstructionInfo &Assembly : Instructions) {
    const rcc::InstructionDescriptor *Descriptor =
        Target->instructionDescriptor(Assembly.InstructionOpcode);
    if (Descriptor == nullptr || Assembly.Encoding == 0) {
      std::cerr << Assembly.Mnemonic << ": missing encoding description\n";
      return 1;
    }
    auto Fixture = createReferenceFixture(*Descriptor, Assembly);
    if (!Fixture) {
      std::cerr << Assembly.Mnemonic << ": " << Fixture.error().message() << '\n';
      return 1;
    }
    auto Encoded = Emitter.encodeInstruction(*Fixture);
    if (!Encoded) {
      std::cerr << Assembly.Mnemonic << ": " << Encoded.error().message() << '\n';
      return 1;
    }
    const auto Category =
        static_cast<rcc::riscv::EncodingCategory>(Descriptor->EncodingCategory.value());
    const std::uint8_t Size = Category == rcc::riscv::EncodingCategory::C ? 2 : 4;
    if (!Encoded->Fixups.empty() || Encoded->Bytes != referenceBytes(Assembly.Encoding, Size)) {
      std::uint32_t Actual = 0;
      for (std::size_t Index = 0; Index < Encoded->Bytes.size(); ++Index)
        Actual |= static_cast<std::uint32_t>(Encoded->Bytes[Index]) << (Index * 8U);
      std::cerr << Assembly.Mnemonic << ": encoding differs from its RV64GC reference word ("
                << Actual << " != " << Assembly.Encoding << ")\n";
      return 1;
    }
  }
  return 0;
}
