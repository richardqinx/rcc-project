// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetMIRTextInfo.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
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

void testTargetRegisterNames(TestContext &Context) {
  auto Registers = rcc::test::createTestTargetRegisterInfo();
  Context.expect(Registers.has_value(), "TestTarget register schema must be valid");
  if (!Registers)
    return;

  const auto G0 = phys(rcc::test::TestRegister::G0);
  Context.expect(Registers->physicalRegister("g0") != nullptr &&
                     Registers->physicalRegister("G0") == nullptr &&
                     Registers->physicalRegisterMIRName(G0) == "g0",
                 "physical register MIR names must be canonical lowercase names");
  Context.expect(Registers->subRegIndex("low32") != nullptr &&
                     Registers->subRegIndex("LOW32") == nullptr,
                 "subregister MIR names must retain their exact target spelling");
  Context.expect(Registers->registerClass("gpr64") != nullptr &&
                     Registers->registerClass("GPR64") == nullptr &&
                     Registers->registerClassMIRName(rcc::test::TestRegisterClass::GPR64) ==
                         "gpr64",
                 "register class MIR names must be canonical lowercase names");
  Context.expect(Registers->registerBank("gpr") != nullptr &&
                     Registers->registerBank("GPR") == nullptr &&
                     Registers->registerBankMIRName(rcc::test::TestRegisterBank::GPR) == "gpr",
                 "register bank MIR names must be canonical lowercase names");
}

void testTextSchemaQueries(TestContext &Context) {
  auto Registers = rcc::test::createTestTargetRegisterInfo();
  if (!Registers)
    return;
  auto Text = rcc::TargetMIRTextInfo::create(rcc::test::createTestTargetMIRTextInfoDescription(),
                                             *Registers);
  Context.expect(Text.has_value(), "TestTarget MIR text schema must be valid");
  if (!Text)
    return;

  const auto Preserved = rcc::test::createTestCallPreservedMask();
  const auto NamedMask = Text->registerMask("test-preserved");
  Context.expect(NamedMask && *NamedMask == *Preserved && !Text->registerMask("Test-Preserved") &&
                     Text->registerMaskName(*Preserved) == "test-preserved",
                 "named register masks must use one canonical lowercase spelling");

  Context.expect(Text->targetIndex("test-index") == rcc::TargetIndex::fromValue(0) &&
                     Text->targetIndexName(rcc::TargetIndex::fromValue(0)) == "test-index",
                 "target index zero must remain a valid named target index");
  Context.expect(Text->directOperandFlag("test-direct-three") == 0x3 &&
                     Text->bitmaskOperandFlag("test-bit-four") == 0x4,
                 "direct and bitmask target flags must retain distinct registries");

  auto OperandNames = Text->operandFlagNames(0xD);
  Context.expect(OperandNames &&
                     *OperandNames == std::vector<std::string_view>(
                                          {"test-direct-one", "test-bit-four", "test-bit-eight"}),
                 "operand flag decomposition must be canonical and declaration ordered");
  Context.expect(!Text->operandFlagNames(0x10),
                 "operand flag decomposition must reject unknown bits");

  auto MMONames = Text->machineMemOperandFlagNames(0x3);
  auto VRegNames = Text->virtualRegisterFlagNames(0x3);
  Context.expect(
      MMONames && *MMONames == std::vector<std::string_view>({"test-mmo-one", "test-mmo-two"}) &&
          VRegNames &&
          *VRegNames == std::vector<std::string_view>({"test-vreg-one", "test-vreg-two"}) &&
          !Text->machineMemOperandFlagNames(0x4) && !Text->virtualRegisterFlagNames(0x4),
      "MMO and virtual-register flags must reject every unregistered bit");

  Context.expect(
      Text->intrinsic("llvm.test.seven") == rcc::IntrinsicID::fromValue(7) &&
          Text->intrinsicName(rcc::IntrinsicID::fromValue(17)) == "llvm.test.seventeen" &&
          Text->intrinsic("llvm.va_copy") == rcc::intrinsicID(rcc::GenericIntrinsic::VACopy) &&
          Text->intrinsicName(rcc::intrinsicID(rcc::GenericIntrinsic::VACopy)) == "llvm.va_copy",
      "intrinsic names and strong IDs must round-trip");

  const auto G0 = phys(rcc::test::TestRegister::G0);
  Context.expect(Text->dwarfRegister(G0) == rcc::test::TestRegister::G0 &&
                     Text->physicalRegister(rcc::test::TestRegister::G0) == G0,
                 "CFI register mappings must support both physical and DWARF lookup");
  Context.expect(Text->supportsStackID(rcc::StackID::defaultStack()) &&
                     Text->supportsStackID(rcc::StackID::noAllocStack()) &&
                     !Text->supportsStackID(rcc::StackID::sgprSpillStack()),
                 "target stack support must be explicit and include fixed baseline defaults");
}

