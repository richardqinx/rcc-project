// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCInst.h"
#include "rcc/Target/RISCV/RISCVAsmBackend.h"
#include "rcc/Target/RISCV/RISCVFixupKinds.h"
#include "rcc/Target/RISCV/RISCVInstPrinter.h"
#include "rcc/Target/RISCV/RISCVMCCodeEmitter.h"
#include "rcc/Target/RISCV/RISCVObjectFile.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view Message) {
  std::cerr << Message << '\n';
  return 1;
}

[[nodiscard]] rcc::mc::MCOperand reg(rcc::riscv::PhysicalRegisterID ID) {
  return rcc::mc::MCOperand::registerOperand(
      rcc::mc::MCRegister::fromValue(static_cast<std::uint32_t>(ID)));
}

[[nodiscard]] rcc::mc::MCOperand imm(std::int64_t Value) {
  return rcc::mc::MCOperand::immediate(Value);
}

[[nodiscard]] rcc::mc::MCInst instruction(rcc::riscv::InstructionOpcode Opcode,
                                          std::initializer_list<rcc::mc::MCOperand> Operands) {
  rcc::mc::MCInst Result(rcc::mc::MCOpcode::fromValue(static_cast<std::uint32_t>(Opcode)));
  Result.reserveOperands(Operands.size());
  for (const rcc::mc::MCOperand &Operand : Operands)
    Result.addOperand(Operand);
  return Result;
}

[[nodiscard]] bool bytesEqual(std::span<const std::uint8_t> Left,
                              std::initializer_list<std::uint8_t> Right) {
  return Left.size() == Right.size() &&
         std::equal(Left.begin(), Left.end(), Right.begin(), Right.end());
}

[[nodiscard]] bool expectEncoding(const rcc::riscv::RISCVMCCodeEmitter &Emitter,
                                  const rcc::mc::MCInst &Instruction,
                                  std::initializer_list<std::uint8_t> Expected,
                                  std::string_view Name) {
  auto Encoded = Emitter.encodeInstruction(Instruction);
  if (!Encoded) {
    std::cerr << Name << ": " << Encoded.error().message() << '\n';
    return false;
  }
  if (!Encoded->Fixups.empty() || !bytesEqual(Encoded->Bytes, Expected)) {
    std::cerr << Name << ": encoded bytes differ\n";
    return false;
  }
  return true;
}

[[nodiscard]] rcc::Expected<rcc::mc::MCExprRef>
symbolExpression(rcc::mc::MCContext &Context, std::string_view Name, std::uint32_t TargetKind = 0) {
  auto Symbol = Context.getOrCreateSymbol(std::string(Name), rcc::mc::MCSymbolKind::Generic);
  if (!Symbol)
    return std::unexpected(Symbol.error());
  auto Expression = rcc::mc::MCExpr::symbol(*Symbol);
  if (!Expression)
    return std::unexpected(Expression.error());
  if (TargetKind == 0)
    return *Expression;
  return rcc::mc::MCExpr::target(TargetKind, *Expression);
}

