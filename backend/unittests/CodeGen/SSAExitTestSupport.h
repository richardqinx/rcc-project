// SPDX-License-Identifier: GPL-3.0-or-later

// This private test header has no stable include-root-relative guard name.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_BACKEND_SSA_EXIT_TEST_SUPPORT_H
#define RCC_BACKEND_SSA_EXIT_TEST_SUPPORT_H

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetInstrInfo.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::test::ssa {

constexpr std::uint32_t DefOpcode = 1024;
constexpr std::uint32_t UseOpcode = 1025;
constexpr std::uint32_t ConditionalBranchOpcode = 1026;
constexpr std::uint32_t JumpOpcode = 1027;
constexpr std::uint32_t ReturnOpcode = 1028;
constexpr std::uint32_t MetaOpcode = 1029;
constexpr std::uint32_t TiedOpcode = 1030;
constexpr std::uint32_t CommutableTiedOpcode = 1031;
constexpr std::uint32_t ConvertibleTiedOpcode = 1032;
constexpr std::uint32_t ThreeAddressOpcode = 1033;

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
                                                            bool AllowSubReg = true) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.AllowSubReg = AllowSubReg;
  Result.AllowAnyTargetFlags = true;
  return Result;
}

[[nodiscard]] inline rcc::OperandDescriptor blockOperand() {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
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

  auto Def = instruction(DefOpcode, "TEST_DEF");
  Def.Operands = {registerOperand(rcc::RegisterOperandRole::Def)};
  Def.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Def));

  auto Use = instruction(UseOpcode, "TEST_USE");
  Use.Operands = {registerOperand(rcc::RegisterOperandRole::Use)};
  Descriptors.push_back(std::move(Use));

  auto Conditional = instruction(ConditionalBranchOpcode, "TEST_BRCOND");
  Conditional.Operands = {registerOperand(rcc::RegisterOperandRole::Use, false), blockOperand()};
  Conditional.Flags.set(rcc::InstructionFlag::Branch);
  Conditional.Flags.set(rcc::InstructionFlag::Terminator);
  Descriptors.push_back(std::move(Conditional));

  auto Jump = instruction(JumpOpcode, "TEST_JUMP");
  Jump.Operands = {blockOperand()};
  Jump.Flags.set(rcc::InstructionFlag::Branch);
  Jump.Flags.set(rcc::InstructionFlag::Terminator);
  Jump.Flags.set(rcc::InstructionFlag::Barrier);
  Descriptors.push_back(std::move(Jump));

  auto Return = instruction(ReturnOpcode, "TEST_RETURN");
  Return.Flags.set(rcc::InstructionFlag::Return);
  Return.Flags.set(rcc::InstructionFlag::Terminator);
  Return.Flags.set(rcc::InstructionFlag::Barrier);
  Descriptors.push_back(std::move(Return));

  auto Meta = instruction(MetaOpcode, "TEST_META");
  Meta.Flags = {rcc::InstructionFlag::Pseudo, rcc::InstructionFlag::Meta};
  Descriptors.push_back(std::move(Meta));

  auto Tied = instruction(TiedOpcode, "TEST_TIED");
  auto TiedDef = registerOperand(rcc::RegisterOperandRole::Def);
  auto TiedUse = registerOperand(rcc::RegisterOperandRole::Use);
  TiedDef.DefaultTiedOperand = 1;
  TiedUse.DefaultTiedOperand = 0;
  Tied.Operands = {TiedDef, TiedUse};
  Tied.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Tied));

  auto Commutable = instruction(CommutableTiedOpcode, "TEST_COMMUTABLE_TIED");
  Commutable.Operands = {TiedDef, TiedUse, registerOperand(rcc::RegisterOperandRole::Use)};
  Commutable.NumExplicitDefs = 1;
  Commutable.Flags.set(rcc::InstructionFlag::Commutable);
  Descriptors.push_back(std::move(Commutable));

  auto Convertible = instruction(ConvertibleTiedOpcode, "TEST_CONVERTIBLE_TIED");
  Convertible.Operands = {TiedDef, TiedUse};
  Convertible.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Convertible));

  auto ThreeAddress = instruction(ThreeAddressOpcode, "TEST_THREE_ADDRESS");
  ThreeAddress.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                           registerOperand(rcc::RegisterOperandRole::Use)};
  ThreeAddress.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(ThreeAddress));

  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors), std::move(Predicates));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

class TestInstrInfo final : public rcc::TargetInstrInfo {
public:
  explicit TestInstrInfo(bool RejectCopies = false) : RejectCopies(RejectCopies) {}

  [[nodiscard]] rcc::Expected<rcc::MachineBranchAnalysis>
  analyzeBranch(rcc::MachineBasicBlock &, rcc::MachineFunction &,
                const rcc::TargetDescription &) const override {
    return rcc::MachineBranchAnalysis{};
  }

