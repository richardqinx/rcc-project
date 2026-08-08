// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineSSAExit.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

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

[[nodiscard]] rcc::Opcode testOpcode(std::uint32_t Value) {
  return rcc::Opcode::fromValue(Value);
}

[[nodiscard]] rcc::OperandDescriptor registerOperand(rcc::RegisterOperandRole Role,
                                                     bool AllowSubReg = true) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.AllowSubReg = AllowSubReg;
  Result.AllowAnyTargetFlags = true;
  return Result;
}

[[nodiscard]] rcc::OperandDescriptor blockOperand() {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::BasicBlock};
  return Result;
}

[[nodiscard]] rcc::InstructionDescriptor instruction(std::uint32_t Value, std::string Name) {
  rcc::InstructionDescriptor Result;
  Result.InstructionOpcode = testOpcode(Value);
  Result.Name = std::move(Name);
  Result.Flags = {rcc::InstructionFlag::ProducesCode};
  return Result;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
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

class TestSSAInfo final : public rcc::TargetSSAInfo {
public:
  explicit TestSSAInfo(bool RejectCopies = false) : RejectCopies(RejectCopies) {}

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

  [[nodiscard]] rcc::Expected<void>
  insertUnconditionalBranch(rcc::MachineBasicBlock &Block, rcc::MachineBasicBlock &TargetBlock,
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
    return {};
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

private:
  bool RejectCopies = false;
};

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  std::vector<rcc::MachineBasicBlock *> Blocks;
};

[[nodiscard]] FunctionFixture createFunction(TestContext &Context, std::string Name,
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

[[nodiscard]] rcc::Expected<rcc::Register>
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

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
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

[[nodiscard]] rcc::Expected<rcc::MachineInstr *> buildJump(rcc::MachineFunction &Function,
                                                           const rcc::TargetDescription &Target,
                                                           rcc::MachineBasicBlock &Block,
                                                           rcc::MachineBasicBlock &Destination) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(testOpcode(JumpOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto TargetOperand = Pending->addBasicBlock(Destination);
  if (!TargetOperand)
    return std::unexpected(TargetOperand.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildPhi(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Destination,
         std::span<const std::pair<rcc::Register, rcc::MachineBasicBlock *>> Incoming,
         rcc::RegisterOperandFlags DestinationFlags = {}, std::uint32_t DestinationTargetFlags = 0,
         rcc::RegisterOperandFlags IncomingFlags = {}, std::uint32_t IncomingTargetFlags = 0) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination, DestinationFlags, {}, DestinationTargetFlags);
  if (!Def)
    return std::unexpected(Def.error());
  for (const auto &[Value, Predecessor] : Incoming) {
    auto Use = Pending->addUse(Value, IncomingFlags, {}, IncomingTargetFlags);
    if (!Use)
      return std::unexpected(Use.error());
    auto IncomingBlock = Pending->addBasicBlock(*Predecessor);
    if (!IncomingBlock)
      return std::unexpected(IncomingBlock.error());
  }
  return Pending->build();
}

[[nodiscard]] rcc::Expected<void> setSelectedSSAProperties(rcc::MachineFunction &Function,
                                                           const rcc::TargetDescription &Target) {
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

[[nodiscard]] rcc::Expected<void> runPHIElimination(rcc::MachineFunction &Function,
                                                    const rcc::TargetDescription &Target,
                                                    const rcc::TargetSSAInfo &Info) {
  rcc::MachinePassManager Pipeline;
  auto Added = Pipeline.addPass(std::make_unique<rcc::PHIEliminationPass>(Info));
  if (!Added)
    return std::unexpected(Added.error());
  rcc::MachineAnalysisManager Analyses;
  return Pipeline.run(Function, Target, Analyses);
}

[[nodiscard]] std::vector<rcc::MachineInstr *> linkedInstructions(rcc::MachineBasicBlock &Block) {
  std::vector<rcc::MachineInstr *> Result;
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    Result.push_back(Instruction);
  }
  return Result;
}

[[nodiscard]] std::vector<rcc::MachineInstr *>
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

[[nodiscard]] bool hasLiveIn(const rcc::MachineBasicBlock &Block, rcc::PhysicalRegister Reg) {
  return std::ranges::any_of(Block.liveIns(), [Reg](const rcc::MachineBasicBlockLiveIn &LiveIn) {
    return LiveIn.PhysReg == Reg;
  });
}

void testParallelPHIsAndPlacement(TestContext &Context, const rcc::TargetDescription &Target,
                                  const TestSSAInfo &Info) {
  auto Fixture = createFunction(Context, "parallel_phis", 1, 3);
  if (!Fixture.Function || Fixture.Blocks.size() != 3)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  if (!I64)
    return;
  auto &Function = *Fixture.Function;
  rcc::MachineBasicBlock &Preheader = *Fixture.Blocks[0];
  rcc::MachineBasicBlock &Header = *Fixture.Blocks[1];
  rcc::MachineBasicBlock &Backedge = *Fixture.Blocks[2];
  const rcc::PhysicalRegister Hint = *rcc::PhysicalRegister::create(rcc::test::TestRegister::G1);

  auto InitA = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                          rcc::test::TestRegisterClass::GPR64);
  auto InitB = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                          rcc::test::TestRegisterClass::GPR64);
  auto DestinationA = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                                 rcc::test::TestRegisterClass::GPR64, 0x1, Hint);
  auto DestinationB = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                                 rcc::test::TestRegisterClass::GPR64);
  Context.expect(InitA && InitB && DestinationA && DestinationB,
                 "parallel PHI registers must be created");
  if (!InitA || !InitB || !DestinationA || !DestinationB)
    return;

  auto CFG = Function.cfgUpdater();
  Context.expect(
      CFG.addSuccessor(Preheader, Header, rcc::BranchProbability::one()).has_value() &&
          CFG.addSuccessor(Header, Backedge, rcc::BranchProbability::one()).has_value() &&
          CFG.addSuccessor(Backedge, Header, rcc::BranchProbability::one()).has_value(),
      "parallel PHI loop CFG must be connected");

  const std::array InitADef{*InitA};
  const std::array InitBDef{*InitB};
  Context.expect(buildRegisterInstruction(Function, Target, Preheader, testOpcode(DefOpcode),
                                          InitADef, {}, {rcc::RegisterOperandFlag::Dead}) &&
                     buildRegisterInstruction(Function, Target, Preheader, testOpcode(DefOpcode),
                                              InitBDef, {}) &&
                     buildJump(Function, Target, Preheader, Header),
                 "parallel PHI preheader must be populated");

  const std::array IncomingA{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*InitA, &Preheader},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*DestinationB, &Backedge}};
  const std::array IncomingB{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*InitB, &Preheader},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*DestinationA, &Backedge}};
  Context.expect(buildPhi(Function, Target, Header, *DestinationA, IncomingA, {}, 0x4, {}, 0x8) &&
                     buildPhi(Function, Target, Header, *DestinationB, IncomingB),
                 "parallel PHIs must be built");

  rcc::MachineIRBuilder HeaderBuilder(Function, Target);
  if (!HeaderBuilder.setInsertPoint(Header))
    return;
  auto Meta = HeaderBuilder.buildInstr(testOpcode(MetaOpcode));
  const std::array HeaderUse{*DestinationA};
  Context.expect(Meta && Meta->build() &&
                     buildRegisterInstruction(Function, Target, Header, testOpcode(UseOpcode), {},
                                              HeaderUse) &&
                     buildJump(Function, Target, Header, Backedge),
                 "parallel PHI header must contain meta, code, and terminator instructions");

  const std::array KilledUse{*DestinationA};
  Context.expect(buildRegisterInstruction(Function, Target, Backedge, testOpcode(UseOpcode), {},
                                          KilledUse, {}, {rcc::RegisterOperandFlag::Kill}) &&
                     buildJump(Function, Target, Backedge, Header),
                 "parallel PHI backedge must be populated");
  auto InitialLiveness = setSelectedSSAProperties(Function, Target);
  Context.expect(InitialLiveness.has_value(), "parallel PHI liveness rebuild must succeed");
  if (!InitialLiveness)
    return;

  auto Run = runPHIElimination(Function, Target, Info);
  Context.expect(Run.has_value(), "parallel PHI elimination must succeed");
  if (!Run)
    return;
  Context.expect(!Function.properties().has(rcc::MachineFunctionProperty::IsSSA) &&
                     Function.properties().has(rcc::MachineFunctionProperty::NoPHIs) &&
                     Function.properties().has(rcc::MachineFunctionProperty::TracksLiveness),
                 "PHI elimination must publish its exact property transition");
  Context.expect(instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::PHI)).empty(),
                 "PHI elimination must remove every PHI");

  const auto HeaderInstructions = linkedInstructions(Header);
  Context.expect(HeaderInstructions.size() == 5 &&
                     HeaderInstructions[0]->opcode() == testOpcode(MetaOpcode) &&
                     HeaderInstructions[1]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     HeaderInstructions[2]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     HeaderInstructions[3]->opcode() == testOpcode(UseOpcode) &&
                     HeaderInstructions[4]->opcode() == testOpcode(JumpOpcode),
                 "destination COPYs must follow the full leading PHI/meta region");
  if (HeaderInstructions.size() < 3)
    return;

  const rcc::Register *FreshA = HeaderInstructions[1]->operandAt(1)->getIf<rcc::Register>();
  const rcc::Register *FreshB = HeaderInstructions[2]->operandAt(1)->getIf<rcc::Register>();
  Context.expect(FreshA != nullptr && FreshB != nullptr && *FreshA != *FreshB &&
                     *FreshA != *DestinationA && *FreshB != *DestinationB,
                 "each PHI must receive an independent fresh incoming vreg");
  if (FreshA == nullptr || FreshB == nullptr)
    return;

  const auto BackedgeInstructions = linkedInstructions(Backedge);
  Context.expect(BackedgeInstructions.size() == 4 &&
                     BackedgeInstructions[1]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     BackedgeInstructions[2]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     BackedgeInstructions[3]->opcode() == testOpcode(JumpOpcode),
                 "incoming COPYs must be placed together before the first terminator");
  if (BackedgeInstructions.size() >= 3) {
    const auto FirstDef = BackedgeInstructions[1]->operandAt(0)->registerValue();
    const auto FirstUse = BackedgeInstructions[1]->operandAt(1)->registerValue();
    const auto SecondDef = BackedgeInstructions[2]->operandAt(0)->registerValue();
    const auto SecondUse = BackedgeInstructions[2]->operandAt(1)->registerValue();
    Context.expect(FirstDef == *FreshA && FirstUse == *DestinationB && SecondDef == *FreshB &&
                       SecondUse == *DestinationA,
                   "backedge COPYs must read old PHI destinations into distinct temporaries");
  }

  auto FreshInfo = Function.registerInfo().virtualRegisterInfo(*FreshA);
  Context.expect(FreshInfo.has_value(), "fresh PHI vreg must remain registered");
  if (!FreshInfo)
    return;
  Context.expect(FreshInfo->get().Type == I64, "fresh PHI vreg must clone its LLT");
  Context.expect(FreshInfo->get().Bank == rcc::test::TestRegisterBank::GPR,
                 "fresh PHI vreg must clone its register bank");
  Context.expect(FreshInfo->get().RegClass == rcc::test::TestRegisterClass::GPR64,
                 "fresh PHI vreg must clone its register class");
  Context.expect(FreshInfo->get().TargetFlags == 0x1,
                 "fresh PHI vreg must clone registered target flags");
  Context.expect(FreshInfo->get().AllocationHints.size() == 1,
                 "fresh PHI vreg must clone the allocation-hint count");
  Context.expect(!FreshInfo->get().AllocationHints.empty() &&
                     FreshInfo->get().AllocationHints.front() == Hint,
                 "fresh PHI vreg must clone the allocation-hint register");
  Context.expect(HeaderInstructions[1]->operandAt(0)->targetFlags() == 0x4 &&
                     HeaderInstructions[1]->operandAt(1)->targetFlags() == 0x4,
                 "successor COPY must preserve registered destination target flags");

  const rcc::MachineInstr *InitDefinition = Preheader.firstInstruction();
  Context.expect(
      InitDefinition != nullptr &&
          !InitDefinition->operandAt(0)->registerFlags().has(rcc::RegisterOperandFlag::Dead) &&
          !BackedgeInstructions[0]->operandAt(0)->registerFlags().has(
              rcc::RegisterOperandFlag::Kill),
      "liveness-preserving PHI elimination must conservatively clear kill/dead flags");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "parallel PHI result must pass MachineVerifier");
}

