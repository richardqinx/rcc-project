// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetMCInstLowering.h"

namespace rcc {

Expected<void> TargetMCInstLowering::emitTerminalInstruction(const MachineInstr &,
                                                             MachineMCContext &,
                                                             mc::MCStreamer &) const {
  return makeUnexpected(ErrorCode::Unsupported,
                        "target does not support this terminal emission instruction");
}

} // namespace rcc
