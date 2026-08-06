// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] PhysicalRegister physical(PhysicalRegisterID ID) {
  const auto Register = physicalRegister(ID);
  assert(Register.has_value());
  return *Register;
}

[[nodiscard]] RegisterUnitLane unitLane(RegisterUnit Unit, std::uint64_t Lanes) {
  return {
      .Unit = RegisterUnitID::fromValue(static_cast<std::uint32_t>(Unit)),
      .Lanes = LaneMask::fromRaw(Lanes),
  };
}

[[nodiscard]] std::vector<PhysicalRegister>
physicalRegisters(std::initializer_list<PhysicalRegisterID> IDs) {
  std::vector<PhysicalRegister> Result;
  Result.reserve(IDs.size());
  for (const PhysicalRegisterID ID : IDs)
    Result.push_back(physical(ID));
  return Result;
}

[[nodiscard]] RegisterClassDesc registerClass(
    RegisterClass ID, const char *Name, std::initializer_list<PhysicalRegisterID> Registers,
    std::initializer_list<PhysicalRegisterID> AllocationOrder, RegisterBank Bank,
    std::uint32_t SizeInBits, std::uint32_t SpillSizeInBits, std::uint32_t SpillAlignmentInBits,
    std::uint64_t Lanes, bool Allocatable, std::uint32_t CopyCost) {
  return {
      .ID = RegisterClassID::fromValue(static_cast<std::uint32_t>(ID)),
      .Name = Name,
      .Registers = physicalRegisters(Registers),
      .AllocationOrder = physicalRegisters(AllocationOrder),
      .SizeInBits = SizeInBits,
      .SpillSizeInBits = SpillSizeInBits,
      .SpillAlignmentInBits = SpillAlignmentInBits,
      .Lanes = LaneMask::fromRaw(Lanes),
      .SupportedSubRegs = {},
      .SuperClasses = {},
      .DefaultBank = RegisterBankID::fromValue(static_cast<std::uint32_t>(Bank)),
      .Allocatable = Allocatable,
      .CopyCost = CopyCost,
  };
}

[[nodiscard]] std::shared_ptr<const RegisterMask>
makePreservedMask(std::initializer_list<PhysicalRegisterID> Preserved) {
  std::vector<std::uint64_t> Words;
  for (const PhysicalRegisterID ID : Preserved) {
    const std::uint32_t Number = static_cast<std::uint32_t>(ID);
    const std::size_t Word = Number / 64U;
    if (Words.size() <= Word)
      Words.resize(Word + 1U);
    Words[Word] |= std::uint64_t{1} << (Number % 64U);
  }
  return std::make_shared<const RegisterMask>(std::move(Words));
}

#include "RISCVGenTargetDescription.inc"

} // namespace

Expected<PhysicalRegister> physicalRegister(PhysicalRegisterID ID) {
  return PhysicalRegister::create(static_cast<std::uint32_t>(ID));
}

Expected<TargetRegisterInfo> createRegisterInfo() {
  TargetRegisterInfoDesc Description;
  appendGeneratedRegisterInfo(Description);
  return TargetRegisterInfo::create(std::move(Description));
}

Expected<TargetMIRTextInfo> createMIRTextInfo() {
  auto Registers = createRegisterInfo();
  if (!Registers)
    return std::unexpected(Registers.error());
  TargetMIRTextInfoDesc Description;
  appendGeneratedMIRTextInfo(Description);
  return TargetMIRTextInfo::create(std::move(Description), *Registers);
}

std::shared_ptr<const RegisterMask> createCallPreservedMask() {
  TargetMIRTextInfoDesc Description;
  appendGeneratedMIRTextInfo(Description);
  assert(!Description.RegisterMasks.empty());
  return Description.RegisterMasks.front().Mask;
}

} // namespace rcc::riscv
