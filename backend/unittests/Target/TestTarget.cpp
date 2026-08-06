// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace rcc::test {
namespace {

[[nodiscard]] PhysicalRegister phys(std::uint32_t Number) {
  return *PhysicalRegister::create(Number);
}

[[nodiscard]] RegisterUnitDesc unit(std::uint32_t ID, const char *Name) {
  return RegisterUnitDesc{RegisterUnitID::fromValue(ID), Name};
}

[[nodiscard]] RegisterUnitLane unitLane(std::uint32_t ID, std::uint64_t Lanes) {
  return RegisterUnitLane{RegisterUnitID::fromValue(ID), LaneMask::fromRaw(Lanes)};
}

[[nodiscard]] PhysicalRegisterDesc physicalRegister(std::uint32_t Number, const char *Name,
                                                    std::uint32_t Encoding,
                                                    std::vector<RegisterUnitLane> Units) {
  return PhysicalRegisterDesc{phys(Number), Name,  Encoding, std::move(Units),
                              false,        false, false,    {}};
}

[[nodiscard]] RegisterClassDesc
registerClass(RegisterClassID ID, const char *Name, std::vector<PhysicalRegister> Registers,
              std::vector<PhysicalRegister> AllocationOrder, std::uint32_t SizeInBits,
              LaneMask Lanes, RegisterBankID DefaultBank,
              std::vector<SubRegIndex> SupportedSubRegs = {},
              std::vector<RegisterClassID> SuperClasses = {}, bool Allocatable = true,
              bool CoveredBySubRegs = false) {
  return RegisterClassDesc{
      ID,
      Name,
      std::move(Registers),
      std::move(AllocationOrder),
      SizeInBits,
      SizeInBits,
      SizeInBits,
      Lanes,
      std::move(SupportedSubRegs),
      std::move(SuperClasses),
      DefaultBank,
      Allocatable,
      1,
      CoveredBySubRegs,
  };
}

} // namespace

