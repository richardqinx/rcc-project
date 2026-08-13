// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFunctionProperties.h"

#include <array>
#include <string>

namespace rcc {
namespace {

constexpr std::array PropertyNames{
    std::string_view{"IsSSA"},
    std::string_view{"NoPHIs"},
    std::string_view{"TracksLiveness"},
    std::string_view{"Legalized"},
    std::string_view{"RegBankSelected"},
    std::string_view{"Selected"},
    std::string_view{"TiedOpsRewritten"},
    std::string_view{"NoVRegs"},
    std::string_view{"FrameFinalized"},
    std::string_view{"NoFrameIndices"},
    std::string_view{"NoExecutablePseudos"},
    std::string_view{"FailedISel"},
    std::string_view{"FailsVerification"},
};

static_assert(PropertyNames.size() == static_cast<std::size_t>(MachineFunctionProperty::Count));

} // namespace

std::string_view getMachineFunctionPropertyName(MachineFunctionProperty Property) noexcept {
  const auto Index = static_cast<std::size_t>(Property);
  if (Index >= PropertyNames.size())
    return "Unknown";
  return PropertyNames[Index];
}

MachineFunctionProperties::MachineFunctionProperties(
    std::initializer_list<MachineFunctionProperty> Properties) noexcept {
  for (const MachineFunctionProperty Property : Properties)
    set(Property);
}

Expected<MachineFunctionProperties> MachineFunctionProperties::fromRaw(std::uint64_t RawValue) {
  if ((RawValue & ~ValidMask) != 0) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machine function property mask contains unknown bits");
  }
  return MachineFunctionProperties(RawValue);
}

Expected<void> MachineFunctionProperties::applyTransition(MachineFunctionProperties Required,
                                                          MachineFunctionProperties Sets,
                                                          MachineFunctionProperties Clears) {
  if (Sets.intersects(Clears)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "a machine function property transition cannot set and clear the "
                          "same property");
  }

  const MachineFunctionProperties Missing = Required.missingFrom(*this);
  if (!Missing.empty()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "missing required machine function properties: " + Missing.toString());
  }

  Mask = (Mask | Sets.Mask) & ~Clears.Mask;
  return {};
}

std::string MachineFunctionProperties::toString() const {
  if (empty())
    return "none";

  std::string Result;
  for (std::size_t Index = 0; Index < PropertyNames.size(); ++Index) {
    if ((Mask & (std::uint64_t{1} << Index)) == 0)
      continue;
    if (!Result.empty())
      Result += ", ";
    Result += PropertyNames[Index];
  }
  return Result;
}

} // namespace rcc