[[nodiscard]] bool testEncodings(const rcc::riscv::RISCVMCCodeEmitter &Emitter) {
  using enum rcc::riscv::InstructionOpcode;
  using enum rcc::riscv::PhysicalRegisterID;
  return expectEncoding(Emitter, instruction(AUIPC, {reg(X11), imm(7)}), {0x97, 0x75, 0x00, 0x00},
                        "AUIPC") &&
         expectEncoding(Emitter, instruction(ADDI, {reg(X12), reg(X13), imm(-17)}),
                        {0x13, 0x86, 0xf6, 0xfe}, "ADDI") &&
         expectEncoding(Emitter, instruction(SRAI, {reg(X14), reg(X15), imm(63)}),
                        {0x13, 0xd7, 0xf7, 0x43}, "SRAI") &&
         expectEncoding(Emitter, instruction(SH1ADD, {reg(X8), reg(X9), reg(X10)}),
                        {0x33, 0xa4, 0xa4, 0x20}, "SH1ADD") &&
         expectEncoding(Emitter, instruction(SEXT_B, {reg(X8), reg(X9)}), {0x13, 0x94, 0x44, 0x60},
                        "SEXT.B") &&
         expectEncoding(Emitter, instruction(ZEXT_H_RV64, {reg(X8), reg(X9)}),
                        {0x3b, 0xc4, 0x04, 0x08}, "ZEXT.H") &&
         expectEncoding(Emitter, instruction(SD, {reg(X16), reg(X17), imm(-32)}),
                        {0x23, 0xb0, 0x08, 0xff}, "SD") &&
         expectEncoding(Emitter, instruction(BGE, {reg(X18), reg(X19), imm(18)}),
                        {0x63, 0x59, 0x39, 0x01}, "BGE") &&
         expectEncoding(Emitter, instruction(JAL, {reg(X20), imm(-20)}), {0x6f, 0xfa, 0xdf, 0xfe},
                        "JAL") &&
         expectEncoding(Emitter, instruction(FENCE, {imm(10), imm(5)}), {0x0f, 0x00, 0x50, 0x0a},
                        "FENCE") &&
         expectEncoding(Emitter, instruction(CSRRWI, {reg(X21), imm(0x305), imm(17)}),
                        {0xf3, 0xda, 0x58, 0x30}, "CSRRWI") &&
         expectEncoding(Emitter, instruction(AMOADD_D, {reg(X22), reg(X23), reg(X24)}),
                        {0x2f, 0x3b, 0x7c, 0x01}, "AMOADD.D") &&
         expectEncoding(Emitter,
                        instruction(FMADD_D, {reg(F1_D), reg(F2_D), reg(F3_D), reg(F4_D), imm(2)}),
                        {0xc3, 0x20, 0x31, 0x22}, "FMADD.D") &&
         expectEncoding(Emitter, instruction(FADD_S, {reg(F5_F), reg(F6_F), reg(F7_F), imm(3)}),
                        {0xd3, 0x32, 0x73, 0x00}, "FADD.S") &&
         expectEncoding(Emitter, instruction(C_ADDI4SPN, {reg(X8), imm(48)}), {0x00, 0x18},
                        "C.ADDI4SPN") &&
         expectEncoding(Emitter, instruction(C_LD, {reg(X9), reg(X8), imm(56)}), {0x04, 0x7c},
                        "C.LD") &&
         expectEncoding(Emitter, instruction(C_SW, {reg(X10), reg(X11), imm(28)}), {0xc8, 0xcd},
                        "C.SW") &&
         expectEncoding(Emitter, instruction(C_ADDI, {reg(X12), imm(-9)}), {0x5d, 0x16},
                        "C.ADDI") &&
         expectEncoding(Emitter, instruction(C_ADDI16SP, {imm(-128)}), {0x19, 0x71},
                        "C.ADDI16SP") &&
         expectEncoding(Emitter, instruction(C_SRAI, {reg(X13), imm(33)}), {0x85, 0x96},
                        "C.SRAI") &&
         expectEncoding(Emitter, instruction(C_SUBW, {reg(X14), reg(X15)}), {0x1d, 0x9f},
                        "C.SUBW") &&
         expectEncoding(Emitter, instruction(C_J, {imm(42)}), {0x2d, 0xa0}, "C.J") &&
         expectEncoding(Emitter, instruction(C_BNEZ, {reg(X8), imm(-30)}), {0x6d, 0xf0},
                        "C.BNEZ") &&
         expectEncoding(Emitter, instruction(C_LDSP, {reg(X10), imm(248)}), {0x6e, 0x75},
                        "C.LDSP") &&
         expectEncoding(Emitter, instruction(C_SWSP, {reg(X11), imm(124)}), {0xae, 0xde},
                        "C.SWSP") &&
         expectEncoding(Emitter, instruction(C_JALR, {reg(X12)}), {0x02, 0x96}, "C.JALR");
}

