// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionAttributes.h"

#include <cstdint>
#include <string_view>

namespace rcc {

std::string_view getMachineFunctionAttributeName(MachineFunctionAttribute Attribute) noexcept {
  switch (Attribute) {
  case MachineFunctionAttribute::ExposesReturnsTwice:
    return "ExposesReturnsTwice";
  case MachineFunctionAttribute::HasWinCFI:
    return "HasWinCFI";
  case MachineFunctionAttribute::HasFakeUses:
    return "HasFakeUses";
  case MachineFunctionAttribute::CallsEHReturn:
    return "CallsEHReturn";
  case MachineFunctionAttribute::CallsUnwindInit:
    return "CallsUnwindInit";
  case MachineFunctionAttribute::HasEHContTarget:
    return "HasEHContTarget";
  case MachineFunctionAttribute::HasEHScopes:
    return "HasEHScopes";
  case MachineFunctionAttribute::HasEHFunclets:
    return "HasEHFunclets";
  case MachineFunctionAttribute::IsOutlined:
    return "IsOutlined";
  case MachineFunctionAttribute::DebugInstrRef:
    return "DebugInstrRef";
  case MachineFunctionAttribute::TracksDebugUserValues:
    return "TracksDebugUserValues";
  case MachineFunctionAttribute::Count:
    return "Invalid";
  }
  return "Invalid";
}

Expected<MachineFunctionAttributes>
MachineFunctionAttributes::fromRawFlags(std::uint16_t RawFlags) {
  if ((RawFlags & static_cast<std::uint16_t>(~ValidFlags)) != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine function attributes contain unknown flag bits");
  }
  MachineFunctionAttributes Result;
  Result.Flags = RawFlags;
  return Result;
}

Expected<void> MachineFunctionAttributes::verify() const {
  if ((Flags & static_cast<std::uint16_t>(~ValidFlags)) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine function attributes contain unknown flag bits");
  }
  if (FunctionAlignment && FunctionAlignment->bytes() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine function alignment must be nonzero");
  }
  return {};
}

} // namespace rcc