void testCriticalEdgeAndPhysicalLiveIn(TestContext &Context, const rcc::TargetDescription &Target,
                                       const TestSSAInfo &Info) {
  auto Fixture = createFunction(Context, "critical_edge", 2, 4);
  if (!Fixture.Function || Fixture.Blocks.size() != 4)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  if (!I64)
    return;
  auto &Function = *Fixture.Function;
  rcc::MachineBasicBlock &From = *Fixture.Blocks[0];
  rcc::MachineBasicBlock &Side = *Fixture.Blocks[1];
  rcc::MachineBasicBlock &Other = *Fixture.Blocks[2];
  rcc::MachineBasicBlock &Merge = *Fixture.Blocks[3];
  auto Condition = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                              rcc::test::TestRegisterClass::GPR64);
  auto OtherValue = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                               rcc::test::TestRegisterClass::GPR64);
  auto Destination = createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                                rcc::test::TestRegisterClass::GPR64);
  if (!Condition || !OtherValue || !Destination)
    return;

  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function.cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(From, Side, *Half) &&
                     CFG.addSuccessor(From, Merge, *Half) &&
                     CFG.addSuccessor(Other, Merge, rcc::BranchProbability::one()),
                 "critical-edge CFG must be connected");

  const std::array ConditionDef{*Condition};
  Context.expect(
      buildRegisterInstruction(Function, Target, From, testOpcode(DefOpcode), ConditionDef, {})
          .has_value(),
      "critical-edge condition must be defined");
  rcc::MachineIRBuilder FromBuilder(Function, Target);
  if (!FromBuilder.setInsertPoint(From))
    return;
  auto Branch = FromBuilder.buildInstr(testOpcode(ConditionalBranchOpcode));
  Context.expect(Branch && Branch->addUse(*Condition) && Branch->addBasicBlock(Merge) &&
                     Branch->build(),
                 "critical-edge conditional branch must be built");
  const std::array OtherDef{*OtherValue};
  Context.expect(
      buildRegisterInstruction(Function, Target, Other, testOpcode(DefOpcode), OtherDef, {})
          .has_value(),
      "unrelated fallthrough predecessor must define its incoming value");

  const rcc::PhysicalRegister G0 = *rcc::PhysicalRegister::create(rcc::test::TestRegister::G0);
  const rcc::PhysicalRegister G1 = *rcc::PhysicalRegister::create(rcc::test::TestRegister::G1);
  Context.expect(Merge.addLiveIn(G1, rcc::LaneMask::fromRaw(0xF)).has_value(),
                 "merge block live-in setup must succeed");
  const std::array Incoming{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{rcc::Register::fromPhysical(G0), &From},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*OtherValue, &Other}};
  Context.expect(buildPhi(Function, Target, Merge, *Destination, Incoming).has_value(),
                 "critical-edge PHI must be built");
  const std::array G1Use{rcc::Register::fromPhysical(G1)};
  Context.expect(buildRegisterInstruction(Function, Target, Merge, testOpcode(UseOpcode), {}, G1Use)
                     .has_value(),
                 "merge block must consume its inherited physical live-in");
  auto InitialLiveness = setSelectedSSAProperties(Function, Target);
  Context.expect(InitialLiveness.has_value(), "critical-edge PHI liveness rebuild must succeed");
  if (!InitialLiveness)
    return;

  auto Run = runPHIElimination(Function, Target, Info);
  Context.expect(Run.has_value(), "critical-edge PHI elimination must succeed");
  if (!Run)
    return;

  rcc::MachineBasicBlock *Edge = Merge.previous();
  Context.expect(
      Edge != nullptr && Edge != &Other && Edge->previous() == &Other &&
          From.successors().size() == 2 &&
          std::ranges::any_of(From.successors(),
                              [Edge](const rcc::MachineSuccessor &S) { return S.Block == Edge; }) &&
          Edge->predecessors().size() == 1 && Edge->predecessors()[0] == &From &&
          Edge->successors().size() == 1 && Edge->successors()[0].Block == &Merge,
      "critical edge must become a reciprocal From-to-edge-to-Merge CFG path");
  if (Edge == nullptr || Edge == &Other)
    return;

  const rcc::MachineOperand *BranchTarget = From.lastInstruction()->operandAt(1);
  const auto *BranchBlock =
      BranchTarget == nullptr ? nullptr : BranchTarget->getIf<rcc::BasicBlockOperand>();
  Context.expect(BranchBlock != nullptr && BranchBlock->Block == Edge,
                 "explicit critical-edge branch must target the split block");
  const auto OtherInstructions = linkedInstructions(Other);
  const auto EdgeInstructions = linkedInstructions(*Edge);
  Context.expect(OtherInstructions.size() == 3 &&
                     OtherInstructions.back()->opcode() == testOpcode(JumpOpcode) &&
                     EdgeInstructions.size() == 2 &&
                     EdgeInstructions[0]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     EdgeInstructions[1]->opcode() == testOpcode(JumpOpcode),
                 "edge split must preserve unrelated fallthrough and terminator placement");
  Context.expect(hasLiveIn(*Edge, G0) && hasLiveIn(*Edge, G1),
                 "split edge must inherit successor live-ins and its physical PHI source");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "critical-edge result must pass MachineVerifier");
}