TargetRegisterInfoDesc createTestTargetRegisterInfoDescription() {
  TargetRegisterInfoDesc Description;
  Description.RegisterUnits = {
      unit(1, "g0.lo16"), unit(2, "g0.bits16_31"), unit(3, "g0.bits32_47"), unit(4, "g0.hi16"),
      unit(5, "g1.lo16"), unit(6, "g1.bits16_31"), unit(7, "g1.bits32_47"), unit(8, "g1.hi16"),
      unit(9, "sp"),      unit(10, "f0"),          unit(11, "f1"),
  };

  Description.PhysicalRegisters = {
      physicalRegister(TestRegister::G0, "g0", 0,
                       {unitLane(1, 0x1), unitLane(2, 0x2), unitLane(3, 0x4), unitLane(4, 0x8)}),
      physicalRegister(TestRegister::G0Low32, "g0l", 0, {unitLane(1, 0x1), unitLane(2, 0x2)}),
      physicalRegister(TestRegister::G0High32, "g0h", 0, {unitLane(3, 0x1), unitLane(4, 0x2)}),
      physicalRegister(TestRegister::G0Low16, "g0w", 0, {unitLane(1, 0x1)}),
      physicalRegister(TestRegister::G1, "g1", 1,
                       {unitLane(5, 0x1), unitLane(6, 0x2), unitLane(7, 0x4), unitLane(8, 0x8)}),
      physicalRegister(TestRegister::G1Low32, "g1l", 1, {unitLane(5, 0x1), unitLane(6, 0x2)}),
      physicalRegister(TestRegister::G1High32, "g1h", 1, {unitLane(7, 0x1), unitLane(8, 0x2)}),
      physicalRegister(TestRegister::G1Low16, "g1w", 1, {unitLane(5, 0x1)}),
      physicalRegister(TestRegister::StackPointer, "sp", 2, {unitLane(9, 0xF)}),
      physicalRegister(TestRegister::F0, "f0", 0, {unitLane(10, 0x1)}),
      physicalRegister(TestRegister::F1, "f1", 1, {unitLane(11, 0x1)}),
  };

  Description.SubRegIndices = {
      SubRegIndexDesc{TestSubReg::Low32, "low32", LaneMask::fromRaw(0x3), 32, 0},
      SubRegIndexDesc{TestSubReg::High32, "high32", LaneMask::fromRaw(0xC), 32, 32},
      SubRegIndexDesc{TestSubReg::Low16, "low16", LaneMask::fromRaw(0x1), 16, 0},
  };
  Description.PhysicalSubRegisters = {
      {phys(TestRegister::G0), TestSubReg::Low32, phys(TestRegister::G0Low32)},
      {phys(TestRegister::G0), TestSubReg::High32, phys(TestRegister::G0High32)},
      {phys(TestRegister::G0), TestSubReg::Low16, phys(TestRegister::G0Low16)},
      {phys(TestRegister::G0Low32), TestSubReg::Low16, phys(TestRegister::G0Low16)},
      {phys(TestRegister::G1), TestSubReg::Low32, phys(TestRegister::G1Low32)},
      {phys(TestRegister::G1), TestSubReg::High32, phys(TestRegister::G1High32)},
      {phys(TestRegister::G1), TestSubReg::Low16, phys(TestRegister::G1Low16)},
      {phys(TestRegister::G1Low32), TestSubReg::Low16, phys(TestRegister::G1Low16)},
  };
  Description.SubRegCompositions = {
      {TestSubReg::Low32, TestSubReg::Low16, TestSubReg::Low16},
  };

  const auto G0 = phys(TestRegister::G0);
  const auto G0Low32 = phys(TestRegister::G0Low32);
  const auto G0High32 = phys(TestRegister::G0High32);
  const auto G0Low16 = phys(TestRegister::G0Low16);
  const auto G1 = phys(TestRegister::G1);
  const auto G1Low32 = phys(TestRegister::G1Low32);
  const auto G1High32 = phys(TestRegister::G1High32);
  const auto G1Low16 = phys(TestRegister::G1Low16);
  const auto StackPointer = phys(TestRegister::StackPointer);
  const auto F0 = phys(TestRegister::F0);
  const auto F1 = phys(TestRegister::F1);

  Description.RegisterClasses = {
      registerClass(TestRegisterClass::GPR64, "GPR64", {G0, G1}, {G0, G1}, 64,
                    LaneMask::fromRaw(0xF), TestRegisterBank::GPR,
                    {TestSubReg::Low32, TestSubReg::High32, TestSubReg::Low16}, {}, true, true),
      registerClass(TestRegisterClass::GPR64Alloc, "GPR64Alloc", {G0, G1}, {G1, G0}, 64,
                    LaneMask::fromRaw(0xF), TestRegisterBank::GPR, {}, {TestRegisterClass::GPR64}),
      registerClass(TestRegisterClass::GPR32, "GPR32", {G0Low32, G0High32, G1Low32, G1High32},
                    {G0Low32, G1Low32, G0High32, G1High32}, 32, LaneMask::fromRaw(0x3),
                    TestRegisterBank::GPR),
      registerClass(TestRegisterClass::GPR32Low, "GPR32Low", {G0Low32, G1Low32}, {G0Low32, G1Low32},
                    32, LaneMask::fromRaw(0x3), TestRegisterBank::GPR, {TestSubReg::Low16},
                    {TestRegisterClass::GPR32}),
      registerClass(TestRegisterClass::GPR32High, "GPR32High", {G0High32, G1High32},
                    {G0High32, G1High32}, 32, LaneMask::fromRaw(0x3), TestRegisterBank::GPR, {},
                    {TestRegisterClass::GPR32}),
      registerClass(TestRegisterClass::GPR16, "GPR16", {G0Low16, G1Low16}, {G0Low16, G1Low16}, 16,
                    LaneMask::fromRaw(0x1), TestRegisterBank::GPR),
      registerClass(TestRegisterClass::FPR64, "FPR64", {F0, F1}, {F0, F1}, 64,
                    LaneMask::fromRaw(0x1), TestRegisterBank::FPR),
      registerClass(TestRegisterClass::SpecialGPR64, "SpecialGPR64", {StackPointer}, {}, 64,
                    LaneMask::fromRaw(0xF), TestRegisterBank::GPR, {}, {}, false),
  };

  Description.SubRegClassMappings = {
      {TestRegisterClass::GPR64, TestSubReg::Low32, TestRegisterClass::GPR32Low},
      {TestRegisterClass::GPR64, TestSubReg::High32, TestRegisterClass::GPR32High},
      {TestRegisterClass::GPR64, TestSubReg::Low16, TestRegisterClass::GPR16},
      {TestRegisterClass::GPR32Low, TestSubReg::Low16, TestRegisterClass::GPR16},
  };
  Description.RegisterBanks = {
      {TestRegisterBank::GPR,
       "GPR",
       64,
       {TestRegisterClass::GPR64, TestRegisterClass::GPR64Alloc, TestRegisterClass::GPR32,
        TestRegisterClass::GPR32Low, TestRegisterClass::GPR32High, TestRegisterClass::GPR16,
        TestRegisterClass::SpecialGPR64}},
      {TestRegisterBank::FPR, "FPR", 64, {TestRegisterClass::FPR64}},
  };

  return Description;
}

