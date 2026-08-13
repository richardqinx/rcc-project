// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_BACKEND_LIVE_TEST_SUPPORT_H
#define RCC_BACKEND_LIVE_TEST_SUPPORT_H

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::test::liveness {

constexpr std::uint32_t DefOpcode = 2048;
constexpr std::uint32_t UseOpcode = 2049;
constexpr std::uint32_t EarlyDefOpcode = 2050;
constexpr std::uint32_t MaskOpcode = 2051;
constexpr std::uint32_t MaskedDefOpcode = 2052;
constexpr std::uint32_t LiveOutMaskOpcode = 2053;

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

[[nodiscard]] inline rcc::Opcode testOpcode(std::uint32_t Value) {
  return rcc::Opcode::fromValue(Value);
}

[[nodiscard]] inline rcc::OperandDescriptor registerOperand(rcc::RegisterOperandRole Role,
                                                            bool EarlyClobber = false) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.EarlyClobber = EarlyClobber;
  Result.AllowSubReg = true;
  return Result;
}

[[nodiscard]] inline rcc::OperandDescriptor plainOperand(rcc::MachineOperand::Kind Kind) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {Kind};
  return Result;
}

[[nodiscard]] inline rcc::InstructionDescriptor instruction(std::uint32_t Value, std::string Name) {
  rcc::InstructionDescriptor Result;
  Result.InstructionOpcode = testOpcode(Value);
  Result.Name = std::move(Name);
  Result.Flags = {rcc::InstructionFlag::ProducesCode};
  return Result;
}

[[nodiscard]] inline rcc::Expected<rcc::TargetDescription> createTarget() {
  std::vector<rcc::InstructionDescriptor> Descriptors;
  std::vector<rcc::ImmediatePredicate> Predicates;
  auto Generic = rcc::appendGenericInstructionInfo(Descriptors, Predicates);
  if (!Generic)
    return std::unexpected(Generic.error());

  auto Def = instruction(DefOpcode, "LIVENESS_DEF");
  Def.Operands = {registerOperand(rcc::RegisterOperandRole::Def)};
  Def.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Def));

  auto Use = instruction(UseOpcode, "LIVENESS_USE");
  Use.Operands = {registerOperand(rcc::RegisterOperandRole::Use)};
  Descriptors.push_back(std::move(Use));

  auto EarlyDef = instruction(EarlyDefOpcode, "LIVENESS_EARLY_DEF");
  EarlyDef.Operands = {registerOperand(rcc::RegisterOperandRole::Def, true)};
  EarlyDef.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(EarlyDef));

  auto Mask = instruction(MaskOpcode, "LIVENESS_MASK");
  Mask.Operands = {plainOperand(rcc::MachineOperand::Kind::RegisterMask)};
  Mask.Flags.set(rcc::InstructionFlag::Call);
  Mask.Flags.set(rcc::InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(Mask));

  auto MaskedDef = instruction(MaskedDefOpcode, "LIVENESS_MASKED_DEF");
  MaskedDef.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                        plainOperand(rcc::MachineOperand::Kind::RegisterMask)};
  MaskedDef.NumExplicitDefs = 1;
  MaskedDef.Flags.set(rcc::InstructionFlag::Call);
  MaskedDef.Flags.set(rcc::InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(MaskedDef));

  auto LiveOutMask = instruction(LiveOutMaskOpcode, "LIVENESS_LIVEOUT_MASK");
  LiveOutMask.Operands = {plainOperand(rcc::MachineOperand::Kind::LiveOutRegisterMask)};
  Descriptors.push_back(std::move(LiveOutMask));

  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors), std::move(Predicates));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  std::vector<rcc::MachineBasicBlock *> Blocks;
};

