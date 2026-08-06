// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetRegisterInfo.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

[[nodiscard]] rcc::PhysicalRegister phys(std::uint32_t Number) {
  return *rcc::PhysicalRegister::create(Number);
}

[[nodiscard]] rcc::OperandDescriptor registerOperand(rcc::RegisterOperandRole Role,
                                                     rcc::RegisterClassID Class,
                                                     rcc::RegisterBankID Bank,
                                                     bool AllowSubReg = false) {
  rcc::OperandDescriptor Operand;
  Operand.Kinds = {rcc::MachineOperand::Kind::Register};
  Operand.RegisterRole = Role;
  Operand.RegClass = Class;
  Operand.RegBank = Bank;
  Operand.AllowSubReg = AllowSubReg;
  return Operand;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription>
createTarget(rcc::TargetRegisterInfoDesc RegisterDescription,
             rcc::InstructionDescriptor Instruction) {
  auto Registers = rcc::TargetRegisterInfo::create(std::move(RegisterDescription));
  if (!Registers)
    return std::unexpected(Registers.error());

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Instruction));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::TargetDescription::create(std::move(*Instructions), std::move(*Registers));
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription>
createOperandTarget(rcc::OperandDescriptor Operand,
                    rcc::TargetRegisterInfoDesc RegisterDescription =
                        rcc::test::createTestTargetRegisterInfoDescription()) {
  rcc::InstructionDescriptor Instruction;
  Instruction.InstructionOpcode = rcc::Opcode::fromValue(1);
  Instruction.Name = "TEST_CONSTRAINED";
  Instruction.Operands.push_back(Operand);
  if (Instruction.Operands.front().RegisterRole == rcc::RegisterOperandRole::Def) {
    Instruction.NumExplicitDefs = 1;
  }
  Instruction.Flags = {rcc::InstructionFlag::ProducesCode};

  return createTarget(std::move(RegisterDescription), std::move(Instruction));
}

