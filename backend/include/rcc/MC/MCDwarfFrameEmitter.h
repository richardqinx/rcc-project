// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCDWARFFRAMEEMITTER_H
#define RCC_MC_MCDWARFFRAMEEMITTER_H

#include "rcc/MC/MCAsmBackend.h"
#include "rcc/MC/MCCFI.h"
#include "rcc/MC/MCObjectWriter.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rcc::mc {

struct MCDwarfFrameInfo final {
  std::uint64_t CodeAlignment = 1;
  std::int64_t DataAlignment = -8;
  std::uint32_t ReturnAddressRegister = 0;
  std::uint32_t InitialCFARegister = 0;
  std::uint64_t InitialCFAOffset = 0;

  [[nodiscard]] Expected<void> verify() const;
};

struct MCDwarfFrameInstruction final {
  MCCFIInstruction Instruction;
  std::uint64_t Offset = 0;
};

struct MCDwarfFrameProcedure final {
  MCSymbolRef Function;
  std::uint64_t AddressRange = 0;
  std::vector<MCDwarfFrameInstruction> Instructions;
};

[[nodiscard]] Expected<MCObjectSection>
emitDwarfEHFrame(MCSectionRef Section, std::span<const MCDwarfFrameProcedure> Procedures,
                 const MCDwarfFrameInfo &Info, const MCAsmBackend &Backend);

} // namespace rcc::mc

#endif // RCC_MC_MCDWARFFRAMEEMITTER_H
