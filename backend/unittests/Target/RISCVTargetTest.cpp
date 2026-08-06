// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstdint>
#include <iostream>
#include <ranges>
#include <string_view>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

[[nodiscard]] rcc::PhysicalRegister registerID(rcc::riscv::PhysicalRegisterID ID) {
  const auto Register = rcc::riscv::physicalRegister(ID);
  if (!Register)
    std::terminate();
  return *Register;
}

void testRegisterFacts(TestContext &Context) {
  auto Registers = rcc::riscv::createRegisterInfo();
  Context.expect(Registers.has_value(), "RV64GC register description must be valid");
  if (!Registers)
    return;

  const rcc::PhysicalRegister X0 = registerID(rcc::riscv::PhysicalRegisterID::X0);
  const rcc::PhysicalRegister X2 = registerID(rcc::riscv::PhysicalRegisterID::X2);
  const rcc::PhysicalRegister X5 = registerID(rcc::riscv::PhysicalRegisterID::X5);
  const rcc::PhysicalRegister X8 = registerID(rcc::riscv::PhysicalRegisterID::X8);
  const rcc::PhysicalRegister F31 = registerID(rcc::riscv::PhysicalRegisterID::F31);

  Context.expect(
      Registers->physicalRegisters().size() == 64 && Registers->registerUnits().size() == 64 &&
          Registers->registerClasses().size() == 6 && Registers->registerBanks().size() == 2,
      "RV64GC must expose the complete scalar integer and floating register files");
  Context.expect(Registers->physicalRegister("x0") != nullptr &&
                     Registers->physicalRegister("zero") == nullptr &&
                     Registers->physicalRegisterByAlternativeName("ZERO") != nullptr &&
                     Registers->physicalRegisterByAlternativeName("zero")->Reg == X0 &&
                     Registers->physicalRegisterByAlternativeName("Sp") != nullptr &&
                     Registers->physicalRegisterByAlternativeName("sp")->Reg == X2 &&
                     Registers->physicalRegisterMIRName(X8) == "x8",
                 "canonical MIR names and case-insensitive ABI aliases must use separate lookups");
  Context.expect(
      Registers->isReservedRegister(X0) && Registers->isReservedRegister(X2) &&
          !Registers->isReservedRegister(X5),
      "zero, stack, global, and thread register constraints must be recorded statically");
  Context.expect(Registers->physicalRegister(F31) != nullptr &&
                     Registers->physicalRegister(F31)->Encoding == 31,
                 "floating register physical IDs must remain distinct from their ISA encodings");

  const auto *GPR = Registers->registerClass("gpr");
  Context.expect(
      GPR != nullptr && std::ranges::find(GPR->AllocationOrder, X0) == GPR->AllocationOrder.end() &&
          std::ranges::find(GPR->AllocationOrder, X2) == GPR->AllocationOrder.end() &&
          std::ranges::find(GPR->AllocationOrder, X5) != GPR->AllocationOrder.end(),
      "the general-register allocation order must exclude statically reserved registers");
}

void testABIAndDebugFacts(TestContext &Context) {
  const rcc::PhysicalRegister X1 = registerID(rcc::riscv::PhysicalRegisterID::X1);
  const rcc::PhysicalRegister X10 = registerID(rcc::riscv::PhysicalRegisterID::X10);
  const rcc::PhysicalRegister F8 = registerID(rcc::riscv::PhysicalRegisterID::F8);
  const rcc::PhysicalRegister F10 = registerID(rcc::riscv::PhysicalRegisterID::F10);
  const rcc::PhysicalRegister F31 = registerID(rcc::riscv::PhysicalRegisterID::F31);

  const auto Mask = rcc::riscv::createCallPreservedMask();
  Context.expect(
      Mask != nullptr && Mask->preserves(X1) && Mask->preserves(F8) && Mask->clobbers(X10) &&
          Mask->clobbers(F10),
      "LP64D preserved-mask facts must distinguish saved and caller-clobbered registers");

  auto MIRText = rcc::riscv::createMIRTextInfo();
  Context.expect(MIRText.has_value(), "RV64GC MIR text facts must be valid");
  if (!MIRText)
    return;
  Context.expect(MIRText->registerMasks().size() == 1 &&
                     MIRText->registerMasks().front().Name == "csr_ilp32d_lp64d",
                 "RV64GC must expose its named LP64D call-preserved mask");
  Context.expect(MIRText->dwarfRegister(X1) == 1,
                 "the return-address register must retain its DWARF register number");
  Context.expect(MIRText->physicalRegister(63) == F31,
                 "the final floating register DWARF number must map to f31");
}

} // namespace

int main() {
  TestContext Context;
  testRegisterFacts(Context);
  testABIAndDebugFacts(Context);
  return Context.result();
}