  [[nodiscard]] rcc::Expected<void>
  reverseBranchCondition(rcc::MachineInstr &, rcc::MachineBasicBlock &, rcc::MachineFunction &,
                         const rcc::TargetDescription &) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported, "test target has no branch reversal");
  }

  [[nodiscard]] rcc::Expected<void> removeBranch(rcc::MachineInstr &, rcc::MachineFunction &,
                                                 const rcc::TargetDescription &) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported, "test target has no branch removal");
  }

  [[nodiscard]] rcc::Expected<bool>
  isFallthroughEdge(const rcc::MachineBasicBlock &From, const rcc::MachineBasicBlock &To,
                    const rcc::MachineFunction &Function,
                    const rcc::TargetDescription &Target) const override {
    if (&From.function() != &Function || &To.function() != &Function) {
      return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument,
                                 "fallthrough query references another function");
    }
    if (From.next() != &To ||
        std::ranges::none_of(From.successors(), [&To](const rcc::MachineSuccessor &Successor) {
          return Successor.Block == &To;
        })) {
      return false;
    }

    bool CanFallThrough = true;
    for (const rcc::MachineInstr *Instruction = From.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const rcc::InstructionDescriptor *Descriptor =
          Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                   "fallthrough query encountered an unknown opcode");
      }
      if (Descriptor->Flags.has(rcc::InstructionFlag::Terminator))
        CanFallThrough = Instruction->opcode() == testOpcode(ConditionalBranchOpcode);
    }
    return CanFallThrough;
  }

  [[nodiscard]] rcc::Expected<bool>
  rewriteBranchTarget(rcc::MachineBasicBlock &Block, rcc::MachineBasicBlock &OldTarget,
                      rcc::MachineBasicBlock &NewTarget, rcc::MachineFunction &Function,
                      const rcc::TargetDescription &Target) const override {
    if (&Block.function() != &Function || &OldTarget.function() != &Function ||
        &NewTarget.function() != &Function) {
      return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument,
                                 "branch rewrite references another function");
    }
    bool Changed = false;
    for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const rcc::InstructionDescriptor *Descriptor =
          Target.instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr)
        return rcc::makeUnexpected(rcc::ErrorCode::VerificationError, "unknown branch opcode");
      if (!Descriptor->Flags.has(rcc::InstructionFlag::Branch))
        continue;
      for (std::size_t Index = 0; Index < Instruction->operandCount(); ++Index) {
        rcc::MachineOperand *Operand = Instruction->operandAt(Index);
        const auto *BlockOperand =
            Operand == nullptr ? nullptr : Operand->getIf<rcc::BasicBlockOperand>();
        if (BlockOperand == nullptr || BlockOperand->Block != &OldTarget)
          continue;
        auto Rewritten = Operand->setBasicBlock(NewTarget);
        if (!Rewritten)
          return std::unexpected(Rewritten.error());
        Changed = true;
      }
    }
    return Changed;
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *>
  insertBranch(rcc::MachineBasicBlock &Block, rcc::MachineBasicBlock &TargetBlock,
               rcc::MachineFunction &Function,
               const rcc::TargetDescription &Target) const override {
    rcc::MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPoint(Block);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(testOpcode(JumpOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto Destination = Pending->addBasicBlock(TargetBlock);
    if (!Destination)
      return std::unexpected(Destination.error());
    auto Built = Pending->build();
    if (!Built)
      return std::unexpected(Built.error());
    return *Built;
  }

  [[nodiscard]] rcc::Expected<void>
  validateCopy(rcc::Register Destination, rcc::SubRegIndex DestinationSubReg, rcc::Register Source,
               rcc::SubRegIndex SourceSubReg, const rcc::MachineFunction &Function,
               const rcc::TargetDescription &Target) const override {
    if (RejectCopies) {
      return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                                 "test target intentionally rejects COPY");
    }
    if (Destination.isNoRegister() || Source.isNoRegister()) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "COPY requires nonzero registers");
    }
    if ((Destination.isPhysical() && DestinationSubReg.value() != 0) ||
        (Source.isPhysical() && SourceSubReg.value() != 0)) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "physical COPY operands cannot carry subregisters");
    }
    auto Def = rcc::MachineOperand::registerOperand(Destination, {rcc::RegisterOperandFlag::Define},
                                                    DestinationSubReg);
    auto Use =
        rcc::MachineOperand::registerOperand(Source, {rcc::RegisterOperandFlag::Use}, SourceSubReg);
    if (!Def || !Use)
      return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to model COPY operands");
    return Target.validateTieCompatibility(*Def, *Use, Function.registerInfo());
  }

  [[nodiscard]] rcc::Expected<bool>
  tryConvertToThreeAddress(rcc::MachineInstr &Instruction, std::size_t DefIndex,
                           std::size_t UseIndex, rcc::MachineFunction &,
                           const rcc::TargetDescription &) const override {
    if (Instruction.opcode() != testOpcode(ConvertibleTiedOpcode))
      return false;
    if (DefIndex != 0 || UseIndex != 1)
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "unexpected convertible tie indices");
    auto Untied = Instruction.untieOperand(DefIndex);
    if (!Untied)
      return std::unexpected(Untied.error());
    auto Opcode = Instruction.setOpcode(testOpcode(ThreeAddressOpcode));
    if (!Opcode)
      return std::unexpected(Opcode.error());
    return true;
  }

  [[nodiscard]] rcc::Expected<std::uint32_t>
  instructionSize(const rcc::MachineInstr &, const rcc::MachineFunction &,
                  const rcc::TargetDescription &) const override {
    return 4;
  }

  [[nodiscard]] rcc::Expected<rcc::MachineBasicBlock *>
  branchTarget(rcc::MachineInstr &, rcc::MachineFunction &,
               const rcc::TargetDescription &) const override {
    return static_cast<rcc::MachineBasicBlock *>(nullptr);
  }

  [[nodiscard]] bool isBranchOffsetInRange(const rcc::MachineInstr &,
                                           std::int64_t) const noexcept override {
    return true;
  }

  [[nodiscard]] rcc::Expected<bool> relaxBranch(rcc::MachineInstr &, std::int64_t,
                                                rcc::MachineFunction &,
                                                const rcc::TargetDescription &) const override {
    return false;
  }