[[nodiscard]] inline rcc::Expected<FunctionFixture>
createFunction(std::string Name, std::uint64_t ID, std::size_t BlockCount) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  if (!Function)
    return std::unexpected(Function.error());
  FunctionFixture Result{std::move(*Function), {}};
  Result.Blocks.reserve(BlockCount);
  for (std::size_t Index = 0; Index < BlockCount; ++Index) {
    auto Block = Result.Function->createBasicBlock();
    if (!Block)
      return std::unexpected(Block.error());
    auto Appended = Result.Function->appendBlock(**Block);
    if (!Appended)
      return std::unexpected(Appended.error());
    Result.Blocks.push_back(*Block);
  }
  return Result;
}

[[nodiscard]] inline rcc::Expected<rcc::Register> createVReg(rcc::MachineFunction &Function) {
  auto Type = rcc::LowLevelType::makeCategoryNeutralScalar(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  if (!Reg)
    return std::unexpected(Reg.error());
  auto Bank = Function.registerInfo().setRegisterBank(*Reg, rcc::test::TestRegisterBank::GPR);
  if (!Bank)
    return std::unexpected(Bank.error());
  auto Class = Function.registerInfo().setRegisterClass(*Reg, rcc::test::TestRegisterClass::GPR64);
  if (!Class)
    return std::unexpected(Class.error());
  return *Reg;
}

[[nodiscard]] inline rcc::Expected<rcc::MachineInstr *>
buildDef(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Reg, rcc::SubRegIndex SubReg = {},
         bool EarlyClobber = false) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(testOpcode(EarlyClobber ? EarlyDefOpcode : DefOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  rcc::RegisterOperandFlags Flags;
  if (EarlyClobber)
    Flags.set(rcc::RegisterOperandFlag::EarlyClobber);
  auto Added = Pending->addDef(Reg, Flags, SubReg);
  if (!Added)
    return std::unexpected(Added.error());
  return Pending->build();
}

[[nodiscard]] inline rcc::Expected<rcc::MachineInstr *>
buildUse(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Reg, rcc::SubRegIndex SubReg = {},
         rcc::MachineInstr *Before = nullptr) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point =
      Before == nullptr ? Builder.setInsertPoint(Block) : Builder.setInsertPointBefore(*Before);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(testOpcode(UseOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Added = Pending->addUse(Reg, {}, SubReg);
  if (!Added)
    return std::unexpected(Added.error());
  return Pending->build();
}

[[nodiscard]] inline rcc::Expected<rcc::MachineInstr *>
buildMask(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
          rcc::MachineBasicBlock &Block, std::shared_ptr<const rcc::RegisterMask> Mask) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(testOpcode(MaskOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Added = Pending->addRegisterMask(std::move(Mask));
  if (!Added)
    return std::unexpected(Added.error());
  return Pending->build();
}

[[nodiscard]] inline rcc::Expected<rcc::MachineInstr *>
buildMaskedDef(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
               rcc::MachineBasicBlock &Block, rcc::Register Reg,
               std::shared_ptr<const rcc::RegisterMask> Mask) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(testOpcode(MaskedDefOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Reg);
  if (!Def)
    return std::unexpected(Def.error());
  auto MaskResult = Pending->addRegisterMask(std::move(Mask));
  if (!MaskResult)
    return std::unexpected(MaskResult.error());
  return Pending->build();
}

[[nodiscard]] inline rcc::PhysicalRegister physical(std::uint32_t Number) {
  return *rcc::PhysicalRegister::create(Number);
}

[[nodiscard]] inline bool hasFlag(const rcc::MachineInstr &Instruction, std::size_t OperandIndex,
                                  rcc::RegisterOperandFlag Flag) {
  const rcc::MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  return Operand != nullptr && Operand->registerFlags().has(Flag);
}

inline void setRebuildProperties(rcc::MachineFunction &Function) {
  Function.properties().set(rcc::MachineFunctionProperty::Legalized);
  Function.properties().set(rcc::MachineFunctionProperty::RegBankSelected);
  Function.properties().set(rcc::MachineFunctionProperty::Selected);
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  Function.properties().set(rcc::MachineFunctionProperty::TiedOpsRewritten);
}

} // namespace rcc::test::liveness

#endif // RCC_BACKEND_LIVE_TEST_SUPPORT_H