void testRegisterTopology(TestContext &Context) {
  auto Target = rcc::test::createTestTargetRegisterInfo();
  Context.expect(Target.has_value(), "TestTarget register description must be valid");
  if (!Target)
    return;

  const auto G0 = phys(rcc::test::TestRegister::G0);
  const auto G0Low32 = phys(rcc::test::TestRegister::G0Low32);
  const auto G0High32 = phys(rcc::test::TestRegister::G0High32);
  const auto G0Low16 = phys(rcc::test::TestRegister::G0Low16);
  const auto G1 = phys(rcc::test::TestRegister::G1);
  const auto F0 = phys(rcc::test::TestRegister::F0);

  Context.expect(Target->registerUnits().size() == 11 && Target->physicalRegisters().size() == 11 &&
                     Target->registerClasses().size() == 8 && Target->registerBanks().size() == 2,
                 "TestTarget must expose its complete static register tables");
  const rcc::PhysicalRegisterDesc *Named = Target->physicalRegister("g0");
  Context.expect(Named != nullptr && Named->Reg == G0 && Named->Encoding == 0,
                 "physical register name lookup must preserve identity and encoding");

  Context.expect(Target->regsOverlap(G0, G0Low32) && Target->regsOverlap(G0, G0High32) &&
                     Target->regsOverlap(G0Low32, G0Low16) &&
                     !Target->regsOverlap(G0Low32, G0High32) && !Target->regsOverlap(G0, G1),
                 "physical overlap must be derived from shared register units");
  Context.expect(std::ranges::find(Target->aliases(G0), G0Low32) != Target->aliases(G0).end() &&
                     std::ranges::find(Target->aliases(G0Low32), G0High32) ==
                         Target->aliases(G0Low32).end(),
                 "alias queries must include only overlapping physical registers");

  Context.expect(Target->subRegister(G0, rcc::test::TestSubReg::Low32) == G0Low32 &&
                     Target->subRegister(G0Low32, rcc::test::TestSubReg::Low16) == G0Low16 &&
                     Target->isSubRegister(G0, G0Low16) &&
                     std::ranges::find(Target->superRegisters(G0Low16), G0) !=
                         Target->superRegisters(G0Low16).end() &&
                     Target->composeSubRegIndices(rcc::test::TestSubReg::Low32,
                                                  rcc::test::TestSubReg::Low16) ==
                         rcc::test::TestSubReg::Low16,
                 "subregister queries must preserve direct, transitive, and composed mappings");
  Context.expect(Target->isValidLaneMask(G0, rcc::LaneMask::fromRaw(0x3)) &&
                     Target->isValidLaneMask(G0, rcc::LaneMask::all()) &&
                     !Target->isValidLaneMask(G0, rcc::LaneMask::fromRaw(0x10)),
                 "lane masks must be checked against target register coverage");

  Context.expect(
      Target->isSubclass(rcc::test::TestRegisterClass::GPR64Alloc,
                         rcc::test::TestRegisterClass::GPR64) &&
          !Target->isSubclass(rcc::test::TestRegisterClass::GPR64,
                              rcc::test::TestRegisterClass::GPR64Alloc) &&
          Target->registerClassesOverlap(rcc::test::TestRegisterClass::GPR64,
                                         rcc::test::TestRegisterClass::GPR64Alloc) &&
          Target->subRegClass(rcc::test::TestRegisterClass::GPR64, rcc::test::TestSubReg::Low32) ==
              rcc::test::TestRegisterClass::GPR32Low,
      "register class queries must preserve hierarchy and subregister projection");
  Context.expect(Target->bankContainsRegister(rcc::test::TestRegisterBank::GPR, G0) &&
                     !Target->bankContainsRegister(rcc::test::TestRegisterBank::GPR, F0) &&
                     !Target->registerBanksOverlap(rcc::test::TestRegisterBank::GPR,
                                                   rcc::test::TestRegisterBank::FPR),
                 "register bank coverage must remain distinct from register classes");

  const auto CallMask = rcc::test::createTestCallPreservedMask();
  Context.expect(CallMask->preserves(G1) &&
                     CallMask->preserves(phys(rcc::test::TestRegister::StackPointer)) &&
                     CallMask->preserves(phys(rcc::test::TestRegister::F1)) &&
                     CallMask->clobbers(G0) && Target->validateRegisterMask(*CallMask).has_value(),
                 "call register mask must use preserved-bit semantics");
  Context.expect(Target->validateRegisterMask(rcc::RegisterMask({})).has_value(),
                 "an empty register mask must represent clobber-all");
  Context.expect(!Target->validateRegisterMask(rcc::RegisterMask({std::uint64_t{1} << 63U})) &&
                     !Target->validateRegisterMask(rcc::RegisterMask({1})),
                 "register masks must reject preserved bits for unknown registers and noreg");
}

void testDescriptionRejection(TestContext &Context) {
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterUnits.back().ID = Description.RegisterUnits.front().ID;
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "duplicate register unit IDs must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.PhysicalRegisters.front().AlternativeNames = {"later-name"};
    Description.PhysicalRegisters[1].Name = "later-name";
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "canonical register names must not collide with earlier ABI aliases");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.PhysicalRegisters.front().IsReserved = true;
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "statically reserved registers must not appear in allocation orders");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.PhysicalSubRegisters.front().SubReg = phys(rcc::test::TestRegister::G1Low32);
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "subregister mappings with unrelated units must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterClasses.front().SuperClasses.push_back(
        rcc::test::TestRegisterClass::GPR64Alloc);
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "cyclic register class hierarchies must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterBanks.front().MaximumSizeInBits = 32;
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "register banks smaller than a covered class must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.SubRegClassMappings.pop_back();
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "supported subregisters without class projections must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterClasses.front().Lanes = rcc::LaneMask::fromRaw(0x10);
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "register class lanes outside a member register must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterBanks.front().RegisterClasses.erase(
        Description.RegisterBanks.front().RegisterClasses.begin());
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "a class default bank must explicitly cover that class");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterUnits.push_back({rcc::RegisterUnitID::fromValue(99), "unused"});
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "unreferenced register units must be rejected");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.SubRegCompositions.push_back(
        {rcc::test::TestSubReg::Low16, rcc::test::TestSubReg::Low16, rcc::test::TestSubReg::Low32});
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "subregister composition bit ranges must be internally consistent");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterClasses[3].CoveredBySubRegs = true;
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "subregister-covered classes must cover all class lanes");
  }
}

