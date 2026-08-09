// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCCFI.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCInstPrinter.h"
#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCStreamer.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

class TestPrinter final : public rcc::mc::MCInstPrinter {
public:
  [[nodiscard]] rcc::Expected<std::string>
  printInstruction(const rcc::mc::MCInst &Instruction) const override {
    if (Instruction.opcode().value() != 7 || Instruction.operands().size() != 2)
      return rcc::makeUnexpected(rcc::ErrorCode::AssemblyLoweringFailed,
                                 "unexpected test instruction");
    const auto *Register = Instruction.operands()[0].registerValue();
    const auto *Immediate = Instruction.operands()[1].immediateValue();
    if (Register == nullptr || Immediate == nullptr)
      return rcc::makeUnexpected(rcc::ErrorCode::AssemblyLoweringFailed,
                                 "unexpected test operands");
    return "probe r" + std::to_string(Register->value()) + ", " + std::to_string(*Immediate);
  }

  [[nodiscard]] rcc::Expected<std::string>
  printTargetExpression(std::uint32_t Kind, std::string_view Operand) const override {
    if (Kind != 3)
      return rcc::makeUnexpected(rcc::ErrorCode::AssemblyLoweringFailed, "unknown test expression");
    return "%test(" + std::string(Operand) + ")";
  }
};

int fail(std::string_view Message) {
  std::cerr << Message << '\n';
  return 1;
}

[[nodiscard]] bool contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

} // namespace

int main() {
  rcc::mc::MCContext Context;
  auto Symbol = Context.getOrCreateSymbol("object", rcc::mc::MCSymbolKind::Object);
  auto SameSymbol = Context.getOrCreateSymbol("object", rcc::mc::MCSymbolKind::Generic);
  if (!Symbol || !SameSymbol || *Symbol != *SameSymbol || Context.symbol("object") != *Symbol)
    return fail("MCContext must uniquify compatible symbols");
  if (Context.getOrCreateSymbol("object", rcc::mc::MCSymbolKind::Function) ||
      Context.getOrCreateSymbol("bad\nname", rcc::mc::MCSymbolKind::Generic)) {
    return fail("MCContext must reject conflicting or invalid symbols");
  }
  auto Temporary0 = Context.createTemporarySymbol(".Ltmp");
  auto Temporary1 = Context.createTemporarySymbol(".Ltmp");
  if (!Temporary0 || !Temporary1 || (*Temporary0)->name() != ".Ltmp0" ||
      (*Temporary1)->name() != ".Ltmp1") {
    return fail("temporary MC symbols must be deterministic");
  }

  auto SymbolExpr = rcc::mc::MCExpr::symbol(*Symbol);
  if (!SymbolExpr)
    return fail("symbol MC expression creation failed");
  auto Add = rcc::mc::MCExpr::binary(rcc::mc::MCBinaryOperator::Add, *SymbolExpr,
                                     rcc::mc::MCExpr::constant(4));
  if (!Add)
    return fail("binary MC expression creation failed");
  auto TargetExpr = rcc::mc::MCExpr::target(3, *Add);
  if (!TargetExpr || !(*TargetExpr)->verify())
    return fail("structured MC expression creation failed");
  auto Folded =
      rcc::mc::MCExpr::binary(rcc::mc::MCBinaryOperator::ShiftLeft, rcc::mc::MCExpr::constant(-1),
                              rcc::mc::MCExpr::constant(63));
  if (!Folded || (*Folded)->evaluateAbsolute() != std::int64_t(-9223372036854775807LL - 1))
    return fail("absolute MC expression evaluation must use defined 64-bit semantics");

  rcc::mc::MCSectionDesc TextDescription{
      .Name = ".text",
      .Kind = rcc::mc::MCSectionKind::Text,
      .Flags = {rcc::mc::MCSectionFlag::Alloc, rcc::mc::MCSectionFlag::Executable},
      .Alignment = 4,
      .EntrySize = 0,
      .Group = {},
      .UniqueID = std::nullopt,
  };
  auto Text = Context.getOrCreateSection(TextDescription);
  auto SameText = Context.getOrCreateSection(TextDescription);
  if (!Text || !SameText || *Text != *SameText || Context.section(".text") != *Text)
    return fail("MCContext must uniquify identical sections");
  TextDescription.Alignment = 8;
  auto DifferentlyAlignedText = Context.getOrCreateSection(TextDescription);
  if (!DifferentlyAlignedText || *DifferentlyAlignedText != *Text)
    return fail("MCContext must merge per-fragment section alignment requirements");
  TextDescription.Flags.set(rcc::mc::MCSectionFlag::Writable);
  if (Context.getOrCreateSection(std::move(TextDescription)))
    return fail("MCContext must reject conflicting section descriptions");

  TestPrinter Printer;
  rcc::mc::AsmTextStreamer Streamer(Printer);
  rcc::mc::MCInst Instruction(rcc::mc::MCOpcode::fromValue(7));
  Instruction.setFlags(11);
  Instruction.setSourceIdentity(42);
  Instruction.addOperand(rcc::mc::MCOperand::registerOperand(rcc::mc::MCRegister::fromValue(5)));
  Instruction.addOperand(rcc::mc::MCOperand::immediate(-9));
  if (Streamer.emitInstruction(Instruction) || !Streamer.switchSection(*Text) ||
      !Streamer.emitSymbolAttribute(*Symbol, rcc::mc::MCSymbolAttribute::Global) ||
      !Streamer.emitLabel(*Symbol) || Streamer.emitLabel(*Symbol) ||
      !Streamer.emitInstruction(Instruction) || !Streamer.emitValue(*TargetExpr, 8)) {
    return fail("assembly streamer event validation failed");
  }
  const std::array<std::uint8_t, 3> Bytes{0x01, 0x02, 0xff};
  if (!Streamer.emitBytes(Bytes) || !Streamer.emitZeroFill(6) ||
      !Streamer.emitAlignment(16, 0xaa, 31) ||
      !Streamer.emitSymbolSize(*Symbol, rcc::mc::MCExpr::constant(17))) {
    return fail("assembly streamer data events failed");
  }
  rcc::mc::MCCFIInstruction CFI;
  CFI.Operation = rcc::mc::MCCFIOperation::DefCFA;
  CFI.Operands = {2, 16};
  if (!Streamer.emitCFI(CFI) || !Streamer.emitInlineAssembly("  nop\n") ||
      !Streamer.emitComment("finished") || !Streamer.finish() || Streamer.finish()) {
    return fail("assembly streamer terminal events failed");
  }
  auto Output = Streamer.takeOutput();
  if (!Output || Streamer.takeOutput())
    return fail("assembly output ownership must transfer exactly once");
  if (!contains(*Output, ".section .text,\"ax\",@progbits\n") ||
      !contains(*Output, ".globl object\nobject:\n") || !contains(*Output, "  probe r5, -9\n") ||
      !contains(*Output, "  .8byte %test((object+4))\n") ||
      !contains(*Output, "  .byte 0x01, 0x02, 0xff\n") || !contains(*Output, "  .zero 6\n") ||
      !contains(*Output, "  .p2align 4, 170, 31\n") || !contains(*Output, ".size object, 17\n") ||
      !contains(*Output, ".cfi_def_cfa 2, 16\n") || !contains(*Output, "  nop\n  # finished\n")) {
    return fail("assembly streamer output is incomplete or unstable");
  }
  return 0;
}
