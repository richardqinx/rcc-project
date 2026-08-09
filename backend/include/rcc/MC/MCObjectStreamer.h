// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCOBJECTSTREAMER_H
#define RCC_MC_MCOBJECTSTREAMER_H

#include "rcc/MC/MCAssembler.h"
#include "rcc/MC/MCObjectWriter.h"
#include "rcc/MC/MCStreamer.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rcc::mc {

class ELFObjectStreamer final : public MCStreamer {
public:
  ELFObjectStreamer(const MCCodeEmitter &Emitter, const MCAsmBackend &Backend,
                    const MCObjectWriter &Writer) noexcept
      : Assembler(Emitter, Backend), Writer(&Writer) {}

  [[nodiscard]] Expected<void> switchSection(MCSectionRef Section) override;
  [[nodiscard]] Expected<void> emitLabel(MCSymbolRef Symbol) override;
  [[nodiscard]] Expected<void> emitInstruction(const MCInst &Instruction) override;
  [[nodiscard]] Expected<void> emitValue(MCExprRef Value, std::uint8_t Size) override;
  [[nodiscard]] Expected<void> emitBytes(std::span<const std::uint8_t> Bytes) override;
  [[nodiscard]] Expected<void> emitZeroFill(std::uint64_t Size) override;
  [[nodiscard]] Expected<void> emitAlignment(std::uint64_t Alignment, std::uint8_t Fill = 0,
                                             std::uint64_t MaximumBytes = 0) override;
  [[nodiscard]] Expected<void> emitSymbolAttribute(MCSymbolRef Symbol,
                                                   MCSymbolAttribute Attribute) override;
  [[nodiscard]] Expected<void> emitSymbolSize(MCSymbolRef Symbol, MCExprRef Size) override;
  [[nodiscard]] Expected<void> emitCommonSymbol(MCSymbolRef Symbol, std::uint64_t Size,
                                                std::uint64_t Alignment) override;
  [[nodiscard]] Expected<void> emitDirective(std::string_view Name,
                                             std::string_view Arguments = {}) override;
  [[nodiscard]] Expected<void> emitCFI(const MCCFIInstruction &Instruction) override;
  [[nodiscard]] Expected<void> emitInlineAssembly(std::string_view Text) override;
  [[nodiscard]] Expected<void> emitComment(std::string_view Text) override;
  [[nodiscard]] Expected<void> finish() override;

  [[nodiscard]] Expected<void> configureDwarfFrame(MCSectionRef Section, MCDwarfFrameInfo Info);

  [[nodiscard]] Expected<std::vector<std::uint8_t>> takeOutput();

private:
  [[nodiscard]] Expected<void> ensureOpen() const;

  MCAssembler Assembler;
  const MCObjectWriter *Writer;
  std::vector<std::uint8_t> Output;
  bool IsFinished = false;
  bool OutputWasTaken = false;
};

} // namespace rcc::mc

#endif // RCC_MC_MCOBJECTSTREAMER_H
