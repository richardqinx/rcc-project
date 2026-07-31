// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/Register.h"

#include <string>

namespace rcc {
namespace {

constexpr std::uint64_t MaximumRegisterIndex = (std::uint64_t{1} << 31) - 1;

} // namespace

Expected<PhysicalRegister> PhysicalRegister::create(std::uint64_t Number) {
  if (Number == 0 || Number > MaximumRegisterIndex) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "physical register number must be in the range [1, 2147483647]");
  }
  return PhysicalRegister(static_cast<std::uint32_t>(Number));
}

Expected<Register> Register::makePhysical(std::uint64_t Number) {
  auto PhysReg = PhysicalRegister::create(Number);
  if (!PhysReg)
    return std::unexpected(PhysReg.error());
  return fromPhysical(*PhysReg);
}

Expected<Register> Register::makeVirtual(std::uint64_t Index) {
  if (Index > MaximumRegisterIndex) {
    return makeUnexpected(ErrorCode::InvalidArgument, "virtual register index must fit in 31 bits");
  }
  return Register(VirtualBit | static_cast<std::uint32_t>(Index));
}

std::optional<PhysicalRegister> Register::physicalRegister() const noexcept {
  if (!isPhysical())
    return std::nullopt;
  return PhysicalRegister(OpaqueValue);
}

std::optional<std::uint32_t> Register::virtualIndex() const noexcept {
  if (!isVirtual())
    return std::nullopt;
  return OpaqueValue & IndexMask;
}

std::string Register::toString() const {
  if (isNoRegister())
    return "noreg";
  if (isVirtual())
    return "%" + std::to_string(OpaqueValue & IndexMask);
  return "$physreg" + std::to_string(OpaqueValue);
}

} // namespace rcc