private:
  bool RejectCopies = false;
};

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  std::vector<rcc::MachineBasicBlock *> Blocks;
};

[[nodiscard]] inline FunctionFixture createFunction(TestContext &Context, std::string Name,
                                                    std::uint64_t ID, std::size_t BlockCount) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "SSA-exit function creation must succeed");
  if (!Function)
    return {};

  FunctionFixture Result{std::move(*Function), {}};
  for (std::size_t Index = 0; Index < BlockCount; ++Index) {
    auto Block = Result.Function->createBasicBlock();
    Context.expect(Block && Result.Function->appendBlock(**Block).has_value(),
                   "SSA-exit block creation must succeed");
    if (!Block)
      return {};
    Result.Blocks.push_back(*Block);
  }
  return Result;
}

[[nodiscard]] inline rcc::Expected<rcc::Register>
createVReg(rcc::MachineFunction &Function, rcc::LLT Type, rcc::RegisterBankID Bank,
           rcc::RegisterClassID Class, std::uint8_t TargetFlags = 0,
           std::optional<rcc::PhysicalRegister> Hint = std::nullopt) {
  auto Reg = Function.registerInfo().createVirtualRegister(Type);
  if (!Reg)
    return std::unexpected(Reg.error());
  auto BankResult = Function.registerInfo().setRegisterBank(*Reg, Bank);
  if (!BankResult)
    return std::unexpected(BankResult.error());
  auto ClassResult = Function.registerInfo().setRegisterClass(*Reg, Class);
  if (!ClassResult)
    return std::unexpected(ClassResult.error());
  if (TargetFlags != 0) {
    auto FlagsResult = Function.registerInfo().setTargetRegisterFlags(*Reg, TargetFlags);
    if (!FlagsResult)
      return std::unexpected(FlagsResult.error());
  }
  if (Hint) {
    auto HintResult = Function.registerInfo().addAllocationHint(*Reg, *Hint);
    if (!HintResult)
      return std::unexpected(HintResult.error());
  }
  return *Reg;
}

[[nodiscard]] inline rcc::Expected<rcc::MachineInstr *>
buildRegisterInstruction(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                         rcc::MachineBasicBlock &Block, rcc::Opcode Opcode,
                         std::span<const rcc::Register> Defs, std::span<const rcc::Register> Uses,
                         rcc::RegisterOperandFlags DefFlags = {},
                         rcc::RegisterOperandFlags UseFlags = {}) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(Opcode);
  if (!Pending)
    return std::unexpected(Pending.error());
  for (rcc::Register Reg : Defs) {
    auto Added = Pending->addDef(Reg, DefFlags);
    if (!Added)
      return std::unexpected(Added.error());
  }
  for (rcc::Register Reg : Uses) {
    auto Added = Pending->addUse(Reg, UseFlags);
    if (!Added)
      return std::unexpected(Added.error());
  }
  return Pending->build();
}

[[nodiscard]] inline rcc::Expected<void>
setSelectedSSAProperties(rcc::MachineFunction &Function, const rcc::TargetDescription &Target) {
  auto Rebuilt = rcc::rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  for (rcc::MachineFunctionProperty Property :
       {rcc::MachineFunctionProperty::IsSSA, rcc::MachineFunctionProperty::TracksLiveness,
        rcc::MachineFunctionProperty::Legalized, rcc::MachineFunctionProperty::RegBankSelected,
        rcc::MachineFunctionProperty::Selected}) {
    Function.properties().set(Property);
  }
  return {};
}

[[nodiscard]] inline std::vector<rcc::MachineInstr *>
instructionsWithOpcode(rcc::MachineFunction &Function, rcc::Opcode Opcode) {
  std::vector<rcc::MachineInstr *> Result;
  for (rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (Instruction->opcode() == Opcode)
        Result.push_back(Instruction);
    }
  }
  return Result;
}

} // namespace rcc::test::ssa

#endif // RCC_BACKEND_SSA_EXIT_TEST_SUPPORT_H
