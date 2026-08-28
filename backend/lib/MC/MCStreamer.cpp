// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCStreamer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::mc {
namespace {

[[nodiscard]] bool containsInvalidLineCharacter(std::string_view Text) noexcept {
  return Text.find('\0') != std::string_view::npos || Text.find('\n') != std::string_view::npos ||
         Text.find('\r') != std::string_view::npos;
}

[[nodiscard]] unsigned log2Exact(std::uint64_t Value) noexcept {
  unsigned Result = 0;
  while (Value > 1) {
    Value >>= 1U;
    ++Result;
  }
  return Result;
}

[[nodiscard]] std::string byteSpelling(std::uint8_t Byte) {
  constexpr std::string_view Hex = "0123456789abcdef";
  std::string Result = "0x00";
  Result[2] = Hex[(Byte >> 4U) & 0xfU];
  Result[3] = Hex[Byte & 0xfU];
  return Result;
}

[[nodiscard]] std::string sectionFlagSpelling(MCSectionFlags Flags) {
  std::string Result;
  const auto Add = [&](MCSectionFlag Flag, char Spelling) {
    if (Flags.has(Flag))
      Result.push_back(Spelling);
  };
  Add(MCSectionFlag::Alloc, 'a');
  Add(MCSectionFlag::Writable, 'w');
  Add(MCSectionFlag::Executable, 'x');
  Add(MCSectionFlag::Merge, 'M');
  Add(MCSectionFlag::Strings, 'S');
  Add(MCSectionFlag::TLS, 'T');
  Add(MCSectionFlag::Group, 'G');
  Add(MCSectionFlag::Retain, 'R');
  return Result;
}

[[nodiscard]] std::string_view sectionTypeSpelling(MCSectionKind Kind) noexcept {
  switch (Kind) {
  case MCSectionKind::BSS:
  case MCSectionKind::TLSBSS:
    return "nobits";
  case MCSectionKind::Text:
  case MCSectionKind::ReadOnly:
  case MCSectionKind::Data:
  case MCSectionKind::TLSData:
  case MCSectionKind::EHFrame:
  case MCSectionKind::Custom:
    return "progbits";
  }
  return {};
}

[[nodiscard]] bool isDirectiveName(std::string_view Name) noexcept {
  if (Name.empty() || Name.front() == '.')
    return false;
  const auto Initial = [](unsigned char Character) {
    return std::isalpha(Character) != 0 || Character == '_';
  };
  const auto Following = [](unsigned char Character) {
    return std::isalnum(Character) != 0 || Character == '_' || Character == '.';
  };
  if (!Initial(static_cast<unsigned char>(Name.front())))
    return false;
  return std::ranges::all_of(Name.substr(1), [Following](char Character) {
    return Following(static_cast<unsigned char>(Character));
  });
}

[[nodiscard]] std::string joinIntegerOperands(std::span<const std::int64_t> Operands) {
  std::string Result;
  for (const std::int64_t Operand : Operands) {
    if (!Result.empty())
      Result += ", ";
    Result += std::to_string(Operand);
  }
  return Result;
}

[[nodiscard]] std::vector<std::uint8_t> encodeULEB128(std::uint64_t Value) {
  std::vector<std::uint8_t> Bytes;
  do {
    std::uint8_t Byte = static_cast<std::uint8_t>(Value & 0x7fU);
    Value >>= 7U;
    if (Value != 0)
      Byte |= 0x80U;
    Bytes.push_back(Byte);
  } while (Value != 0);
  return Bytes;
}

} // namespace

Expected<void> AsmTextStreamer::appendLine(std::string_view Prefix, std::string_view Text) {
  if (IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit after the assembly stream is finalized");
  if (Text.empty() || containsInvalidLineCharacter(Text)) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly stream line must be nonempty and contain no line break");
  }
  Output.append(Prefix);
  Output.append(Text);
  Output.push_back('\n');
  return {};
}

Expected<void> AsmTextStreamer::requireActiveSection() const {
  if (!CurrentSection) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly emission requires an active section");
  }
  if (IsFinished) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit after the assembly stream is finalized");
  }
  return {};
}

