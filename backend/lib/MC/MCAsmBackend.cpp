// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCAsmBackend.h"

namespace rcc::mc {

Expected<std::vector<MCFixupRelocation>>
MCAsmBackend::relocationsForFixup(const MCFixup &Fixup, const MCValue &Value) const {
  if (!Value.AddSymbol || Value.SubtractSymbol) {
    return makeUnexpected(ErrorCode::RelocationFailed,
                          "MC backend cannot represent this expression as one relocation");
  }
  auto Type = relocationType(Fixup, Value);
  if (!Type)
    return std::unexpected(Type.error());
  return std::vector<MCFixupRelocation>{
      {.Type = *Type, .Symbol = Value.AddSymbol, .Addend = Value.Addend}};
}

Expected<MCInst> MCAsmBackend::relaxInstruction(const MCInst &) const {
  return makeUnexpected(ErrorCode::InstructionEncodingFailed,
                        "target does not support MC instruction relaxation");
}

} // namespace rcc::mc
