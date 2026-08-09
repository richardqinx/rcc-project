// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCSTREAMER_H
#define RCC_MC_MCSTREAMER_H

#include "rcc/MC/MCCFI.h"
#include "rcc/MC/MCInst.h"
#include "rcc/MC/MCInstPrinter.h"
#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace rcc::mc {

enum class MCSymbolAttribute : std::uint8_t {
  Global,
  Weak,
  Local,
  Hidden,
  Protected,
  Function,
  Object,
  TLSObject,
};

class MCStreamer {
public:
  virtual ~MCStreamer() = default;

  [[nodiscard]] virtual Expected<void> switchSection(MCSectionRef Section) = 0;
  [[nodiscard]] virtual Expected<void> emitLabel(MCSymbolRef Symbol) = 0;
  [[nodiscard]] virtual Expected<void> emitInstruction(const MCInst &Instruction) = 0;
  [[nodiscard]] virtual Expected<void> emitValue(MCExprRef Value, std::uint8_t Size) = 0;
  [[nodiscard]] virtual Expected<void> emitBytes(std::span<const std::uint8_t> Bytes) = 0;
  [[nodiscard]] virtual Expected<void> emitZeroFill(std::uint64_t Size) = 0;
  [[nodiscard]] virtual Expected<void> emitAlignment(std::uint64_t Alignment, std::uint8_t Fill = 0,
                                                     std::uint64_t MaximumBytes = 0) = 0;
  [[nodiscard]] virtual Expected<void> emitSymbolAttribute(MCSymbolRef Symbol,
                                                           MCSymbolAttribute Attribute) = 0;
  [[nodiscard]] virtual Expected<void> emitSymbolSize(MCSymbolRef Symbol, MCExprRef Size) = 0;
  [[nodiscard]] virtual Expected<void> emitCommonSymbol(MCSymbolRef Symbol, std::uint64_t Size,
                                                        std::uint64_t Alignment) = 0;
  [[nodiscard]] virtual Expected<void> emitDirective(std::string_view Name,
                                                     std::string_view Arguments = {}) = 0;
  [[nodiscard]] virtual Expected<void> emitCFI(const MCCFIInstruction &Instruction) = 0;
  [[nodiscard]] virtual Expected<void> emitInlineAssembly(std::string_view Text) = 0;
  [[nodiscard]] virtual Expected<void> emitComment(std::string_view Text) = 0;
  [[nodiscard]] virtual Expected<void> finish() = 0;
};

class AsmTextStreamer final : public MCStreamer {
public:
  explicit AsmTextStreamer(const MCInstPrinter &Printer) noexcept : Printer(&Printer) {}

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

  [[nodiscard]] Expected<std::string> takeOutput();

private:
  [[nodiscard]] Expected<void> appendLine(std::string_view Prefix, std::string_view Text);
  [[nodiscard]] Expected<void> requireActiveSection() const;

  const MCInstPrinter *Printer;
  MCSectionRef CurrentSection;
  std::unordered_set<std::string> DefinedSymbols;
  std::string Output;
  bool IsFinished = false;
  bool OutputWasTaken = false;
};

} // namespace rcc::mc

#endif // RCC_MC_MCSTREAMER_H
