// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HOME_RICHARD_CODE_RCC_PROJECT_BACKEND_UNITTESTS_TARGET_TESTTARGET_H
#define HOME_RICHARD_CODE_RCC_PROJECT_BACKEND_UNITTESTS_TARGET_TESTTARGET_H

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <memory>

namespace rcc::test {

struct TestRegister final {

  static constexpr std::uint32_t G0 = 1;
  static constexpr std::uint32_t G0Low32 = 2;
  static constexpr std::uint32_t G0High32 = 3;
  static constexpr std::uint32_t G0Low16 = 4;
  static constexpr std::uint32_t G1 = 5;
  static constexpr std::uint32_t G1Low32 = 6;
  static constexpr std::uint32_t G1High32 = 7;
  static constexpr std::uint32_t G1Low16 = 8;
  static constexpr std::uint32_t StackPointer = 9;
  static constexpr std::uint32_t F0 = 10;
  static constexpr std::uint32_t F1 = 11;
};

struct TestSubReg final {

  static constexpr SubRegIndex Low32 = SubRegIndex::fromValue(1);
  static constexpr SubRegIndex High32 = SubRegIndex::fromValue(2);
  static constexpr SubRegIndex Low16 = SubRegIndex::fromValue(3);
};

struct TestRegisterBank final {

  static constexpr RegisterBankID GPR = RegisterBankID::fromValue(1);
  static constexpr RegisterBankID FPR = RegisterBankID::fromValue(2);
};

struct TestRegisterClass final {

  static constexpr RegisterClassID GPR64 = RegisterClassID::fromValue(1);
  static constexpr RegisterClassID GPR64Alloc = RegisterClassID::fromValue(2);
  static constexpr RegisterClassID GPR32 = RegisterClassID::fromValue(3);
  static constexpr RegisterClassID GPR32Low = RegisterClassID::fromValue(4);
  static constexpr RegisterClassID GPR32High = RegisterClassID::fromValue(5);
  static constexpr RegisterClassID GPR16 = RegisterClassID::fromValue(6);
  static constexpr RegisterClassID FPR64 = RegisterClassID::fromValue(7);
  static constexpr RegisterClassID SpecialGPR64 = RegisterClassID::fromValue(8);
};

[[nodiscard]] TargetRegisterInfoDesc createTestTargetRegisterInfoDescription();
[[nodiscard]] Expected<TargetRegisterInfo> createTestTargetRegisterInfo();
[[nodiscard]] TargetMIRTextInfoDesc createTestTargetMIRTextInfoDescription();
[[nodiscard]] Expected<TargetDescription> createTestTargetDescription(InstructionSet Instructions);
[[nodiscard]] std::shared_ptr<const RegisterMask> createTestCallPreservedMask();

} // namespace rcc::test

#endif // HOME_RICHARD_CODE_RCC_PROJECT_BACKEND_UNITTESTS_TARGET_TESTTARGET_H