void testTextSchemaRejection(TestContext &Context) {
  auto Registers = rcc::test::createTestTargetRegisterInfo();
  if (!Registers)
    return;
  auto Reject = [&Context, &Registers](rcc::TargetMIRTextInfoDesc Description,
                                       std::string_view Message) {
    Context.expect(!rcc::TargetMIRTextInfo::create(std::move(Description), *Registers), Message);
  };

  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.RegisterMasks.push_back(
        {"TEST-PRESERVED",
         std::make_shared<const rcc::RegisterMask>(std::vector<std::uint64_t>{})});
    Reject(std::move(Description), "register mask names must be unique case-insensitively");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.RegisterMasks.push_back({"other-mask", rcc::test::createTestCallPreservedMask()});
    Reject(std::move(Description), "register mask values must have one canonical name");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.RegisterMasks.front().Mask = std::make_shared<const rcc::RegisterMask>(
        std::vector<std::uint64_t>{std::uint64_t{1} << 63U});
    Reject(std::move(Description), "named masks must reject unknown physical registers");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.TargetIndices.push_back({"other-index", rcc::TargetIndex::fromValue(0)});
    Reject(std::move(Description), "target index values must be unique");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.DirectOperandFlagMask = 0;
    Reject(std::move(Description), "direct flags require a direct flag mask");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.DirectOperandFlags.front().Value = 0x4;
    Reject(std::move(Description), "direct flags must remain inside their direct field");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.BitmaskOperandFlags.front().Value = 0xC;
    Reject(std::move(Description), "bitmask operand flags must contain one bit");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.MachineMemOperandFlags.front().Value = 0x3;
    Reject(std::move(Description), "MMO target flags must contain one bit");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.VirtualRegisterFlags.back().Value = Description.VirtualRegisterFlags.front().Value;
    Reject(std::move(Description), "virtual-register target flag values must be unique");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.Intrinsics.front().Name = "test.not-an-intrinsic";
    Reject(std::move(Description), "intrinsic names must use the llvm namespace");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.CFIRegisters[1].DwarfRegister = Description.CFIRegisters[0].DwarfRegister;
    Reject(std::move(Description),
           "a DWARF register must not have multiple canonical physical registers");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.CFIRegisters.front().IsCanonical = false;
    Reject(std::move(Description),
           "every referenced DWARF register must have a canonical physical register");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.CFIRegisters.front().Register = phys(99);
    Reject(std::move(Description), "CFI mappings must reject unknown physical registers");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.FunctionStateType = rcc::TargetFunctionStateTypeID{};
    Reject(std::move(Description), "target function state schemas require a nonzero TypeID");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.SupportedStackIDs.pop_back();
    Reject(std::move(Description), "targets must support the noalloc stack ID");
  }
  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.SupportedStackIDs.push_back(rcc::StackID::defaultStack());
    Reject(std::move(Description), "supported stack IDs must be unique");
  }

  {
    auto Description = rcc::test::createTestTargetMIRTextInfoDescription();
    Description.CFIRegisters[1].DwarfRegister = Description.CFIRegisters[0].DwarfRegister;
    Description.CFIRegisters[1].IsCanonical = false;
    auto Text = rcc::TargetMIRTextInfo::create(std::move(Description), *Registers);
    Context.expect(Text &&
                       Text->dwarfRegister(phys(rcc::test::TestRegister::G0Low32)) ==
                           rcc::test::TestRegister::G0 &&
                       Text->physicalRegister(rcc::test::TestRegister::G0) ==
                           phys(rcc::test::TestRegister::G0),
                   "noncanonical physical aliases must map to one canonical DWARF register");
  }

  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.PhysicalRegisters[1].Name = "G0";
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "physical register names must be case-insensitively unique");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterClasses[1].Name = "gpr64";
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "register class names must be case-insensitively unique");
  }
  {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    Description.RegisterBanks[1].Name = "gpr";
    Context.expect(!rcc::TargetRegisterInfo::create(std::move(Description)),
                   "register bank names must be case-insensitively unique");
  }
}

