// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_UNITTESTS_CODEGEN_GLOBALISEL_GISELMITEST_H
#define RCC_UNITTESTS_CODEGEN_GLOBALISEL_GISELMITEST_H

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rcc::test {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Entry = nullptr;
};

[[nodiscard]] inline FunctionFixture createFixture(TestContext &Context, std::string_view Name,
                                                   std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::string(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine pipeline function creation must succeed");
  if (!Function)
    return {};
  auto Entry = (*Function)->createBasicBlock();
  Context.expect(Entry.has_value(), "machine pipeline block creation must succeed");
  if (!Entry || !(*Function)->appendBlock(**Entry))
    return {};
  return {std::move(*Function), *Entry};
}

[[nodiscard]] inline rcc::Register
createRegister(TestContext &Context, rcc::MachineFunction &Function, std::uint32_t Bits) {
  auto Type = rcc::LLT::makeInteger(Bits);
  Context.expect(Type.has_value(), "integer LLT construction must succeed");
  if (!Type)
    return {};
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Reg.has_value(), "virtual register construction must succeed");
  return Reg ? *Reg : rcc::Register{};
}

[[nodiscard]] inline rcc::Register createFloatRegister(TestContext &Context,
                                                       rcc::MachineFunction &Function) {
  auto Type = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  Context.expect(Type.has_value(), "floating LLT construction must succeed");
  if (!Type)
    return {};
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  Context.expect(Reg.has_value(), "floating virtual register construction must succeed");
  return Reg ? *Reg : rcc::Register{};
}

[[nodiscard]] inline bool assignGPR(rcc::MachineFunction &Function, rcc::Register Reg) {
  const auto Bank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto RegClass =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  return Function.registerInfo().setRegisterBank(Reg, Bank).has_value() &&
         Function.registerInfo().setRegisterClass(Reg, RegClass).has_value();
}

[[nodiscard]] inline bool buildConstant(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                        rcc::Register Destination, std::int64_t Value) {
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_CONSTANT));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addImmediate(Value) ||
      !Pending->build()) {
    Context.expect(false, "generic constant construction must succeed");
    return false;
  }
  return true;
}

[[nodiscard]] inline bool buildAdd(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                   rcc::Register Destination, rcc::Register Left,
                                   rcc::Register Right) {
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_ADD));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Left) ||
      !Pending->addUse(Right) || !Pending->build()) {
    Context.expect(false, "generic add construction must succeed");
    return false;
  }
  return true;
}

} // namespace rcc::test

#endif // RCC_UNITTESTS_CODEGEN_GLOBALISEL_GISELMITEST_H