Expected<void> AsmTextStreamer::switchSection(MCSectionRef Section) {
  if (IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot switch section after assembly finalization");
  if (!Section)
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot switch to a null MC section");
  auto Verification = Section->description().verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  if (CurrentSection == Section)
    return {};

  auto Name = printMCSymbolName(Section->name());
  if (!Name)
    return std::unexpected(Name.error());
  const MCSectionDesc &Description = Section->description();
  std::string Directive = ".section " + *Name + ",\"" + sectionFlagSpelling(Description.Flags) +
                          "\",@" + std::string(sectionTypeSpelling(Description.Kind));
  if (Description.EntrySize != 0)
    Directive += "," + std::to_string(Description.EntrySize);
  if (Description.Group) {
    auto GroupName = printMCSymbolName(Description.Group->name());
    if (!GroupName)
      return std::unexpected(GroupName.error());
    Directive += "," + *GroupName + ",comdat";
  }
  if (Description.UniqueID)
    Directive += ",unique," + std::to_string(*Description.UniqueID);

  auto Emitted = appendLine({}, Directive);
  if (!Emitted)
    return std::unexpected(Emitted.error());
  CurrentSection = std::move(Section);
  return {};
}

Expected<void> AsmTextStreamer::emitLabel(MCSymbolRef Symbol) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (!Symbol)
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot emit a null MC symbol label");
  const std::string Name(Symbol->name());
  if (!DefinedSymbols.insert(Name).second) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "MC symbol label is defined more than once");
  }
  auto Printed = printMCSymbolName(Name);
  if (!Printed)
    return std::unexpected(Printed.error());
  return appendLine({}, *Printed + ":");
}

Expected<void> AsmTextStreamer::emitInstruction(const MCInst &Instruction) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (!Instruction.opcode().isValid())
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "MC instruction has an invalid opcode");
  for (const MCOperand &Operand : Instruction.operands()) {
    if (const MCExprRef *Expression = Operand.expressionValue(); Expression != nullptr) {
      if (!*Expression)
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "MC instruction contains a null expression operand");
      auto Verification = (*Expression)->verify();
      if (!Verification)
        return std::unexpected(Verification.error());
    }
  }
  auto Text = Printer->printInstruction(Instruction);
  if (!Text)
    return std::unexpected(Text.error());
  return appendLine("  ", *Text);
}

Expected<void> AsmTextStreamer::emitValue(MCExprRef Value, std::uint8_t Size) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (!Value)
    return makeUnexpected(ErrorCode::InvalidArgument, "cannot emit a null MC value expression");
  std::string_view Directive;
  switch (Size) {
  case 1:
    Directive = ".byte ";
    break;
  case 2:
    Directive = ".2byte ";
    break;
  case 4:
    Directive = ".4byte ";
    break;
  case 8:
    Directive = ".8byte ";
    break;
  default:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly value size must be 1, 2, 4, or 8 bytes");
  }
  auto Printed = printMCExpression(*Value, *Printer);
  if (!Printed)
    return std::unexpected(Printed.error());
  return appendLine("  ", std::string(Directive) + *Printed);
}

Expected<void> AsmTextStreamer::emitBytes(std::span<const std::uint8_t> Bytes) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  for (std::size_t Begin = 0; Begin < Bytes.size(); Begin += 16) {
    const std::size_t End = std::min(Bytes.size(), Begin + 16);
    std::string Directive = ".byte ";
    for (std::size_t Index = Begin; Index < End; ++Index) {
      if (Index != Begin)
        Directive += ", ";
      Directive += byteSpelling(Bytes[Index]);
    }
    auto Emitted = appendLine("  ", Directive);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }
  return {};
}

Expected<void> AsmTextStreamer::emitZeroFill(std::uint64_t Size) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (Size == 0)
    return {};
  return appendLine("  ", ".zero " + std::to_string(Size));
}

Expected<void> AsmTextStreamer::emitAlignment(std::uint64_t Alignment, std::uint8_t Fill,
                                              std::uint64_t MaximumBytes) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (!std::has_single_bit(Alignment)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "assembly alignment must be a nonzero power of two");
  }
  std::string Directive = ".p2align " + std::to_string(log2Exact(Alignment));
  if (Fill != 0 || MaximumBytes != 0)
    Directive += ", " + std::to_string(Fill);
  if (MaximumBytes != 0)
    Directive += ", " + std::to_string(MaximumBytes);
  return appendLine("  ", Directive);
}