void testSemanticValidation(TestContext &Context) {
  rcc::OperandDescriptor Operand;
  Operand.Kinds = {rcc::MachineOperand::Kind::Immediate};
  Operand.AllowedTargetFlags = 0xF;

  rcc::InstructionDescriptor Instruction;
  Instruction.InstructionOpcode = rcc::Opcode::fromValue(1);
  Instruction.Name = "TEST_LOAD_IMM";
  Instruction.Operands.push_back(Operand);
  Instruction.Flags = {
      rcc::InstructionFlag::ProducesCode,
      rcc::InstructionFlag::MayLoad,
  };
  Instruction.RequiresMemoryOperands = true;

  auto Instructions = rcc::InstructionSet::create({std::move(Instruction)});
  if (!Instructions)
    return;
  auto Target = rcc::test::createTestTargetDescription(std::move(*Instructions));
  auto Function =
      rcc::MachineFunction::create("schema_validation", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Target.has_value() && Function.has_value(),
                 "semantic target schema fixture must be constructible");
  if (!Target || !Function)
    return;

  auto Reg = (*Function)->registerInfo().createVirtualRegister();
  Context.expect(Reg && (*Function)->registerInfo().setTargetRegisterFlags(*Reg, 0x3).has_value() &&
                     Target->validateRegisterInfo((*Function)->registerInfo()).has_value(),
                 "registered virtual-register target flags must pass target validation");
  if (Reg) {
    Context.expect((*Function)->registerInfo().setTargetRegisterFlags(*Reg, 0x4).has_value() &&
                       !Target->validateRegisterInfo((*Function)->registerInfo()),
                   "unregistered virtual-register target flags must fail target validation");
    Context.expect(
        (*Function)->registerInfo().clearTargetRegisterFlags(*Reg).has_value(),
        "semantic validation fixture must restore registered virtual-register target flags");
  }

  const rcc::MachineOperand ValidOperand = rcc::MachineOperand::immediate(7, 0x5);
  const rcc::MachineOperand InvalidOperand = rcc::MachineOperand::immediate(7, 0x10);
  Context.expect(
      Target->validateExplicitOperands(rcc::Opcode::fromValue(1),
                                       std::span<const rcc::MachineOperand>(&ValidOperand, 1),
                                       (*Function)->registerInfo())
              .has_value() &&
          !Target->validateExplicitOperands(
              rcc::Opcode::fromValue(1), std::span<const rcc::MachineOperand>(&InvalidOperand, 1),
              (*Function)->registerInfo()),
      "machine operands must use only descriptor-allowed serializable target flags");

  rcc::MachineMemOperandDesc MemoryDescription;
  MemoryDescription.Accesses = {rcc::MemoryAccessFlag::Load};
  MemoryDescription.Size = *rcc::MemorySize::fixed(8);
  MemoryDescription.Align = *rcc::Alignment::create(8);
  MemoryDescription.TargetFlags = 0x3;
  auto ValidMemory = rcc::MachineMemOperand::create(MemoryDescription);
  MemoryDescription.TargetFlags = 0x4;
  auto InvalidMemory = rcc::MachineMemOperand::create(MemoryDescription);
  if (ValidMemory && InvalidMemory) {
    const std::array<const rcc::MachineMemOperand *, 1> ValidOperands{&*ValidMemory};
    const std::array<const rcc::MachineMemOperand *, 1> InvalidOperands{&*InvalidMemory};
    Context.expect(
        Target->validateMemOperands(rcc::Opcode::fromValue(1), ValidOperands).has_value() &&
            !Target->validateMemOperands(rcc::Opcode::fromValue(1), InvalidOperands),
        "machine memory operands must use only registered target flags");
  }

  rcc::MachineCFIInfo CFI;
  auto CFIIndex = CFI.addInstruction(
      rcc::MachineCFIInstruction::sameValue(rcc::DwarfRegisterNumber::fromValue(99)));
  Context.expect(CFIIndex && !Target->validateCFIInfo(CFI),
                 "CFI instructions must reject unmapped DWARF registers");

  rcc::MachineFrameInfo Frame;
  rcc::StackObjectDesc StackObject;
  StackObject.Kind = rcc::StackObjectKind::Target;
  StackObject.Size = 8;
  StackObject.Align = *rcc::Alignment::create(8);
  StackObject.Stack = rcc::StackID::sgprSpillStack();
  auto StackIndex = Frame.createObject(StackObject);
  Context.expect(StackIndex && !Target->validateFrameInfo(Frame),
                 "frame objects must reject stack IDs unsupported by the target");

  rcc::OperandDescriptor BadOperand;
  BadOperand.Kinds = {rcc::MachineOperand::Kind::Immediate};
  BadOperand.AllowedTargetFlags = 0x10;
  rcc::InstructionDescriptor BadInstruction;
  BadInstruction.InstructionOpcode = rcc::Opcode::fromValue(2);
  BadInstruction.Name = "TEST_BAD_FLAGS";
  BadInstruction.Operands.push_back(BadOperand);
  BadInstruction.Flags = {rcc::InstructionFlag::ProducesCode};
  auto BadInstructions = rcc::InstructionSet::create({std::move(BadInstruction)});
  Context.expect(BadInstructions &&
                     !rcc::test::createTestTargetDescription(std::move(*BadInstructions)),
                 "target construction must reject operand flag bits outside its text schema");

  auto PartialDescription = rcc::test::createTestTargetMIRTextInfoDescription();
  PartialDescription.DirectOperandFlags.erase(PartialDescription.DirectOperandFlags.begin() + 1,
                                              PartialDescription.DirectOperandFlags.end());
  auto Registers = rcc::test::createTestTargetRegisterInfo();
  auto PartialText =
      Registers ? rcc::TargetMIRTextInfo::create(std::move(PartialDescription), *Registers)
                : rcc::Expected<rcc::TargetMIRTextInfo>(std::unexpected(Registers.error()));
  Context.expect(PartialText && !PartialText->validateOperandFlags(0x2),
                 "unused direct-field encodings must remain invalid");
}

} // namespace

int main() {
  TestContext Context;
  testTargetRegisterNames(Context);
  testTextSchemaQueries(Context);
  testTextSchemaRejection(Context);
  testSemanticValidation(Context);
  return Context.result();
}
