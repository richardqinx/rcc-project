// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineBasicBlockAttributes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace rcc {

std::string_view getMachineBasicBlockAttributeName(MachineBasicBlockAttribute Attribute) noexcept {
  switch (Attribute) {
  case MachineBasicBlockAttribute::MachineBlockAddressTaken:
    return "MachineBlockAddressTaken";
  case MachineBasicBlockAttribute::IRBlockAddressTaken:
    return "IRBlockAddressTaken";
  case MachineBasicBlockAttribute::LandingPad:
    return "LandingPad";
  case MachineBasicBlockAttribute::InlineAsmBrIndirectTarget:
    return "InlineAsmBrIndirectTarget";
  case MachineBasicBlockAttribute::EHFuncletEntry:
    return "EHFuncletEntry";
  case MachineBasicBlockAttribute::EHScopeEntry:
    return "EHScopeEntry";
  case MachineBasicBlockAttribute::Count:
    return "Invalid";
  }
  return "Invalid";
}

Expected<MachineBasicBlockAttributes>
MachineBasicBlockAttributes::fromRawFlags(std::uint8_t RawFlags) {
  if ((RawFlags & static_cast<std::uint8_t>(~ValidFlags)) != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine basic block attributes contain unknown flag bits");
  }
  MachineBasicBlockAttributes Result;
  Result.Flags = RawFlags;
  return Result;
}

Expected<void> MachineBasicBlockAttributes::setName(std::string NewName) {
  if (NewName.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument, "machine basic block name cannot be empty");
  }
  Name = std::move(NewName);
  return {};
}

Expected<void> MachineBasicBlockAttributes::setIRBlockName(std::string NewName) {
  if (NewName.empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine basic block IR name cannot be empty");
  }
  IRBlockName = std::move(NewName);
  return {};
}

Expected<void> MachineBasicBlockAttributes::verify() const {
  if ((Flags & static_cast<std::uint8_t>(~ValidFlags)) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine basic block attributes contain unknown flag bits");
  }
  if (Name && Name->empty()) {
    return makeUnexpected(ErrorCode::VerificationError, "machine basic block name cannot be empty");
  }
  if (IRBlockName && IRBlockName->empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine basic block IR name cannot be empty");
  }
  if (has(MachineBasicBlockAttribute::IRBlockAddressTaken) && !IRBlockName) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "IR-block-address-taken block requires an IR block reference");
  }
  if (BlockAlignment.bytes() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine basic block alignment must be nonzero");
  }
  return {};
}

} // namespace rcc