Expected<void> AsmTextStreamer::emitSymbolAttribute(MCSymbolRef Symbol,
                                                    MCSymbolAttribute Attribute) {
  if (IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit a symbol attribute after assembly finalization");
  if (!Symbol)
    return makeUnexpected(ErrorCode::InvalidArgument, "symbol attribute requires an MC symbol");
  auto Name = printMCSymbolName(Symbol->name());
  if (!Name)
    return std::unexpected(Name.error());

  switch (Attribute) {
  case MCSymbolAttribute::Global:
    return appendLine({}, ".globl " + *Name);
  case MCSymbolAttribute::Weak:
    return appendLine({}, ".weak " + *Name);
  case MCSymbolAttribute::Local:
    return appendLine({}, ".local " + *Name);
  case MCSymbolAttribute::Hidden:
    return appendLine({}, ".hidden " + *Name);
  case MCSymbolAttribute::Protected:
    return appendLine({}, ".protected " + *Name);
  case MCSymbolAttribute::Function:
    return appendLine({}, ".type " + *Name + ", @function");
  case MCSymbolAttribute::Object:
    return appendLine({}, ".type " + *Name + ", @object");
  case MCSymbolAttribute::TLSObject:
    return appendLine({}, ".type " + *Name + ", @tls_object");
  }
  return makeUnexpected(ErrorCode::InternalError, "MC symbol attribute is invalid");
}

Expected<void> AsmTextStreamer::emitSymbolSize(MCSymbolRef Symbol, MCExprRef Size) {
  if (IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit a symbol size after assembly finalization");
  if (!Symbol || !Size)
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "symbol size event requires a symbol and an expression");
  auto Name = printMCSymbolName(Symbol->name());
  if (!Name)
    return std::unexpected(Name.error());
  auto PrintedSize = printMCExpression(*Size, *Printer);
  if (!PrintedSize)
    return std::unexpected(PrintedSize.error());
  return appendLine({}, ".size " + *Name + ", " + *PrintedSize);
}

Expected<void> AsmTextStreamer::emitCommonSymbol(MCSymbolRef Symbol, std::uint64_t Size,
                                                 std::uint64_t Alignment) {
  if (IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit a common symbol after assembly finalization");
  if (!Symbol || Size == 0 || Alignment == 0 || (Alignment & (Alignment - 1U)) != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "common symbol requires a symbol, size, and power-of-two alignment");
  }
  auto Name = printMCSymbolName(Symbol->name());
  if (!Name)
    return std::unexpected(Name.error());
  return appendLine({}, ".comm " + *Name + "," + std::to_string(Size) + "," +
                            std::to_string(Alignment));
}

Expected<void> AsmTextStreamer::emitDirective(std::string_view Name, std::string_view Arguments) {
  if (!isDirectiveName(Name) || containsInvalidLineCharacter(Arguments)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "assembly directive name or arguments are invalid");
  }
  std::string Text = "." + std::string(Name);
  if (!Arguments.empty())
    Text += " " + std::string(Arguments);
  return appendLine({}, Text);
}