void testUndefAndFailureAtomicity(TestContext &Context, const rcc::TargetDescription &Target,
                                  const TestSSAInfo &Info) {
  auto BuildFixture = [&](std::uint64_t ID) {
    auto Fixture = createFunction(Context, "undef_phi", ID, 2);
    if (!Fixture.Function || Fixture.Blocks.size() != 2)
      return Fixture;
    auto I64 = rcc::LLT::makeInteger(64);
    if (!I64)
      return Fixture;
    auto Source = createVReg(*Fixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                             rcc::test::TestRegisterClass::GPR64);
    auto Destination = createVReg(*Fixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                                  rcc::test::TestRegisterClass::GPR64);
    if (!Source || !Destination)
      return Fixture;
    auto CFG = Fixture.Function->cfgUpdater();
    if (!CFG.addSuccessor(*Fixture.Blocks[0], *Fixture.Blocks[1], rcc::BranchProbability::one()) ||
        !buildJump(*Fixture.Function, Target, *Fixture.Blocks[0], *Fixture.Blocks[1])) {
      return Fixture;
    }
    const std::array Incoming{
        std::pair<rcc::Register, rcc::MachineBasicBlock *>{*Source, Fixture.Blocks[0]}};
    if (!buildPhi(*Fixture.Function, Target, *Fixture.Blocks[1], *Destination, Incoming, {}, 0,
                  {rcc::RegisterOperandFlag::Undef})) {
      return Fixture;
    }
    Context.expect(setSelectedSSAProperties(*Fixture.Function, Target).has_value(),
                   "undef PHI liveness rebuild must succeed");
    return Fixture;
  };

  auto Undef = BuildFixture(3);
  if (!Undef.Function)
    return;
  auto Run = runPHIElimination(*Undef.Function, Target, Info);
  Context.expect(Run.has_value(), "undef PHI elimination must succeed");
  if (Run) {
    const auto PredecessorInstructions = linkedInstructions(*Undef.Blocks[0]);
    Context.expect(PredecessorInstructions.size() == 2 &&
                       PredecessorInstructions[0]->opcode() ==
                           rcc::opcode(rcc::GenericOpcode::IMPLICIT_DEF) &&
                       PredecessorInstructions[1]->opcode() == testOpcode(JumpOpcode),
                   "undef PHI incoming values must become edge IMPLICIT_DEF instructions");
  }

  auto Atomic = BuildFixture(4);
  if (!Atomic.Function)
    return;
  const std::size_t BlocksBefore = Atomic.Function->blockCount();
  const std::size_t InstructionsBefore =
      instructionsWithOpcode(*Atomic.Function, rcc::opcode(rcc::GenericOpcode::PHI)).size();
  const std::size_t RegistersBefore = Atomic.Function->registerInfo().virtualRegisters().size();
  const std::uint64_t PropertiesBefore = Atomic.Function->properties().rawValue();
  TestSSAInfo RejectingInfo(true);
  rcc::MachineAnalysisManager Analyses;
  rcc::PHIEliminationPass FailingPass(RejectingInfo);
  auto Failed = FailingPass.run(*Atomic.Function, Target, Analyses);
  Context.expect(!Failed && Failed.error().code() == rcc::ErrorCode::Unsupported,
                 "COPY rejection must produce a structured PHI-elimination failure");
  Context.expect(
      Atomic.Function->blockCount() == BlocksBefore &&
          instructionsWithOpcode(*Atomic.Function, rcc::opcode(rcc::GenericOpcode::PHI)).size() ==
              InstructionsBefore &&
          Atomic.Function->registerInfo().virtualRegisters().size() == RegistersBefore &&
          Atomic.Function->properties().rawValue() == PropertiesBefore,
      "failed PHI transaction preflight must leave the original function unchanged");

  rcc::PHIEliminationPass MissingBehavior;
  auto Missing = MissingBehavior.run(*Atomic.Function, Target, Analyses);
  Context.expect(!Missing && Missing.error().code() == rcc::ErrorCode::InvalidArgument,
                 "PHI elimination without target behavior must fail explicitly");
}

