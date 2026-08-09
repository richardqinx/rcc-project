// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetMCInstLowering.h"

namespace rcc {

Expected<void> TargetMCInstLowering::emitTerminalInstruction(const MachineInstr &,
                                                             MachineMCContext &,
                                                             mc::MCStreamer &) const {
  return makeUnexpected(ErrorCode::Unsupported,
                        "target does not support this terminal emission instruction");
}

Expected<void> TargetMCInstLowering::emitFileHeader(MachineMCContext &, mc::MCStreamer &) const {
  return {};
}

Expected<void> TargetMCInstLowering::emitFileTrailer(MachineMCContext &, mc::MCStreamer &) const {
  return {};
}

Expected<void> TargetMCInstLowering::emitGenericConstant(const GenericConstantData &, std::uint64_t,
                                                         MachineMCContext &,
                                                         mc::MCStreamer &) const {
  return makeUnexpected(ErrorCode::Unsupported,
                        "target does not support assembly emission for generic constants");
}

Expected<void> TargetMCInstLowering::emitGlobalInitializer(const GlobalInitializer &,
                                                           MachineMCContext &,
                                                           mc::MCStreamer &) const {
  return makeUnexpected(ErrorCode::Unsupported,
                        "target does not support assembly emission for global initializers");
}

Expected<void> TargetMCInstLowering::emitTargetConstant(const TargetConstant &, MachineMCContext &,
                                                        mc::MCStreamer &) const {
  return makeUnexpected(ErrorCode::Unsupported,
                        "target does not support assembly emission for this target constant");
}

Expected<void> TargetMCInstLowering::emitTargetJumpTable(const MachineFunction &,
                                                         const MachineJumpTableEntry &,
                                                         MachineMCContext &,
                                                         mc::MCStreamer &) const {
  return makeUnexpected(ErrorCode::Unsupported, "target does not support this jump-table encoding");
}

} // namespace rcc