Expected<void> AsmTextStreamer::emitCFI(const MCCFIInstruction &Instruction) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  auto Verification = Instruction.verify();
  if (!Verification)
    return std::unexpected(Verification.error());

  const std::string Operands = joinIntegerOperands(Instruction.Operands);
  switch (Instruction.Operation) {
  case MCCFIOperation::StartProc:
    return emitDirective("cfi_startproc");
  case MCCFIOperation::EndProc:
    return emitDirective("cfi_endproc");
  case MCCFIOperation::Sections:
    return emitDirective("cfi_sections", ".eh_frame");
  case MCCFIOperation::Personality:
  case MCCFIOperation::LSDA: {
    auto Symbol = printMCSymbolName(Instruction.Symbol->name());
    if (!Symbol)
      return std::unexpected(Symbol.error());
    return emitDirective(Instruction.Operation == MCCFIOperation::Personality ? "cfi_personality"
                                                                              : "cfi_lsda",
                         Operands + ", " + *Symbol);
  }
  case MCCFIOperation::SignalFrame:
    return emitDirective("cfi_signal_frame");
  case MCCFIOperation::ReturnColumn:
    return emitDirective("cfi_return_column", Operands);
  case MCCFIOperation::SameValue:
    return emitDirective("cfi_same_value", Operands);
  case MCCFIOperation::RememberState:
    return emitDirective("cfi_remember_state");
  case MCCFIOperation::RestoreState:
    return emitDirective("cfi_restore_state");
  case MCCFIOperation::Offset:
    return emitDirective("cfi_offset", Operands);
  case MCCFIOperation::DefCFARegister:
    return emitDirective("cfi_def_cfa_register", Operands);
  case MCCFIOperation::DefCFAOffset:
    return emitDirective("cfi_def_cfa_offset", Operands);
  case MCCFIOperation::DefCFA:
    return emitDirective("cfi_def_cfa", Operands);
  case MCCFIOperation::RelativeOffset:
    return emitDirective("cfi_rel_offset", Operands);
  case MCCFIOperation::AdjustCFAOffset:
    return emitDirective("cfi_adjust_cfa_offset", Operands);
  case MCCFIOperation::Escape: {
    std::string Bytes;
    for (const std::uint8_t Byte : Instruction.Bytes) {
      if (!Bytes.empty())
        Bytes += ", ";
      Bytes += byteSpelling(Byte);
    }
    return emitDirective("cfi_escape", Bytes);
  }
  case MCCFIOperation::Restore:
    return emitDirective("cfi_restore", Operands);
  case MCCFIOperation::Undefined:
    return emitDirective("cfi_undefined", Operands);
  case MCCFIOperation::Register:
    return emitDirective("cfi_register", Operands);
  case MCCFIOperation::WindowSave:
    return emitDirective("cfi_window_save");
  case MCCFIOperation::NegateReturnAddressState:
    return emitDirective("cfi_negate_ra_state");
  case MCCFIOperation::NegateReturnAddressStateWithPC:
    return emitDirective("cfi_negate_ra_state_with_pc");
  case MCCFIOperation::GNUArgsSize: {
    if (Instruction.Operands.front() < 0) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "GNU CFI argument size cannot be negative");
    }
    std::vector<std::uint8_t> Bytes{0x2eU};
    const auto Encoded = encodeULEB128(static_cast<std::uint64_t>(Instruction.Operands.front()));
    Bytes.insert(Bytes.end(), Encoded.begin(), Encoded.end());
    return emitCFI(MCCFIInstruction{.Operation = MCCFIOperation::Escape,
                                    .Operands = {},
                                    .Bytes = std::move(Bytes),
                                    .Symbol = {}});
  }
  case MCCFIOperation::Label:
    return emitLabel(Instruction.Symbol);
  case MCCFIOperation::ValueOffset:
    return emitDirective("cfi_val_offset", Operands);
  }
  return makeUnexpected(ErrorCode::InternalError, "MC CFI operation is invalid");
}

Expected<void> AsmTextStreamer::emitInlineAssembly(std::string_view Text) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (Text.find('\0') != std::string_view::npos || Text.find('\r') != std::string_view::npos) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "inline assembly contains an invalid character");
  }
  if (Text.empty())
    return {};
  Output.append(Text);
  if (Text.back() != '\n')
    Output.push_back('\n');
  return {};
}

Expected<void> AsmTextStreamer::emitComment(std::string_view Text) {
  auto Active = requireActiveSection();
  if (!Active)
    return std::unexpected(Active.error());
  if (Text.empty() || containsInvalidLineCharacter(Text))
    return makeUnexpected(ErrorCode::InvalidArgument, "assembly comment is invalid");
  return appendLine("  # ", Text);
}

Expected<void> AsmTextStreamer::finish() {
  if (IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly stream was already finalized");
  IsFinished = true;
  return {};
}

Expected<std::string> AsmTextStreamer::takeOutput() {
  if (!IsFinished)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly output cannot be taken before finalization");
  if (OutputWasTaken)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "assembly output was already taken");
  OutputWasTaken = true;
  return std::move(Output);
}

} // namespace rcc::mc