void testTwoAddressAndSubregisterPseudos(TestContext &Context, const rcc::TargetDescription &Target,
                                         const TestSSAInfo &Info) {
  auto Fixture = createFunction(Context, "two_address", 5, 1);
  if (!Fixture.Function || Fixture.Blocks.empty())
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  auto I32 = rcc::LLT::makeInteger(32);
  if (!I64 || !I32)
    return;
  auto &Function = *Fixture.Function;
  rcc::MachineBasicBlock &Block = *Fixture.Blocks[0];
  auto GPR64 = [&](std::uint8_t Flags = 0) {
    return createVReg(Function, *I64, rcc::test::TestRegisterBank::GPR,
                      rcc::test::TestRegisterClass::GPR64, Flags);
  };
  auto GPR32 = [&]() {
    return createVReg(Function, *I32, rcc::test::TestRegisterBank::GPR,
                      rcc::test::TestRegisterClass::GPR32);
  };

  auto Satisfied = GPR64();
  auto UndefSource = GPR64();
  auto UndefDestination = GPR64();
  auto CommuteSource = GPR64();
  auto CommuteDestination = GPR64();
  auto ConvertSource = GPR64();
  auto ConvertDestination = GPR64();
  auto FallbackSource = GPR64();
  auto FallbackDestination = GPR64();
  auto SequenceDestination = GPR64();
  auto SequenceLow = GPR32();
  auto SequenceHigh = GPR32();
  auto InsertDestination = GPR64();
  auto InsertBase = GPR64();
  auto Inserted = GPR32();
  const bool Complete = Satisfied && UndefSource && UndefDestination && CommuteSource &&
                        CommuteDestination && ConvertSource && ConvertDestination &&
                        FallbackSource && FallbackDestination && SequenceDestination &&
                        SequenceLow && SequenceHigh && InsertDestination && InsertBase && Inserted;
  Context.expect(Complete, "two-address test registers must be created");
  if (!Complete)
    return;

  for (rcc::Register Source : {*CommuteSource, *ConvertSource, *FallbackSource, *SequenceLow,
                               *SequenceHigh, *InsertBase, *Inserted}) {
    const std::array Def{Source};
    Context.expect(buildRegisterInstruction(Function, Target, Block, testOpcode(DefOpcode), Def, {},
                                            {rcc::RegisterOperandFlag::Dead})
                       .has_value(),
                   "two-address source definition must be built");
  }
  const std::array UndefDef{*UndefSource};
  Context.expect(buildRegisterInstruction(Function, Target, Block,
                                          rcc::opcode(rcc::GenericOpcode::IMPLICIT_DEF), UndefDef,
                                          {})
                     .has_value(),
                 "two-address undef source must be defined by IMPLICIT_DEF");

  auto BuildTied = [&](rcc::Opcode Opcode, rcc::Register Def, rcc::Register Use,
                       std::optional<rcc::Register> Other = std::nullopt) {
    rcc::MachineIRBuilder Builder(Function, Target);
    auto Point = Builder.setInsertPoint(Block);
    if (!Point)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(Point.error())};
    auto Pending = Builder.buildInstr(Opcode);
    if (!Pending)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(Pending.error())};
    auto AddedDef = Pending->addDef(Def);
    if (!AddedDef)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(AddedDef.error())};
    auto AddedUse = Pending->addUse(Use, {rcc::RegisterOperandFlag::Kill});
    if (!AddedUse)
      return rcc::Expected<rcc::MachineInstr *>{std::unexpected(AddedUse.error())};
    if (Other) {
      auto AddedOther = Pending->addUse(*Other);
      if (!AddedOther)
        return rcc::Expected<rcc::MachineInstr *>{std::unexpected(AddedOther.error())};
    }
    return Pending->build();
  };

  auto AlreadyTied = BuildTied(testOpcode(TiedOpcode), *Satisfied, *Satisfied);
  auto UndefTied = BuildTied(testOpcode(TiedOpcode), *UndefDestination, *UndefSource);
  auto Commutable = BuildTied(testOpcode(CommutableTiedOpcode), *CommuteDestination, *CommuteSource,
                              *CommuteDestination);
  auto Convertible =
      BuildTied(testOpcode(ConvertibleTiedOpcode), *ConvertDestination, *ConvertSource);
  auto Fallback = BuildTied(testOpcode(TiedOpcode), *FallbackDestination, *FallbackSource);
  Context.expect(AlreadyTied && UndefTied && Commutable && Convertible && Fallback,
                 "two-address instruction fixtures must be built");
  if (!AlreadyTied || !UndefTied || !Commutable || !Convertible || !Fallback)
    return;

  rcc::MachineIRBuilder Builder(Function, Target);
  if (!Builder.setInsertPoint(Block))
    return;
  auto Sequence = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::REG_SEQUENCE));
  Context.expect(
      Sequence && Sequence->addDef(*SequenceDestination) && Sequence->addUse(*SequenceLow) &&
          Sequence->addImmediate(rcc::test::TestSubReg::Low32.value()) &&
          Sequence->addUse(*SequenceHigh) &&
          Sequence->addImmediate(rcc::test::TestSubReg::High32.value()) && Sequence->build(),
      "REG_SEQUENCE fixture must be built");
  auto Insert = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::INSERT_SUBREG));
  Context.expect(Insert && Insert->addDef(*InsertDestination) && Insert->addUse(*InsertBase) &&
                     Insert->addUse(*Inserted) &&
                     Insert->addImmediate(rcc::test::TestSubReg::Low32.value()) && Insert->build(),
                 "INSERT_SUBREG fixture must be built");
  auto Return = Builder.buildInstr(testOpcode(ReturnOpcode));
  Context.expect(Return && Return->build(), "two-address return must be built");

  auto InitialLiveness = setSelectedSSAProperties(Function, Target);
  Context.expect(InitialLiveness.has_value(), "two-address liveness rebuild must succeed");
  if (!InitialLiveness)
    return;
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  rcc::MachinePassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<rcc::TwoAddressRewritePass>(Info)).has_value(),
                 "two-address pass must be registered");
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pipeline.run(Function, Target, Analyses);
  Context.expect(Run.has_value(), "two-address rewrite must succeed");
  if (!Run)
    return;

  Context.expect(Function.properties().has(rcc::MachineFunctionProperty::TiedOpsRewritten) &&
                     !Function.properties().has(rcc::MachineFunctionProperty::IsSSA),
                 "two-address rewrite must publish TiedOpsRewritten and clear IsSSA");
  Context.expect(
      instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::REG_SEQUENCE)).empty() &&
          instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::INSERT_SUBREG)).empty(),
      "TwoAddressRewrite must consume all Stage 10 subregister pseudos");

  Context.expect((*AlreadyTied)->operandAt(1)->registerValue() == *Satisfied,
                 "an already-satisfied tie must remain unchanged");
  Context.expect(
      (*UndefTied)->operandAt(1)->registerValue() == *UndefDestination &&
          (*UndefTied)->operandAt(1)->registerFlags().has(rcc::RegisterOperandFlag::Undef),
      "undef tied uses must be rewritten without a COPY");
  Context.expect((*Commutable)->operandAt(1)->registerValue() == *CommuteDestination &&
                     (*Commutable)->operandAt(2)->registerValue() == *CommuteSource,
                 "commutation must satisfy a tie before COPY fallback");
  Context.expect((*Convertible)->opcode() == testOpcode(ThreeAddressOpcode) &&
                     !(*Convertible)->operandAt(0)->tiedOperandIndex() &&
                     !(*Convertible)->operandAt(1)->tiedOperandIndex(),
                 "target three-address conversion must run before COPY fallback");
  Context.expect((*Fallback)->previous() != nullptr &&
                     (*Fallback)->previous()->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     (*Fallback)->previous()->operandAt(0)->registerValue() ==
                         *FallbackDestination &&
                     (*Fallback)->previous()->operandAt(1)->registerValue() == *FallbackSource &&
                     (*Fallback)->operandAt(1)->registerValue() == *FallbackDestination,
                 "unsatisfied ties must receive a COPY immediately before the instruction");

  const auto Copies = instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::COPY));
  const auto ImplicitDefs =
      instructionsWithOpcode(Function, rcc::opcode(rcc::GenericOpcode::IMPLICIT_DEF));
  Context.expect(Copies.size() >= 5 && ImplicitDefs.size() >= 2,
                 "subregister pseudo expansion must materialize base and lane definitions");
  Context.expect(
      std::ranges::any_of(Copies,
                          [InsertDestination](const rcc::MachineInstr *Copy) {
                            return Copy->operandAt(0)->registerValue() == *InsertDestination &&
                                   Copy->operandAt(0)->subReg() == rcc::test::TestSubReg::Low32;
                          }),
      "INSERT_SUBREG must copy its inserted value into the requested destination lane");
  const bool HasKill = std::ranges::any_of(Function.allInstructions(), [](const auto *MI) {
    return MI->parent() != nullptr &&
           std::ranges::any_of(MI->operands(), [](const rcc::MachineOperand &Operand) {
             return Operand.registerFlags().has(rcc::RegisterOperandFlag::Kill);
           });
  });
  const bool HasDead = std::ranges::any_of(Function.allInstructions(), [](const auto *MI) {
    return MI->parent() != nullptr &&
           std::ranges::any_of(MI->operands(), [](const rcc::MachineOperand &Operand) {
             return Operand.registerFlags().has(rcc::RegisterOperandFlag::Dead);
           });
  });
  Context.expect(HasKill && HasDead,
                 "two-address rewrite must rebuild precise kill and dead flags");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "two-address and subregister expansion result must verify");

  auto AtomicFixture = createFunction(Context, "two_address_atomic", 10, 1);
  if (!AtomicFixture.Function || AtomicFixture.Blocks.empty())
    return;
  auto AtomicSource = createVReg(*AtomicFixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                                 rcc::test::TestRegisterClass::GPR64);
  auto AtomicDestination =
      createVReg(*AtomicFixture.Function, *I64, rcc::test::TestRegisterBank::GPR,
                 rcc::test::TestRegisterClass::GPR64);
  if (!AtomicSource || !AtomicDestination)
    return;
  const std::array AtomicSourceDef{*AtomicSource};
  Context.expect(buildRegisterInstruction(*AtomicFixture.Function, Target, *AtomicFixture.Blocks[0],
                                          testOpcode(DefOpcode), AtomicSourceDef, {})
                     .has_value(),
                 "two-address atomicity source definition must build");
  rcc::MachineIRBuilder AtomicBuilder(*AtomicFixture.Function, Target);
  if (!AtomicBuilder.setInsertPoint(*AtomicFixture.Blocks[0]))
    return;
  auto AtomicPending = AtomicBuilder.buildInstr(testOpcode(TiedOpcode));
  auto AtomicInstruction =
      AtomicPending && AtomicPending->addDef(*AtomicDestination) &&
              AtomicPending->addUse(*AtomicSource, {rcc::RegisterOperandFlag::Kill})
          ? AtomicPending->build()
          : rcc::Expected<rcc::MachineInstr *>{rcc::makeUnexpected(
                rcc::ErrorCode::InternalError, "failed to build atomicity fixture")};
  Context.expect(AtomicInstruction.has_value(),
                 "two-address atomicity tied instruction must build");
  if (!AtomicInstruction)
    return;
  auto AtomicLiveness = setSelectedSSAProperties(*AtomicFixture.Function, Target);
  Context.expect(AtomicLiveness.has_value(), "two-address atomicity liveness rebuild must succeed");
  if (!AtomicLiveness)
    return;
  AtomicFixture.Function->properties().set(rcc::MachineFunctionProperty::NoPHIs);
  const std::size_t AtomicInstructionsBefore = AtomicFixture.Function->allInstructions().size();
  const std::size_t AtomicRegistersBefore =
      AtomicFixture.Function->registerInfo().virtualRegisters().size();
  const std::uint64_t AtomicPropertiesBefore = AtomicFixture.Function->properties().rawValue();

  TestSSAInfo RejectingInfo(true);
  rcc::MachineAnalysisManager AtomicAnalyses;
  rcc::TwoAddressRewritePass FailingPass(RejectingInfo);
  auto Failed = FailingPass.run(*AtomicFixture.Function, Target, AtomicAnalyses);
  Context.expect(!Failed && Failed.error().code() == rcc::ErrorCode::Unsupported,
                 "COPY rejection must produce a structured two-address failure");
  Context.expect(
      AtomicFixture.Function->allInstructions().size() == AtomicInstructionsBefore &&
          AtomicFixture.Function->registerInfo().virtualRegisters().size() ==
              AtomicRegistersBefore &&
          AtomicFixture.Function->properties().rawValue() == AtomicPropertiesBefore &&
          (*AtomicInstruction)->operandAt(0)->registerValue() == *AtomicDestination &&
          (*AtomicInstruction)->operandAt(1)->registerValue() == *AtomicSource &&
          (*AtomicInstruction)->operandAt(1)->registerFlags().has(rcc::RegisterOperandFlag::Kill),
      "failed two-address transaction preflight must leave the original function unchanged");

  rcc::TwoAddressRewritePass MissingBehavior;
  auto Missing = MissingBehavior.run(*AtomicFixture.Function, Target, AtomicAnalyses);
  Context.expect(!Missing && Missing.error().code() == rcc::ErrorCode::InvalidArgument,
                 "two-address rewrite without target behavior must fail explicitly");
}

