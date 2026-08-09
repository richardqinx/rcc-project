// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCObjectStreamer.h"

#include <utility>

namespace rcc::mc {

Expected<void> ELFObjectStreamer::ensureOpen() const {
  if (IsFinished)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "object stream is already finalized");
  return {};
}

Expected<void> ELFObjectStreamer::switchSection(MCSectionRef Section) {
  auto Open = ensureOpen();
  return Open ? Assembler.switchSection(std::move(Section)) : Open;
}

Expected<void> ELFObjectStreamer::emitLabel(MCSymbolRef Symbol) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitLabel(std::move(Symbol)) : Open;
}

Expected<void> ELFObjectStreamer::emitInstruction(const MCInst &Instruction) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitInstruction(Instruction) : Open;
}

Expected<void> ELFObjectStreamer::emitValue(MCExprRef Value, std::uint8_t Size) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitValue(std::move(Value), Size) : Open;
}

Expected<void> ELFObjectStreamer::emitBytes(std::span<const std::uint8_t> Bytes) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitBytes(Bytes) : Open;
}

Expected<void> ELFObjectStreamer::emitZeroFill(std::uint64_t Size) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitZeroFill(Size) : Open;
}

Expected<void> ELFObjectStreamer::emitAlignment(std::uint64_t Alignment, std::uint8_t Fill,
                                                std::uint64_t MaximumBytes) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitAlignment(Alignment, Fill, MaximumBytes) : Open;
}

Expected<void> ELFObjectStreamer::emitSymbolAttribute(MCSymbolRef Symbol,
                                                      MCSymbolAttribute Attribute) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitSymbolAttribute(std::move(Symbol), Attribute) : Open;
}

Expected<void> ELFObjectStreamer::emitSymbolSize(MCSymbolRef Symbol, MCExprRef Size) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitSymbolSize(std::move(Symbol), std::move(Size)) : Open;
}

Expected<void> ELFObjectStreamer::emitCommonSymbol(MCSymbolRef Symbol, std::uint64_t Size,
                                                   std::uint64_t Alignment) {
  auto Open = ensureOpen();
  return Open ? Assembler.emitCommonSymbol(std::move(Symbol), Size, Alignment) : Open;
}

Expected<void> ELFObjectStreamer::emitDirective(std::string_view Name, std::string_view) {
  auto Open = ensureOpen();
  if (!Open)
    return Open;
  if (Name == "attribute")
    return {};
  return makeUnexpected(ErrorCode::Unsupported,
                        "native object emission does not support the MC directive '" +
                            std::string(Name) + "'");
}

Expected<void> ELFObjectStreamer::emitCFI(const MCCFIInstruction &Instruction) {
  auto Open = ensureOpen();
  if (!Open)
    return Open;
  return Assembler.emitCFI(Instruction);
}

Expected<void> ELFObjectStreamer::emitInlineAssembly(std::string_view Text) {
  auto Open = ensureOpen();
  if (!Open)
    return Open;
  if (Text.empty())
    return {};
  return makeUnexpected(ErrorCode::Unsupported,
                        "native object emission requires parsed inline assembly");
}

Expected<void> ELFObjectStreamer::emitComment(std::string_view) {
  return ensureOpen();
}

Expected<void> ELFObjectStreamer::configureDwarfFrame(MCSectionRef Section, MCDwarfFrameInfo Info) {
  auto Open = ensureOpen();
  if (!Open)
    return Open;
  return Assembler.configureDwarfFrame(std::move(Section), Info);
}

Expected<void> ELFObjectStreamer::finish() {
  auto Open = ensureOpen();
  if (!Open)
    return Open;
  auto Object = Assembler.assemble();
  if (!Object)
    return std::unexpected(Object.error());
  auto Written = Writer->writeObject(*Object);
  if (!Written)
    return std::unexpected(Written.error());
  Output = std::move(*Written);
  IsFinished = true;
  return {};
}

Expected<std::vector<std::uint8_t>> ELFObjectStreamer::takeOutput() {
  if (!IsFinished)
    return makeUnexpected(ErrorCode::ObjectWritingFailed,
                          "object output cannot be taken before finalization");
  if (OutputWasTaken)
    return makeUnexpected(ErrorCode::ObjectWritingFailed, "object output was already taken");
  OutputWasTaken = true;
  return std::move(Output);
}

} // namespace rcc::mc