[[nodiscard]] bool testFixups(const rcc::riscv::RISCVMCCodeEmitter &Emitter,
                              const rcc::riscv::RISCVAsmBackend &Backend) {
  using enum rcc::riscv::InstructionOpcode;
  using enum rcc::riscv::PhysicalRegisterID;
  rcc::mc::MCContext Context;

  auto HighExpression =
      symbolExpression(Context, "destination",
                       static_cast<std::uint32_t>(rcc::riscv::RISCVMCExprKind::PCRelativeHigh));
  if (!HighExpression)
    return false;
  auto High = instruction(AUIPC, {reg(X5), rcc::mc::MCOperand::expression(*HighExpression)});
  auto HighEncoding = Emitter.encodeInstruction(High);
  if (!HighEncoding || HighEncoding->Fixups.size() != 1 ||
      HighEncoding->Fixups[0].Kind !=
          rcc::riscv::fixupKind(rcc::riscv::RISCVFixupKind::PCRelativeHigh20) ||
      !HighEncoding->Fixups[0].PCRelative ||
      !bytesEqual(HighEncoding->Bytes, {0x97, 0x02, 0x00, 0x00})) {
    return false;
  }
  auto HighValue = rcc::mc::evaluateAsRelocatable(**HighExpression);
  if (!HighValue)
    return false;
  auto HighRelocation = Backend.relocationType(HighEncoding->Fixups[0], *HighValue);
  if (!HighRelocation || *HighRelocation != 23)
    return false;

  auto LowExpression = symbolExpression(
      Context, "anchor", static_cast<std::uint32_t>(rcc::riscv::RISCVMCExprKind::PCRelativeLow));
  if (!LowExpression)
    return false;
  auto Low = instruction(JALR, {reg(X1), reg(X5), rcc::mc::MCOperand::expression(*LowExpression)});
  auto LowEncoding = Emitter.encodeInstruction(Low);
  if (!LowEncoding || LowEncoding->Fixups.size() != 1 ||
      LowEncoding->Fixups[0].Kind !=
          rcc::riscv::fixupKind(rcc::riscv::RISCVFixupKind::PCRelativeLow12I)) {
    return false;
  }

  auto AbsoluteHighExpression = symbolExpression(
      Context, "absolute", static_cast<std::uint32_t>(rcc::riscv::RISCVMCExprKind::AbsoluteHigh));
  if (!AbsoluteHighExpression)
    return false;
  auto AbsoluteHigh =
      instruction(LUI, {reg(X6), rcc::mc::MCOperand::expression(*AbsoluteHighExpression)});
  auto AbsoluteHighEncoding = Emitter.encodeInstruction(AbsoluteHigh);
  if (!AbsoluteHighEncoding || AbsoluteHighEncoding->Fixups.size() != 1 ||
      AbsoluteHighEncoding->Fixups[0].Kind !=
          rcc::riscv::fixupKind(rcc::riscv::RISCVFixupKind::High20)) {
    return false;
  }
  auto AbsoluteHighValue = rcc::mc::evaluateAsRelocatable(**AbsoluteHighExpression);
  auto AbsoluteHighRelocation =
      AbsoluteHighValue
          ? Backend.relocationType(AbsoluteHighEncoding->Fixups[0], *AbsoluteHighValue)
          : rcc::Expected<std::uint32_t>{std::unexpected(AbsoluteHighValue.error())};
  if (!AbsoluteHighRelocation || *AbsoluteHighRelocation != 26)
    return false;

  auto AbsoluteLowExpression = symbolExpression(
      Context, "absolute", static_cast<std::uint32_t>(rcc::riscv::RISCVMCExprKind::AbsoluteLow));
  if (!AbsoluteLowExpression)
    return false;
  auto AbsoluteLow =
      instruction(ADDI, {reg(X6), reg(X6), rcc::mc::MCOperand::expression(*AbsoluteLowExpression)});
  auto AbsoluteLowEncoding = Emitter.encodeInstruction(AbsoluteLow);
  if (!AbsoluteLowEncoding || AbsoluteLowEncoding->Fixups.size() != 1 ||
      AbsoluteLowEncoding->Fixups[0].Kind !=
          rcc::riscv::fixupKind(rcc::riscv::RISCVFixupKind::Low12I)) {
    return false;
  }
  auto AbsoluteLowValue = rcc::mc::evaluateAsRelocatable(**AbsoluteLowExpression);
  auto AbsoluteLowRelocation =
      AbsoluteLowValue ? Backend.relocationType(AbsoluteLowEncoding->Fixups[0], *AbsoluteLowValue)
                       : rcc::Expected<std::uint32_t>{std::unexpected(AbsoluteLowValue.error())};
  if (!AbsoluteLowRelocation || *AbsoluteLowRelocation != 27)
    return false;

  auto BranchExpression = symbolExpression(Context, "block");
  if (!BranchExpression)
    return false;
  auto Branch =
      instruction(BEQ, {reg(X10), reg(X11), rcc::mc::MCOperand::expression(*BranchExpression)});
  auto BranchEncoding = Emitter.encodeInstruction(Branch);
  if (!BranchEncoding || BranchEncoding->Fixups.size() != 1 ||
      BranchEncoding->Fixups[0].Kind != rcc::riscv::fixupKind(rcc::riscv::RISCVFixupKind::Branch)) {
    return false;
  }
  auto Applied = Backend.applyFixup(BranchEncoding->Fixups[0].Kind, BranchEncoding->Bytes, 0, 18);
  if (!Applied || !bytesEqual(BranchEncoding->Bytes, {0x63, 0x09, 0xb5, 0x00}))
    return false;

  std::vector<std::uint8_t> Word(4);
  auto OutOfRange =
      Backend.applyFixup(rcc::riscv::fixupKind(rcc::riscv::RISCVFixupKind::Branch), Word, 0, 4096);
  return !OutOfRange && OutOfRange.error().code() == rcc::ErrorCode::FixupApplicationFailed;
}