Expected<TargetRegisterInfo> createTestTargetRegisterInfo() {
  return TargetRegisterInfo::create(createTestTargetRegisterInfoDescription());
}

TargetMIRTextInfoDesc createTestTargetMIRTextInfoDescription() {
  TargetMIRTextInfoDesc Description;
  Description.RegisterMasks = {
      {"Test-Preserved", createTestCallPreservedMask()},
  };
  Description.TargetIndices = {
      {"test-index", TargetIndex::fromValue(0)},
  };
  Description.DirectOperandFlagMask = 0x3;
  Description.DirectOperandFlags = {
      {"test-direct-one", 0x1},
      {"test-direct-two", 0x2},
      {"test-direct-three", 0x3},
  };
  Description.BitmaskOperandFlags = {
      {"test-bit-four", 0x4},
      {"test-bit-eight", 0x8},
  };
  Description.MachineMemOperandFlags = {
      {"test-mmo-one", 0x1},
      {"test-mmo-two", 0x2},
  };
  Description.VirtualRegisterFlags = {
      {"test-vreg-one", 0x1},
      {"test-vreg-two", 0x2},
  };
  Description.Intrinsics = {
      {"llvm.test.seven", IntrinsicID::fromValue(7)},
      {"llvm.test.seventeen", IntrinsicID::fromValue(17)},
  };

  for (const PhysicalRegisterDesc &Register :
       createTestTargetRegisterInfoDescription().PhysicalRegisters) {
    Description.CFIRegisters.push_back({Register.Reg, Register.Reg.number(), true});
  }
  return Description;
}

Expected<TargetDescription> createTestTargetDescription(InstructionSet Instructions) {
  auto Registers = createTestTargetRegisterInfo();
  if (!Registers)
    return std::unexpected(Registers.error());
  return TargetDescription::create(std::move(Instructions), std::move(*Registers),
                                   createTestTargetMIRTextInfoDescription());
}

std::shared_ptr<const RegisterMask> createTestCallPreservedMask() {
  std::uint64_t Preserved = 0;
  Preserved |= std::uint64_t{1} << TestRegister::G1;
  Preserved |= std::uint64_t{1} << TestRegister::G1Low32;
  Preserved |= std::uint64_t{1} << TestRegister::G1High32;
  Preserved |= std::uint64_t{1} << TestRegister::G1Low16;
  Preserved |= std::uint64_t{1} << TestRegister::StackPointer;
  Preserved |= std::uint64_t{1} << TestRegister::F1;
  return std::make_shared<const RegisterMask>(std::vector<std::uint64_t>{Preserved});
}

} // namespace rcc::test
