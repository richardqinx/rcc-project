// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCCFI.h"

#include <cstddef>

namespace rcc::mc {

Expected<void> MCCFIInstruction::verify() const {
  std::size_t ExpectedOperands = 0;
  bool RequiresBytes = false;
  bool RequiresSymbol = false;

  switch (Operation) {
  case MCCFIOperation::StartProc:
  case MCCFIOperation::EndProc:
  case MCCFIOperation::Sections:
  case MCCFIOperation::SignalFrame:
  case MCCFIOperation::RememberState:
  case MCCFIOperation::RestoreState:
  case MCCFIOperation::WindowSave:
  case MCCFIOperation::NegateReturnAddressState:
  case MCCFIOperation::NegateReturnAddressStateWithPC:
    break;
  case MCCFIOperation::Personality:
  case MCCFIOperation::LSDA:
    ExpectedOperands = 1;
    RequiresSymbol = true;
    break;
  case MCCFIOperation::ReturnColumn:
  case MCCFIOperation::SameValue:
  case MCCFIOperation::DefCFARegister:
  case MCCFIOperation::DefCFAOffset:
  case MCCFIOperation::AdjustCFAOffset:
  case MCCFIOperation::Restore:
  case MCCFIOperation::Undefined:
  case MCCFIOperation::GNUArgsSize:
    ExpectedOperands = 1;
    break;
  case MCCFIOperation::Offset:
  case MCCFIOperation::DefCFA:
  case MCCFIOperation::RelativeOffset:
  case MCCFIOperation::Register:
  case MCCFIOperation::ValueOffset:
    ExpectedOperands = 2;
    break;
  case MCCFIOperation::Escape:
    RequiresBytes = true;
    break;
  case MCCFIOperation::Label:
    RequiresSymbol = true;
    break;
  }

  if (Operands.size() != ExpectedOperands)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC CFI event has an invalid operand count");
  if (RequiresBytes != !Bytes.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          RequiresBytes ? "MC CFI escape event requires bytes"
                                        : "non-escape MC CFI event cannot contain bytes");
  }
  if (RequiresSymbol != static_cast<bool>(Symbol)) {
    return makeUnexpected(ErrorCode::InvalidArgument, RequiresSymbol
                                                          ? "MC CFI event requires a symbol"
                                                          : "MC CFI event cannot contain a symbol");
  }
  return {};
}

} // namespace rcc::mc
