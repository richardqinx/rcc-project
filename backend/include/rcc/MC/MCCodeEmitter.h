// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCCODEEMITTER_H
#define RCC_MC_MCCODEEMITTER_H

#include "rcc/MC/MCFixup.h"
#include "rcc/MC/MCInst.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <vector>

namespace rcc::mc {

struct MCEncodedInstruction final {
  std::vector<std::uint8_t> Bytes;
  std::vector<MCFixup> Fixups;
};

class MCCodeEmitter {
public:
  virtual ~MCCodeEmitter() = default;

  [[nodiscard]] virtual Expected<MCEncodedInstruction>
  encodeInstruction(const MCInst &Instruction) const = 0;
};

} // namespace rcc::mc

#endif // RCC_MC_MCCODEEMITTER_H