[[nodiscard]] bool testFailures(const rcc::riscv::RISCVMCCodeEmitter &Emitter) {
  using enum rcc::riscv::InstructionOpcode;
  using enum rcc::riscv::PhysicalRegisterID;
  auto BadShift = Emitter.encodeInstruction(instruction(SLLI, {reg(X1), reg(X2), imm(64)}));
  auto BadCompactRegister =
      Emitter.encodeInstruction(instruction(C_LD, {reg(X1), reg(X8), imm(8)}));
  auto BadAlignment = Emitter.encodeInstruction(instruction(C_SDSP, {reg(X10), imm(6)}));
  auto Pseudo = Emitter.encodeInstruction(instruction(PseudoCALL, {}));
  return !BadShift && BadShift.error().code() == rcc::ErrorCode::InstructionEncodingFailed &&
         !BadCompactRegister &&
         BadCompactRegister.error().code() == rcc::ErrorCode::InstructionEncodingFailed &&
         !BadAlignment &&
         BadAlignment.error().code() == rcc::ErrorCode::InstructionEncodingFailed && !Pseudo &&
         Pseudo.error().code() == rcc::ErrorCode::InstructionEncodingFailed;
}

[[nodiscard]] bool containsBytes(std::span<const std::uint8_t> Bytes, std::string_view Text) {
  if (Text.size() > Bytes.size())
    return false;
  const auto *Begin = reinterpret_cast<const char *>(Bytes.data());
  return std::string_view(Begin, Bytes.size()).find(Text) != std::string_view::npos;
}

[[nodiscard]] bool testObjectEmission(const rcc::TargetDescription &Target) {
  auto Alignment = rcc::Alignment::create(8);
  auto Value = rcc::APInt::fromUnsigned(64, 0x1122334455667788ULL);
  if (!Alignment || !Value)
    return false;
  rcc::GlobalObjectDesc Global;
  Global.Symbol = "object_value";
  Global.ObjectType = rcc::GenericConstantType::byte();
  Global.Align = *Alignment;
  Global.Size = 8;
  Global.Initializer.emplace(rcc::GlobalInitializer::integer(std::move(*Value)));
  rcc::CompilationUnit Unit;
  if (!Unit.addGlobalObject(std::move(Global)))
    return false;
  auto Object = rcc::riscv::emitObject(Unit, Target);
  if (!Object || Object->size() < 64 || (*Object)[0] != 0x7f || (*Object)[1] != 'E' ||
      (*Object)[2] != 'L' || (*Object)[3] != 'F') {
    return false;
  }
  const std::uint16_t Machine =
      static_cast<std::uint16_t>((*Object)[18]) | static_cast<std::uint16_t>((*Object)[19] << 8U);
  const std::uint32_t Flags = static_cast<std::uint32_t>((*Object)[48]) |
                              static_cast<std::uint32_t>((*Object)[49]) << 8U |
                              static_cast<std::uint32_t>((*Object)[50]) << 16U |
                              static_cast<std::uint32_t>((*Object)[51]) << 24U;
  return Machine == 243 && Flags == 5 && containsBytes(*Object, ".data") &&
         containsBytes(*Object, ".riscv.attributes") && containsBytes(*Object, "object_value") &&
         containsBytes(*Object, std::string_view("\x88\x77\x66\x55\x44\x33\x22\x11", 8));
}

} // namespace

int main() {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return fail(Target.error().message());
  rcc::riscv::RISCVMCCodeEmitter Emitter(*Target);
  rcc::riscv::RISCVAsmBackend Backend;
  rcc::riscv::RISCVInstPrinter Printer(*Target);
  auto PrintedHigh = Printer.printTargetExpression(
      static_cast<std::uint32_t>(rcc::riscv::RISCVMCExprKind::AbsoluteHigh), "symbol");
  auto PrintedLow = Printer.printTargetExpression(
      static_cast<std::uint32_t>(rcc::riscv::RISCVMCExprKind::AbsoluteLow), "symbol");
  if (!PrintedHigh || *PrintedHigh != "%hi(symbol)" || !PrintedLow || *PrintedLow != "%lo(symbol)")
    return fail("RISC-V absolute address modifiers do not print canonically");
  auto PrintedUnary = Printer.printInstruction(instruction(
      rcc::riscv::InstructionOpcode::SEXT_B,
      {reg(rcc::riscv::PhysicalRegisterID::X8), reg(rcc::riscv::PhysicalRegisterID::X9)}));
  if (!PrintedUnary || *PrintedUnary != "sext.b x8, x9")
    return fail("RISC-V unary integer instructions do not print canonically");
  if (!testEncodings(Emitter))
    return fail("RISC-V MC encodings differ from the RV64GC reference encodings");
  if (!testFixups(Emitter, Backend))
    return fail("RISC-V MC fixup or relocation behavior is incorrect");
  if (!testFailures(Emitter))
    return fail("RISC-V MC encoding failures are not diagnosed");
  if (!testObjectEmission(*Target))
    return fail("RISC-V ELF object emission is incomplete");
  if (Backend.objectFlags() != 5)
    return fail("RISC-V ELF flags do not describe RV64GC LP64D");
  return 0;
}