void testVerifierAndPseudoFailures(TestContext &Context, const rcc::TargetDescription &Target) {
  auto BuildBadPhi = [&](std::uint64_t ID, rcc::LLT DestinationType,
                         rcc::RegisterBankID DestinationBank, rcc::RegisterClassID DestinationClass,
                         rcc::LLT SourceType, rcc::RegisterBankID SourceBank,
                         rcc::RegisterClassID SourceClass, rcc::SubRegIndex SourceSubReg = {}) {
    auto Fixture = createFunction(Context, "bad_phi", ID, 2);
    if (!Fixture.Function || Fixture.Blocks.size() != 2)
      return Fixture;
    auto Destination =
        createVReg(*Fixture.Function, DestinationType, DestinationBank, DestinationClass);
    auto Source = createVReg(*Fixture.Function, SourceType, SourceBank, SourceClass);
    if (!Destination || !Source)
      return Fixture;
    auto CFG = Fixture.Function->cfgUpdater();
    if (!CFG.addSuccessor(*Fixture.Blocks[0], *Fixture.Blocks[1], rcc::BranchProbability::one())) {
      return Fixture;
    }
    const std::array SourceDef{*Source};
    if (!buildRegisterInstruction(*Fixture.Function, Target, *Fixture.Blocks[0],
                                  testOpcode(DefOpcode), SourceDef, {})) {
      return Fixture;
    }
    rcc::MachineIRBuilder Builder(*Fixture.Function, Target);
    if (!Builder.setInsertPoint(*Fixture.Blocks[1]))
      return Fixture;
    auto Phi = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI));
    if (!Phi || !Phi->addDef(*Destination) || !Phi->addUse(*Source, {}, SourceSubReg) ||
        !Phi->addBasicBlock(*Fixture.Blocks[0]) || !Phi->build()) {
      return Fixture;
    }
    return Fixture;
  };

  auto I64 = rcc::LLT::makeInteger(64);
  auto I32 = rcc::LLT::makeInteger(32);
  if (!I64 || !I32)
    return;
  auto BadType =
      BuildBadPhi(6, *I64, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64,
                  *I32, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR32);
  Context.expect(BadType.Function && !rcc::MachineVerifier::verify(*BadType.Function, Target),
                 "MachineVerifier must reject incompatible PHI LLTs");

  auto BadClass =
      BuildBadPhi(7, *I64, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64,
                  *I64, rcc::test::TestRegisterBank::FPR, rcc::test::TestRegisterClass::FPR64);
  Context.expect(BadClass.Function && !rcc::MachineVerifier::verify(*BadClass.Function, Target),
                 "MachineVerifier must reject nonoverlapping PHI register classes");

  auto BadSubReg =
      BuildBadPhi(8, *I32, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR32Low,
                  *I64, rcc::test::TestRegisterBank::GPR, rcc::test::TestRegisterClass::GPR64,
                  rcc::test::TestSubReg::High32);
  Context.expect(BadSubReg.Function && !rcc::MachineVerifier::verify(*BadSubReg.Function, Target),
                 "MachineVerifier must reject incompatible PHI subregister projections");

  auto Residual = createFunction(Context, "residual_stage10_pseudo", 9, 1);
  if (!Residual.Function || Residual.Blocks.empty())
    return;
  auto Destination = createVReg(*Residual.Function, *I64, rcc::test::TestRegisterBank::GPR,
                                rcc::test::TestRegisterClass::GPR64);
  auto Source = createVReg(*Residual.Function, *I32, rcc::test::TestRegisterBank::GPR,
                           rcc::test::TestRegisterClass::GPR32);
  if (!Destination || !Source)
    return;
  rcc::MachineIRBuilder Builder(*Residual.Function, Target);
  if (!Builder.setInsertPoint(*Residual.Blocks[0]))
    return;
  auto Sequence = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::REG_SEQUENCE));
  Context.expect(Sequence && Sequence->addDef(*Destination) && Sequence->addUse(*Source) &&
                     Sequence->addImmediate(rcc::test::TestSubReg::Low32.value()) &&
                     Sequence->build(),
                 "residual Stage 10 pseudo fixture must be built");
  for (rcc::MachineFunctionProperty Property :
       {rcc::MachineFunctionProperty::Legalized, rcc::MachineFunctionProperty::RegBankSelected,
        rcc::MachineFunctionProperty::Selected, rcc::MachineFunctionProperty::NoPHIs,
        rcc::MachineFunctionProperty::TiedOpsRewritten}) {
    Residual.Function->properties().set(Property);
  }
  Context.expect(!rcc::MachineVerifier::verify(*Residual.Function, Target),
                 "TiedOpsRewritten must reject residual REG_SEQUENCE/INSERT_SUBREG pseudos");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "SSA-exit TestTarget must be created");
  if (!Target)
    return Context.result();
  TestSSAInfo Info;
  testParallelPHIsAndPlacement(Context, *Target, Info);
  testCriticalEdgeAndPhysicalLiveIn(Context, *Target, Info);
  testUndefAndFailureAtomicity(Context, *Target, Info);
  testTwoAddressAndSubregisterPseudos(Context, *Target, Info);
  testVerifierAndPseudoFailures(Context, *Target);
  return Context.result();
}
