// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCASSEMBLER_H
#define RCC_MC_MCASSEMBLER_H

#include "rcc/MC/MCAsmBackend.h"
#include "rcc/MC/MCCodeEmitter.h"
#include "rcc/MC/MCDwarfFrameEmitter.h"
#include "rcc/MC/MCObjectWriter.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rcc::mc {

class MCAssembler final {
public:
  MCAssembler(const MCCodeEmitter &Emitter, const MCAsmBackend &Backend) noexcept
      : Emitter(&Emitter), Backend(&Backend) {}

  [[nodiscard]] Expected<void> switchSection(MCSectionRef Section);
  [[nodiscard]] Expected<void> emitLabel(MCSymbolRef Symbol);
  [[nodiscard]] Expected<void> emitInstruction(const MCInst &Instruction);
  [[nodiscard]] Expected<void> emitValue(MCExprRef Value, std::uint8_t Size);
  [[nodiscard]] Expected<void> emitBytes(std::span<const std::uint8_t> Bytes);
  [[nodiscard]] Expected<void> emitZeroFill(std::uint64_t Size);
  [[nodiscard]] Expected<void> emitAlignment(std::uint64_t Alignment, std::uint8_t Fill,
                                             std::uint64_t MaximumBytes);
  [[nodiscard]] Expected<void> emitSymbolAttribute(MCSymbolRef Symbol, MCSymbolAttribute Attribute);
  [[nodiscard]] Expected<void> emitSymbolSize(MCSymbolRef Symbol, MCExprRef Size);
  [[nodiscard]] Expected<void> emitCommonSymbol(MCSymbolRef Symbol, std::uint64_t Size,
                                                std::uint64_t Alignment);
  [[nodiscard]] Expected<void> emitCFI(const MCCFIInstruction &Instruction);
  [[nodiscard]] Expected<void> configureDwarfFrame(MCSectionRef Section, MCDwarfFrameInfo Info);

  [[nodiscard]] Expected<MCAssembledObject> assemble();

private:
  struct InstructionFragment final {
    MCInst Instruction;
  };
  struct DataFragment final {
    std::vector<std::uint8_t> Bytes;
    std::vector<MCFixup> Fixups;
  };
  struct AlignmentFragment final {
    std::uint64_t Alignment = 1;
    std::uint8_t Fill = 0;
    std::uint64_t MaximumBytes = 0;
  };
  struct ZeroFillFragment final {
    std::uint64_t Size = 0;
  };
  using FragmentPayload =
      std::variant<InstructionFragment, DataFragment, AlignmentFragment, ZeroFillFragment>;

  struct Fragment final {
    FragmentPayload Payload;
    std::uint64_t Offset = 0;
    std::uint64_t Size = 0;
    std::vector<std::uint8_t> EncodedBytes;
    std::vector<MCFixup> Fixups;
  };

  struct SectionData final {
    MCSectionRef Section;
    std::vector<Fragment> Fragments;
    std::uint64_t AddressSize = 0;
    std::uint64_t Alignment = 1;
  };

  struct SymbolData final {
    MCSymbolRef Symbol;
    MCObjectSymbolBinding Binding = MCObjectSymbolBinding::Local;
    bool BindingWasSet = false;
    MCObjectSymbolType Type = MCObjectSymbolType::None;
    MCObjectSymbolVisibility Visibility = MCObjectSymbolVisibility::Default;
    std::optional<std::size_t> SectionIndex;
    std::size_t FragmentIndex = 0;
    MCExprRef Size;
    std::uint64_t CommonSize = 0;
    std::uint64_t CommonAlignment = 0;
  };

  struct CFIEvent final {
    MCCFIInstruction Instruction;
    std::size_t FragmentIndex = 0;
  };

  struct CFIProcedure final {
    MCSymbolRef Function;
    std::size_t SectionIndex = 0;
    std::size_t BeginFragmentIndex = 0;
    std::size_t EndFragmentIndex = 0;
    std::vector<CFIEvent> Events;
    bool IsClosed = false;
  };

  [[nodiscard]] Expected<std::size_t> currentSectionIndex() const;
  [[nodiscard]] SymbolData &getOrCreateSymbol(MCSymbolRef Symbol);
  [[nodiscard]] Expected<void> encodeFragments();
  [[nodiscard]] Expected<bool> layoutAndRelax();
  [[nodiscard]] Expected<MCAssembledObject> finalizeObject();
  [[nodiscard]] Expected<void> appendDwarfFrame(MCAssembledObject &Object) const;
  [[nodiscard]] std::optional<std::uint64_t> symbolOffset(const SymbolData &Symbol) const;

  const MCCodeEmitter *Emitter;
  const MCAsmBackend *Backend;
  std::vector<SectionData> Sections;
  std::unordered_map<const MCSection *, std::size_t> SectionIndices;
  std::vector<SymbolData> Symbols;
  std::unordered_map<const MCSymbol *, std::size_t> SymbolIndices;
  std::optional<std::size_t> CurrentSection;
  std::vector<CFIProcedure> CFIProcedures;
  std::optional<std::size_t> ActiveCFIProcedure;
  MCSectionRef EHFrameSection;
  std::optional<MCDwarfFrameInfo> DwarfFrameInfo;
  bool WasAssembled = false;
};

} // namespace rcc::mc

#endif // RCC_MC_MCASSEMBLER_H
