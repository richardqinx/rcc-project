// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCVRegisterBankInfo.h"

#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] bool isRVFloatType(LLT Type) noexcept {
  return Type.isFloat() && (Type.scalarBitWidth() == 32 || Type.scalarBitWidth() == 64);
}

[[nodiscard]] bool isScalarGPRType(LLT Type) noexcept {
  return (Type.isPointer() && Type.addressSpace() == 0 && Type.scalarBitWidth() == 64) ||
         ((Type.isInteger() || Type.isAnyScalar()) && Type.scalarBitWidth() >= 1 &&
          Type.scalarBitWidth() <= 64);
}

[[nodiscard]] Expected<RegisterClassID> requiredRegisterClass(const MachineFunction &Function,
                                                              Register Reg,
                                                              const TargetDescription &Target) {
  if (!Reg.isVirtual())
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "RISC-V register-class selection requires a virtual register");
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  if (!Info->get().Type) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V selection requires a type for every virtual register");
  }
  const VirtualRegisterInfo &RegisterInfo = Info->get();
  const LLT Type = RegisterInfo.Type.value_or(LLT{});
  RegisterClassID RequiredClass;
  if (isScalarGPRType(Type)) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::GPR));
  } else if (Type.isFloat() && Type.scalarBitWidth() == 32) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::FPR32));
  } else if (Type.isFloat() && Type.scalarBitWidth() == 64) {
    RequiredClass = RegisterClassID::fromValue(static_cast<std::uint32_t>(RegisterClass::FPR64));
  } else {
    return makeUnexpected(ErrorCode::Unsupported,
                          "RISC-V RV64GC has no register class for type '" + Type.toString() + "'");
  }

  if (Target.registers().registerClass(RequiredClass) == nullptr) {
    return makeUnexpected(ErrorCode::InternalError,
                          "RISC-V target description is missing a required register class");
  }
  if (RegisterInfo.Bank && !Target.registers().bankCoversClass(*RegisterInfo.Bank, RequiredClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register bank cannot cover its required class");
  }
  if (RegisterInfo.RegClass &&
      !Target.registers().isSubclass(*RegisterInfo.RegClass, RequiredClass)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V virtual register has a conflicting register class");
  }
  return RequiredClass;
}

} // namespace

Expected<RegisterBankID> RISCVRegisterBankInfo::selectBank(const MachineFunction &, Register,
                                                           const VirtualRegisterInfo &Info,
                                                           const MachineInstr *,
                                                           const TargetDescription &) const {
  if (!Info.Type) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RISC-V register-bank selection requires a virtual-register type");
  }
  if (isScalarGPRType(*Info.Type))
    return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::GPR));
  if (isRVFloatType(*Info.Type))
    return RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::FPR));
  return makeUnexpected(ErrorCode::Unsupported,
                        "RISC-V RV64GC has no register bank for virtual register type '" +
                            Info.Type->toString() + "'");
}

Expected<void> RISCVRegisterBankInfo::constrainInstruction(MachineInstr &Instruction,
                                                           MachineFunction &Function,
                                                           const TargetDescription &Target) const {
  std::vector<std::pair<Register, RegisterClassID>> PlannedClasses;
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      continue;
    auto Required = requiredRegisterClass(Function, *Reg, Target);
    if (!Required)
      return std::unexpected(Required.error());
    if (std::ranges::find_if(PlannedClasses, [Reg](const auto &Entry) {
          return Entry.first == *Reg;
        }) == PlannedClasses.end())
      PlannedClasses.emplace_back(*Reg, *Required);
  }
  for (const auto &[Reg, Required] : PlannedClasses) {
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    if (Info->get().RegClass)
      continue;
    auto SetClass = Function.registerInfo().setRegisterClass(Reg, Required);
    if (!SetClass)
      return std::unexpected(SetClass.error());
  }
  return {};
}

Expected<void> RISCVRegisterBankInfo::constrainRegister(MachineFunction &Function, Register Reg,
                                                        const TargetDescription &Target) const {
  auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
  if (!Info)
    return std::unexpected(Info.error());
  auto Bank = selectBank(Function, Reg, Info->get(), nullptr, Target);
  if (!Bank)
    return std::unexpected(Bank.error());
  auto RequiredClass = requiredRegisterClass(Function, Reg, Target);
  if (!RequiredClass)
    return std::unexpected(RequiredClass.error());
  if (!Info->get().Bank) {
    auto SetBank = Function.registerInfo().setRegisterBank(Reg, *Bank);
    if (!SetBank)
      return std::unexpected(SetBank.error());
  }
  if (!Info->get().RegClass) {
    auto SetClass = Function.registerInfo().setRegisterClass(Reg, *RequiredClass);
    if (!SetClass)
      return std::unexpected(SetClass.error());
  }
  return {};
}

Expected<void> RISCVRegisterBankInfo::validateInstruction(const MachineInstr &Instruction,
                                                          const MachineFunction &Function,
                                                          const TargetDescription &Target) const {
  for (const MachineOperand &Operand : Instruction.operands()) {
    const Register *Reg = Operand.getIf<Register>();
    if (Reg == nullptr || !Reg->isVirtual())
      continue;
    auto Required = requiredRegisterClass(Function, *Reg, Target);
    if (!Required)
      return std::unexpected(Required.error());
  }
  return {};
}

} // namespace rcc::riscv