void testTargetDescriptionConstraints(TestContext &Context) {
  auto SubclassTarget = createOperandTarget(registerOperand(rcc::RegisterOperandRole::Def,
                                                            rcc::test::TestRegisterClass::GPR64,
                                                            rcc::test::TestRegisterBank::GPR));
  auto Function =
      rcc::MachineFunction::create("constraints", rcc::MachineFunctionStableID::fromValue(1));
  auto Type = rcc::LLT::makeInteger(64);
  if (!SubclassTarget || !Function || !Type)
    return;

  auto Reg = (*Function)->registerInfo().createVirtualRegister(*Type);
  if (!Reg)
    return;
  Context.expect((*Function)
                         ->registerInfo()
                         .setRegisterBank(*Reg, rcc::test::TestRegisterBank::GPR)
                         .has_value() &&
                     (*Function)
                         ->registerInfo()
                         .setRegisterClass(*Reg, rcc::test::TestRegisterClass::GPR64Alloc)
                         .has_value(),
                 "constrained test virtual register must be classified");

  auto Operand = rcc::MachineOperand::registerOperand(*Reg, {rcc::RegisterOperandFlag::Define});
  Context.expect(
      Operand && SubclassTarget
                     ->validateExplicitOperands(rcc::Opcode::fromValue(1),
                                                std::span<const rcc::MachineOperand>(&*Operand, 1),
                                                (*Function)->registerInfo())
                     .has_value(),
      "an actual register subclass must satisfy a superclass operand constraint");

  auto SubRegTarget = createOperandTarget(registerOperand(rcc::RegisterOperandRole::Def,
                                                          rcc::test::TestRegisterClass::GPR32,
                                                          rcc::test::TestRegisterBank::GPR, true));
  auto BaseClassResult =
      (*Function)->registerInfo().setRegisterClass(*Reg, rcc::test::TestRegisterClass::GPR64);
  auto SubRegOperand = rcc::MachineOperand::registerOperand(
      *Reg, {rcc::RegisterOperandFlag::Define}, rcc::test::TestSubReg::Low32);
  Context.expect(
      SubRegTarget && BaseClassResult && SubRegOperand &&
          SubRegTarget
              ->validateExplicitOperands(rcc::Opcode::fromValue(1),
                                         std::span<const rcc::MachineOperand>(&*SubRegOperand, 1),
                                         (*Function)->registerInfo())
              .has_value(),
      "subregister class projection must satisfy a compatible superclass constraint");

  auto PhysicalSubReg = rcc::MachineOperand::registerOperand(
      rcc::Register::fromPhysical(phys(rcc::test::TestRegister::G0)),
      {rcc::RegisterOperandFlag::Define}, rcc::test::TestSubReg::Low32);
  Context.expect(PhysicalSubReg && !SubRegTarget->validateExplicitOperands(
                                       rcc::Opcode::fromValue(1),
                                       std::span<const rcc::MachineOperand>(&*PhysicalSubReg, 1),
                                       (*Function)->registerInfo()),
                 "physical register operands must reject nonzero subregister indices");

  auto WideType = rcc::LLT::makeInteger(128);
  if (!WideType)
    return;
  auto WideReg = (*Function)->registerInfo().createVirtualRegister(*WideType);
  Context.expect(WideReg &&
                     (*Function)
                         ->registerInfo()
                         .setRegisterBank(*WideReg, rcc::test::TestRegisterBank::GPR)
                         .has_value() &&
                     !SubclassTarget->validateRegisterInfo((*Function)->registerInfo()),
                 "virtual register type must fit its selected bank");

  auto UnknownClassOperand =
      registerOperand(rcc::RegisterOperandRole::Def, rcc::RegisterClassID::fromValue(99),
                      rcc::test::TestRegisterBank::GPR);
  Context.expect(!createOperandTarget(UnknownClassOperand),
                 "target creation must reject unknown operand register classes");
  auto MismatchedBankOperand =
      registerOperand(rcc::RegisterOperandRole::Def, rcc::test::TestRegisterClass::GPR64,
                      rcc::test::TestRegisterBank::FPR);
  Context.expect(!createOperandTarget(MismatchedBankOperand),
                 "target creation must reject incompatible class and bank constraints");

  auto ConstantDescription = rcc::test::createTestTargetRegisterInfoDescription();
  ConstantDescription.PhysicalRegisters.front().IsConstant = true;
  auto ConstantTarget = createOperandTarget(registerOperand(rcc::RegisterOperandRole::Def,
                                                            rcc::test::TestRegisterClass::GPR64,
                                                            rcc::test::TestRegisterBank::GPR),
                                            std::move(ConstantDescription));
  auto HintFunction =
      rcc::MachineFunction::create("constant_hint", rcc::MachineFunctionStableID::fromValue(2));
  if (ConstantTarget && HintFunction) {
    auto HintReg = (*HintFunction)->registerInfo().createVirtualRegister(*Type);
    Context.expect(HintReg &&
                       (*HintFunction)
                           ->registerInfo()
                           .setRegisterClass(*HintReg, rcc::test::TestRegisterClass::GPR64)
                           .has_value() &&
                       (*HintFunction)
                           ->registerInfo()
                           .addAllocationHint(*HintReg, phys(rcc::test::TestRegister::G0))
                           .has_value() &&
                       !ConstantTarget->validateRegisterInfo((*HintFunction)->registerInfo()),
                   "constant physical registers must be rejected as allocation hints");
  }

  auto RenamableDescription = rcc::test::createTestTargetRegisterInfoDescription();
  RenamableDescription.PhysicalRegisters.front().IsArtificial = true;
  rcc::InstructionDescriptor RenamableImplicit;
  RenamableImplicit.InstructionOpcode = rcc::Opcode::fromValue(2);
  RenamableImplicit.Name = "TEST_BAD_IMPLICIT";
  RenamableImplicit.ImplicitDefs.push_back({phys(rcc::test::TestRegister::G0), false, true});
  Context.expect(!createTarget(std::move(RenamableDescription), std::move(RenamableImplicit)),
                 "target creation must reject renamable artificial implicit registers");

  rcc::InstructionDescriptor ConflictingImplicit;
  ConflictingImplicit.InstructionOpcode = rcc::Opcode::fromValue(3);
  ConflictingImplicit.Name = "TEST_BAD_EARLY_IMPLICIT";
  ConflictingImplicit.ImplicitDefs.push_back({phys(rcc::test::TestRegister::G0), true, false});
  ConflictingImplicit.ImplicitUses.push_back(
      {phys(rcc::test::TestRegister::G0Low32), false, false});
  Context.expect(!createTarget(rcc::test::createTestTargetRegisterInfoDescription(),
                               std::move(ConflictingImplicit)),
                 "target creation must reject overlapping early-clobber implicit operands");
}

} // namespace

int main() {
  TestContext Context;
  testRegisterTopology(Context);
  testDescriptionRejection(Context);
  testTargetDescriptionConstraints(Context);
  return Context.result();
}
