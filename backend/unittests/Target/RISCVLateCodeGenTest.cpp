// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineBlockPlacement.h"
#include "rcc/CodeGen/MachineBranchOptimization.h"
#include "rcc/CodeGen/MachineBranchRelaxation.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineCopyPropagation.h"
#include "rcc/CodeGen/MachineFallthrough.h"
#include "rcc/CodeGen/MachineFinalization.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineMemoryFolding.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineOptimizationPipeline.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachinePseudoExpansion.h"
#include "rcc/CodeGen/MachineShrinkWrapping.h"
#include "rcc/CodeGen/MachineStackSlotColoring.h"
#include "rcc/CodeGen/MachineTailDuplication.h"
#include "rcc/CodeGen/MachineTailMerge.h"
#include "rcc/CodeGen/PrologueEpilogueInsertion.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

[[nodiscard]] constexpr rcc::Opcode opcode(rcc::riscv::InstructionOpcode Value) noexcept {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

constexpr rcc::Opcode GrowingLatePseudoOpcode = rcc::Opcode::fromValue(10'000);
constexpr rcc::Opcode LateExpansionRealOpcode = rcc::Opcode::fromValue(10'001);
constexpr rcc::Opcode ShrinkingLatePseudoOpcode = rcc::Opcode::fromValue(10'002);

class GrowingLatePseudoInfo final : public rcc::TargetPseudoExpansionInfo {
public:
  [[nodiscard]] rcc::Expected<bool>
  expandPseudo(rcc::MachineInstr &Instruction, rcc::MachinePseudoExpansionPhase Phase,
               rcc::MachineFunction &Function,
               const rcc::TargetDescription &Target) const override {
    if (Phase != rcc::MachinePseudoExpansionPhase::TargetLate ||
        (Instruction.opcode() != GrowingLatePseudoOpcode &&
         Instruction.opcode() != ShrinkingLatePseudoOpcode)) {
      return false;
    }
    rcc::MachineBasicBlock *Block = Instruction.parent();
    if (Block == nullptr) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "late expansion test pseudo is detached");
    }
    if (Instruction.opcode() == GrowingLatePseudoOpcode) {
      rcc::MachineIRBuilder Builder(Function, Target);
      if (auto Point = Builder.setInsertPointBefore(Instruction); !Point)
        return std::unexpected(Point.error());
      for (std::size_t Index = 0; Index < 2; ++Index) {
        auto Pending = Builder.buildInstr(LateExpansionRealOpcode);
        if (!Pending)
          return std::unexpected(Pending.error());
        auto Built = Pending->build();
        if (!Built)
          return std::unexpected(Built.error());
      }
    }
    if (auto Removed = Block->removeInstruction(Instruction); !Removed)
      return std::unexpected(Removed.error());
    return true;
  }
};

class FixedSizeBranchInfo final : public rcc::TargetInstrInfo {
public:
  [[nodiscard]] rcc::Expected<rcc::MachineBranchAnalysis>
  analyzeBranch(rcc::MachineBasicBlock &, rcc::MachineFunction &,
                const rcc::TargetDescription &) const override {
    return rcc::MachineBranchAnalysis{};
  }

  [[nodiscard]] rcc::Expected<void>
  reverseBranchCondition(rcc::MachineInstr &, rcc::MachineBasicBlock &, rcc::MachineFunction &,
                         const rcc::TargetDescription &) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported, "test target has no branches");
  }

  [[nodiscard]] rcc::Expected<void> removeBranch(rcc::MachineInstr &, rcc::MachineFunction &,
                                                 const rcc::TargetDescription &) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported, "test target has no branches");
  }

  [[nodiscard]] rcc::Expected<bool>
  rewriteBranchTarget(rcc::MachineBasicBlock &, rcc::MachineBasicBlock &, rcc::MachineBasicBlock &,
                      rcc::MachineFunction &, const rcc::TargetDescription &) const override {
    return false;
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *>
  insertUnconditionalBranch(rcc::MachineBasicBlock &, rcc::MachineBasicBlock &,
                            rcc::MachineFunction &, const rcc::TargetDescription &) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported, "test target has no branches");
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
};

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createLateExpansionSizeTarget() {
  rcc::InstructionDescriptor Pseudo;
  Pseudo.InstructionOpcode = GrowingLatePseudoOpcode;
  Pseudo.Name = "TEST_GROWING_LATE_PSEUDO";
  Pseudo.Flags = {rcc::InstructionFlag::Pseudo, rcc::InstructionFlag::ProducesCode};
  Pseudo.Owner = rcc::PseudoOwner::TargetLateExpansion;
  Pseudo.Deadline = rcc::PipelineStage::Emission;

  rcc::InstructionDescriptor ShrinkingPseudo;
  ShrinkingPseudo.InstructionOpcode = ShrinkingLatePseudoOpcode;
  ShrinkingPseudo.Name = "TEST_SHRINKING_LATE_PSEUDO";
  ShrinkingPseudo.Flags = {rcc::InstructionFlag::Pseudo, rcc::InstructionFlag::ProducesCode};
  ShrinkingPseudo.Owner = rcc::PseudoOwner::TargetLateExpansion;
  ShrinkingPseudo.Deadline = rcc::PipelineStage::Emission;

  rcc::InstructionDescriptor Real;
  Real.InstructionOpcode = LateExpansionRealOpcode;
  Real.Name = "TEST_LATE_REAL";
  Real.Flags = {rcc::InstructionFlag::ProducesCode};

  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Pseudo));
  Descriptors.push_back(std::move(Real));
  Descriptors.push_back(std::move(ShrinkingPseudo));
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  auto Registers = rcc::riscv::createRegisterInfo();
  if (!Registers)
    return std::unexpected(Registers.error());
  return rcc::TargetDescription::create(std::move(*Instructions), std::move(*Registers));
}

[[nodiscard]] rcc::Register physical(rcc::riscv::PhysicalRegisterID ID) {
  return rcc::Register::fromPhysical(*rcc::riscv::physicalRegister(ID));
}

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
makeFunction(TestContext &Context, std::string Name, std::uint64_t StableID) {
  auto Function = rcc::MachineFunction::create(std::move(Name),
                                               rcc::MachineFunctionStableID::fromValue(StableID));
  Context.expect(Function.has_value(), "late-codegen function creation must succeed");
  return Function ? std::move(*Function) : nullptr;
}

[[nodiscard]] rcc::MachineBasicBlock *appendBlock(TestContext &Context,
                                                  rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  Context.expect(Block.has_value() && Function.appendBlock(**Block).has_value(),
                 "late-codegen block creation must succeed");
  return Block.value_or(nullptr);
}

void setSelectedProperties(rcc::MachineFunction &Function) {
  for (const rcc::MachineFunctionProperty Property : {
           rcc::MachineFunctionProperty::Legalized,
           rcc::MachineFunctionProperty::RegBankSelected,
           rcc::MachineFunctionProperty::Selected,
           rcc::MachineFunctionProperty::NoPHIs,
           rcc::MachineFunctionProperty::TiedOpsRewritten,
       }) {
    Function.properties().set(Property);
  }
}

void setPostRAProperties(rcc::MachineFunction &Function) {
  setSelectedProperties(Function);
  Function.properties().set(rcc::MachineFunctionProperty::TracksLiveness);
  Function.properties().set(rcc::MachineFunctionProperty::NoVRegs);
}

void setRelaxationProperties(TestContext &Context, rcc::MachineFunction &Function,
                             const rcc::TargetDescription &Target) {
  setPostRAProperties(Function);
  auto StackAlignment = rcc::Alignment::create(16);
  Context.expect(StackAlignment && Function.frameInfo().freeze(0, *StackAlignment).has_value(),
                 "relaxation fixture frame must be frozen");
  Function.properties().set(rcc::MachineFunctionProperty::FrameFinalized);
  Function.properties().set(rcc::MachineFunctionProperty::NoFrameIndices);
  Function.properties().set(rcc::MachineFunctionProperty::NoExecutablePseudos);
  auto Liveness = rcc::rebuildMachineLiveness(Function, Target);
  Context.expect(Liveness.has_value(), "relaxation fixture liveness must be rebuilt");
}

[[nodiscard]] rcc::MachineInstr *buildADDI(TestContext &Context, rcc::MachineIRBuilder &Builder,
                                           rcc::Register Destination, rcc::Register Source,
                                           std::int64_t Immediate) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ADDI));
  if (!Pending || !Pending->addDef(Destination) || !Pending->addUse(Source) ||
      !Pending->addImmediate(Immediate)) {
    Context.expect(false, "late-codegen ADDI construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "late-codegen ADDI insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] bool constrainGPR64(TestContext &Context, rcc::MachineFunction &Function,
                                  rcc::Register Reg) {
  const auto Bank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto Class =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  const bool Valid = Function.registerInfo().setRegisterBank(Reg, Bank).has_value() &&
                     Function.registerInfo().setRegisterClass(Reg, Class).has_value();
  Context.expect(Valid, "late-codegen GPR64 constraint must succeed");
  return Valid;
}

[[nodiscard]] rcc::Expected<const rcc::MachineMemOperand *>
createStackMemory(rcc::MachineFunction &Function, rcc::FrameIndex Slot,
                  rcc::MemoryAccessFlag Access) {
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  if (!Size)
    return std::unexpected(Size.error());
  if (!Align)
    return std::unexpected(Align.error());
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = rcc::MemoryBase::frameIndex(Slot);
  return Function.createMemOperand(std::move(Description));
}

[[nodiscard]] rcc::MachineInstr *buildStackAccess(TestContext &Context,
                                                  rcc::MachineIRBuilder &Builder,
                                                  rcc::riscv::InstructionOpcode Opcode,
                                                  rcc::Register Reg, rcc::FrameIndex Slot,
                                                  rcc::MemoryAccessFlag Access) {
  auto Memory = createStackMemory(Builder.function(), Slot, Access);
  auto Pending = Builder.buildInstr(opcode(Opcode));
  const bool IsLoad = Access == rcc::MemoryAccessFlag::Load;
  if (!Memory || !Pending ||
      !(IsLoad ? Pending->addDef(Reg).has_value() : Pending->addUse(Reg).has_value()) ||
      !Pending->addFrameIndex(Slot) || !Pending->addImmediate(0) ||
      !Pending->addMemOperand(**Memory)) {
    Context.expect(false, "late-codegen stack access construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "late-codegen stack access insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] rcc::MachineInstr *buildConditional(TestContext &Context,
                                                  rcc::MachineIRBuilder &Builder,
                                                  rcc::riscv::InstructionOpcode Opcode,
                                                  rcc::MachineBasicBlock &Target) {
  auto Pending = Builder.buildInstr(opcode(Opcode));
  if (!Pending || !Pending->addUse(physical(rcc::riscv::PhysicalRegisterID::X10)) ||
      !Pending->addUse(physical(rcc::riscv::PhysicalRegisterID::X0)) ||
      !Pending->addBasicBlock(Target)) {
    Context.expect(false, "late-codegen conditional branch construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "late-codegen conditional branch insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] rcc::MachineInstr *
buildJAL(TestContext &Context, rcc::MachineIRBuilder &Builder, rcc::MachineBasicBlock &Target,
         rcc::riscv::PhysicalRegisterID Destination = rcc::riscv::PhysicalRegisterID::X0) {
  auto Pending = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::JAL));
  if (!Pending || !Pending->addDef(physical(Destination)) || !Pending->addBasicBlock(Target)) {
    Context.expect(false, "late-codegen JAL construction must succeed");
    return nullptr;
  }
  auto Built = Pending->build();
  Context.expect(Built.has_value(), "late-codegen JAL insertion must succeed");
  return Built.value_or(nullptr);
}

[[nodiscard]] rcc::MachineBasicBlock *branchTarget(rcc::MachineInstr &Instruction) {
  for (std::size_t Index = 0; Index < Instruction.operandCount(); ++Index) {
    rcc::MachineOperand *Operand = Instruction.operandAt(Index);
    const auto *Block = Operand == nullptr ? nullptr : Operand->getIf<rcc::BasicBlockOperand>();
    if (Block != nullptr)
      return Block->Block;
  }
  return nullptr;
}

void testBlockPlacement(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "block_placement", 1);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Cold = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Hot = appendBlock(Context, *Function);
  if (Entry == nullptr || Cold == nullptr || Hot == nullptr)
    return;
  auto ColdProbability = rcc::BranchProbability::fromFraction(1, 10);
  auto HotProbability = rcc::BranchProbability::fromFraction(9, 10);
  Context.expect(ColdProbability && HotProbability,
                 "placement probabilities must be representable");
  if (!ColdProbability || !HotProbability)
    return;
  auto CFG = Function->cfgUpdater();
  Context.expect(CFG.addSuccessor(*Entry, *Cold, *ColdProbability).has_value() &&
                     CFG.addSuccessor(*Entry, *Hot, *HotProbability).has_value(),
                 "placement CFG construction must succeed");
  setSelectedProperties(*Function);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineBlockPlacementPass Pass;
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "block placement failure: " << Result.error().message() << '\n';
  Context.expect(Result && Result->Changed && Entry->next() == Hot && Hot->next() == Cold,
                 "block placement must put the highest-probability successor on the trace");
}

void testFallthroughNormalization(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "fallthrough", 2);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *False = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *True = appendBlock(Context, *Function);
  if (Entry == nullptr || False == nullptr || True == nullptr)
    return;
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function->cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(*Entry, *True, *Half).has_value() &&
                     CFG.addSuccessor(*Entry, *False, *Half).has_value(),
                 "fallthrough CFG construction must succeed");
  rcc::MachineIRBuilder Builder(*Function, Target);
  Context.expect(Builder.setInsertPoint(*Entry).has_value(),
                 "fallthrough insertion point must be established");
  rcc::MachineInstr *Conditional =
      buildConditional(Context, Builder, rcc::riscv::InstructionOpcode::BEQ, *True);
  rcc::MachineInstr *Jump = buildJAL(Context, Builder, *False);
  if (Conditional == nullptr || Jump == nullptr)
    return;
  setPostRAProperties(*Function);
  auto Liveness = rcc::rebuildMachineLiveness(*Function, Target);
  Context.expect(Liveness.has_value(), "fallthrough fixture liveness must be rebuilt");
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineFallthroughPass Pass(rcc::riscv::branchInfo());
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "fallthrough normalization failure: " << Result.error().message() << '\n';
  Context.expect(Result && Result->Changed && Conditional->next() == nullptr &&
                     Jump->parent() == nullptr,
                 "fallthrough normalization must remove a jump to the next block");
}

void testFallthroughMaterialization(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "fallthrough_materialization", 8);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Padding = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *True = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *False = appendBlock(Context, *Function);
  if (Entry == nullptr || Padding == nullptr || True == nullptr || False == nullptr)
    return;
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function->cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(*Entry, *True, *Half).has_value() &&
                     CFG.addSuccessor(*Entry, *False, *Half).has_value(),
                 "materialized fallthrough CFG construction must succeed");
  rcc::MachineIRBuilder Builder(*Function, Target);
  Context.expect(Builder.setInsertPoint(*Entry).has_value(),
                 "materialized fallthrough insertion point must be established");
  rcc::MachineInstr *Conditional =
      buildConditional(Context, Builder, rcc::riscv::InstructionOpcode::BNE, *True);
  if (Conditional == nullptr)
    return;
  setPostRAProperties(*Function);
  Context.expect(rcc::rebuildMachineLiveness(*Function, Target).has_value(),
                 "materialized fallthrough liveness must be rebuilt");
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineFallthroughPass Pass(rcc::riscv::branchInfo());
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "fallthrough materialization failure: " << Result.error().message() << '\n';
  Context.expect(Result && Result->Changed && Conditional->next() != nullptr &&
                     Conditional->next()->opcode() == opcode(rcc::riscv::InstructionOpcode::JAL) &&
                     branchTarget(*Conditional->next()) == False,
                 "fallthrough normalization must materialize a displaced false edge");
}

void testBranchOptimization(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "branch_optimization", 9);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Forward = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Exit = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Dead = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *AddressTaken = appendBlock(Context, *Function);
  if (Entry == nullptr || Forward == nullptr || Exit == nullptr || Dead == nullptr ||
      AddressTaken == nullptr) {
    return;
  }
  AddressTaken->attributes().set(rcc::MachineBasicBlockAttribute::MachineBlockAddressTaken);
  auto CFG = Function->cfgUpdater();
  Context.expect(CFG.addSuccessor(*Entry, *Forward, rcc::BranchProbability::one()).has_value() &&
                     CFG.addSuccessor(*Forward, *Exit, rcc::BranchProbability::one()).has_value(),
                 "branch-optimization CFG construction must succeed");
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Entry) || buildJAL(Context, Builder, *Forward) == nullptr ||
      !Builder.setInsertPoint(*Forward) || buildJAL(Context, Builder, *Exit) == nullptr ||
      !Builder.setInsertPoint(*Dead) ||
      buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X12),
                physical(rcc::riscv::PhysicalRegisterID::X11), 1) == nullptr ||
      !Builder.setInsertPoint(*AddressTaken) ||
      buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X13),
                physical(rcc::riscv::PhysicalRegisterID::X11), 2) == nullptr) {
    return;
  }
  setSelectedProperties(*Function);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineBranchOptimizationPass Pass(rcc::riscv::branchInfo());
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "branch optimization failure: " << Result.error().message() << '\n';
  Context.expect(Result && Result->Changed && !Forward->isLinked() && !Dead->isLinked() &&
                     AddressTaken->isLinked() && Entry->successors().size() == 1 &&
                     Entry->successors().front().Block == Exit &&
                     branchTarget(*Entry->lastInstruction()) == Exit,
                 "branch optimization must thread empty blocks and delete unreachable code");

  auto ConstantFunction = makeFunction(Context, "constant_branch", 10);
  if (!ConstantFunction)
    return;
  rcc::MachineBasicBlock *ConstantEntry = appendBlock(Context, *ConstantFunction);
  rcc::MachineBasicBlock *Taken = appendBlock(Context, *ConstantFunction);
  rcc::MachineBasicBlock *NotTaken = appendBlock(Context, *ConstantFunction);
  if (ConstantEntry == nullptr || Taken == nullptr || NotTaken == nullptr)
    return;
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto ConstantCFG = ConstantFunction->cfgUpdater();
  Context.expect(Half && ConstantCFG.addSuccessor(*ConstantEntry, *Taken, *Half).has_value() &&
                     ConstantCFG.addSuccessor(*ConstantEntry, *NotTaken, *Half).has_value(),
                 "constant-branch CFG construction must succeed");
  rcc::MachineIRBuilder ConstantBuilder(*ConstantFunction, Target);
  if (!ConstantBuilder.setInsertPoint(*ConstantEntry))
    return;
  auto Pending = ConstantBuilder.buildInstr(opcode(rcc::riscv::InstructionOpcode::BEQ));
  if (!Pending || !Pending->addUse(physical(rcc::riscv::PhysicalRegisterID::X0)) ||
      !Pending->addUse(physical(rcc::riscv::PhysicalRegisterID::X0)) ||
      !Pending->addBasicBlock(*Taken) || !Pending->build() ||
      buildJAL(Context, ConstantBuilder, *NotTaken) == nullptr) {
    Context.expect(false, "constant branch construction must succeed");
    return;
  }
  setSelectedProperties(*ConstantFunction);
  rcc::MachineAnalysisManager ConstantAnalyses;
  auto ConstantResult = Pass.run(*ConstantFunction, Target, ConstantAnalyses);
  if (!ConstantResult)
    std::cerr << "constant branch folding failure: " << ConstantResult.error().message() << '\n';
  Context.expect(
      ConstantResult && ConstantResult->Changed && ConstantEntry->successors().size() == 1 &&
          ConstantEntry->successors().front().Block == Taken &&
          branchTarget(*ConstantEntry->lastInstruction()) == Taken && !NotTaken->isLinked(),
      "branch optimization must fold an always-true RV64GC branch");
}

void testTailDuplication(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "tail_duplication", 11);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Left = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Right = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Tail = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Exit = appendBlock(Context, *Function);
  if (Entry == nullptr || Left == nullptr || Right == nullptr || Tail == nullptr ||
      Exit == nullptr) {
    return;
  }
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function->cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(*Entry, *Left, *Half).has_value() &&
                     CFG.addSuccessor(*Entry, *Right, *Half).has_value() &&
                     CFG.addSuccessor(*Left, *Tail, rcc::BranchProbability::one()).has_value() &&
                     CFG.addSuccessor(*Right, *Tail, rcc::BranchProbability::one()).has_value() &&
                     CFG.addSuccessor(*Tail, *Exit, rcc::BranchProbability::one()).has_value(),
                 "tail-duplication CFG construction must succeed");
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Entry) ||
      buildConditional(Context, Builder, rcc::riscv::InstructionOpcode::BNE, *Left) == nullptr ||
      buildJAL(Context, Builder, *Right) == nullptr || !Builder.setInsertPoint(*Left) ||
      buildJAL(Context, Builder, *Tail) == nullptr || !Builder.setInsertPoint(*Right) ||
      buildJAL(Context, Builder, *Tail) == nullptr || !Builder.setInsertPoint(*Tail) ||
      buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X12),
                physical(rcc::riscv::PhysicalRegisterID::X11), 1) == nullptr ||
      buildJAL(Context, Builder, *Exit) == nullptr) {
    return;
  }
  setRelaxationProperties(Context, *Function, Target);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineTailDuplicationPass Pass(rcc::riscv::branchInfo());
  Tail->attributes().set(rcc::MachineBasicBlockAttribute::MachineBlockAddressTaken);
  auto Protected = Pass.run(*Function, Target, Analyses);
  Context.expect(Protected && !Protected->Changed && Tail->isLinked(),
                 "tail duplication must preserve an address-taken tail");
  Tail->attributes().clear(rcc::MachineBasicBlockAttribute::MachineBlockAddressTaken);
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "tail duplication failure: " << Result.error().message() << '\n';
  Context.expect(
      Result && Result->Changed && !Tail->isLinked() && Left->instructionCount() == 2 &&
          Right->instructionCount() == 2 &&
          Left->firstInstruction()->opcode() == opcode(rcc::riscv::InstructionOpcode::ADDI) &&
          Right->firstInstruction()->opcode() == opcode(rcc::riscv::InstructionOpcode::ADDI) &&
          branchTarget(*Left->lastInstruction()) == Exit &&
          branchTarget(*Right->lastInstruction()) == Exit &&
          rcc::verifyTrackedMachineLiveness(*Function, Target).has_value(),
      "tail duplication must clone a small shared tail and repair liveness");
}

void testTailMerge(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "tail_merge", 12);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *First = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Second = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Exit = appendBlock(Context, *Function);
  if (Entry == nullptr || First == nullptr || Second == nullptr || Exit == nullptr)
    return;
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function->cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(*Entry, *First, *Half).has_value() &&
                     CFG.addSuccessor(*Entry, *Second, *Half).has_value() &&
                     CFG.addSuccessor(*First, *Exit, rcc::BranchProbability::one()).has_value() &&
                     CFG.addSuccessor(*Second, *Exit, rcc::BranchProbability::one()).has_value(),
                 "tail-merge CFG construction must succeed");
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Entry) ||
      buildConditional(Context, Builder, rcc::riscv::InstructionOpcode::BNE, *First) == nullptr ||
      buildJAL(Context, Builder, *Second) == nullptr || !Builder.setInsertPoint(*First) ||
      buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X12),
                physical(rcc::riscv::PhysicalRegisterID::X11), 7) == nullptr ||
      buildJAL(Context, Builder, *Exit) == nullptr || !Builder.setInsertPoint(*Second) ||
      buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X12),
                physical(rcc::riscv::PhysicalRegisterID::X11), 7) == nullptr ||
      buildJAL(Context, Builder, *Exit) == nullptr) {
    return;
  }
  setRelaxationProperties(Context, *Function, Target);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineTailMergePass Pass(rcc::riscv::branchInfo());
  Second->attributes().set(rcc::MachineBasicBlockAttribute::MachineBlockAddressTaken);
  auto Protected = Pass.run(*Function, Target, Analyses);
  Context.expect(Protected && !Protected->Changed && Second->isLinked(),
                 "tail merge must preserve an address-taken duplicate block");
  Second->attributes().clear(rcc::MachineBasicBlockAttribute::MachineBlockAddressTaken);
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "tail merge failure: " << Result.error().message() << '\n';
  Context.expect(Result && Result->Changed && !Second->isLinked() &&
                     Entry->successors().size() == 1 &&
                     Entry->successors().front().Block == First &&
                     branchTarget(*Entry->firstInstruction()) == First &&
                     branchTarget(*Entry->lastInstruction()) == First &&
                     rcc::verifyTrackedMachineLiveness(*Function, Target).has_value(),
                 "tail merge must redirect identical tails and repair liveness");
}

void testCopyPropagation(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "pre_ra_copy_propagation", 13);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  auto I64 = rcc::LLT::makeInteger(64);
  if (Block == nullptr || !I64)
    return;
  auto Source = Function->registerInfo().createVirtualRegister(*I64);
  auto CopyResult = Function->registerInfo().createVirtualRegister(*I64);
  auto ResultReg = Function->registerInfo().createVirtualRegister(*I64);
  if (!Source || !CopyResult || !ResultReg || !constrainGPR64(Context, *Function, *Source) ||
      !constrainGPR64(Context, *Function, *CopyResult) ||
      !constrainGPR64(Context, *Function, *ResultReg)) {
    return;
  }
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Block))
    return;
  auto Copy = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!Copy || !Copy->addDef(*CopyResult) || !Copy->addUse(*Source) || !Copy->build())
    return;
  rcc::MachineInstr *Add = buildADDI(Context, Builder, *ResultReg, *CopyResult, 3);
  if (Add == nullptr)
    return;
  setSelectedProperties(*Function);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineCopyPropagationPass PreRA(rcc::riscv::ssaInfo(),
                                        rcc::MachineCopyPropagationStage::PreRegisterAllocation);
  auto PreResult = PreRA.run(*Function, Target, Analyses);
  if (!PreResult)
    std::cerr << "pre-RA COPY propagation failure: " << PreResult.error().message() << '\n';
  Context.expect(PreResult && PreResult->Changed && Block->instructionCount() == 1 &&
                     Add->operandAt(1)->registerValue() == *Source &&
                     !Function->registerInfo().isKnownVirtualRegister(*CopyResult),
                 "pre-RA COPY propagation must replace uses and retire the COPY result");

  auto PostFunction = makeFunction(Context, "post_ra_copy_propagation", 14);
  if (!PostFunction)
    return;
  rcc::MachineBasicBlock *PostBlock = appendBlock(Context, *PostFunction);
  if (PostBlock == nullptr)
    return;
  rcc::MachineIRBuilder PostBuilder(*PostFunction, Target);
  if (!PostBuilder.setInsertPoint(*PostBlock))
    return;
  auto PhysicalCopy = PostBuilder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!PhysicalCopy || !PhysicalCopy->addDef(physical(rcc::riscv::PhysicalRegisterID::X11)) ||
      !PhysicalCopy->addUse(physical(rcc::riscv::PhysicalRegisterID::X10)) ||
      !PhysicalCopy->build()) {
    return;
  }
  rcc::MachineInstr *PhysicalAdd =
      buildADDI(Context, PostBuilder, physical(rcc::riscv::PhysicalRegisterID::X12),
                physical(rcc::riscv::PhysicalRegisterID::X11), 5);
  rcc::MachineInstr *SourceClobber =
      buildADDI(Context, PostBuilder, physical(rcc::riscv::PhysicalRegisterID::X10),
                physical(rcc::riscv::PhysicalRegisterID::X0), 9);
  rcc::MachineInstr *AfterClobber =
      buildADDI(Context, PostBuilder, physical(rcc::riscv::PhysicalRegisterID::X13),
                physical(rcc::riscv::PhysicalRegisterID::X11), 6);
  if (PhysicalAdd == nullptr || SourceClobber == nullptr || AfterClobber == nullptr)
    return;
  setPostRAProperties(*PostFunction);
  Context.expect(rcc::rebuildMachineLiveness(*PostFunction, Target).has_value(),
                 "post-RA COPY propagation liveness must be rebuilt");
  rcc::MachineAnalysisManager PostAnalyses;
  rcc::MachineCopyPropagationPass PostRA(rcc::riscv::ssaInfo(),
                                         rcc::MachineCopyPropagationStage::PostRegisterAllocation);
  auto PostResult = PostRA.run(*PostFunction, Target, PostAnalyses);
  if (!PostResult)
    std::cerr << "post-RA COPY propagation failure: " << PostResult.error().message() << '\n';
  Context.expect(PostResult && PostResult->Changed &&
                     PhysicalAdd->operandAt(1)->registerValue() ==
                         physical(rcc::riscv::PhysicalRegisterID::X10) &&
                     AfterClobber->operandAt(1)->registerValue() ==
                         physical(rcc::riscv::PhysicalRegisterID::X11) &&
                     rcc::verifyTrackedMachineLiveness(*PostFunction, Target).has_value(),
                 "post-RA COPY propagation must stop at clobbers and repair liveness");
}

void testStackSlotColoring(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "stack_slot_coloring", 15);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  auto Align = rcc::Alignment::create(8);
  if (Block == nullptr || !Align)
    return;
  rcc::StackObjectDesc Description;
  Description.Kind = rcc::StackObjectKind::SpillSlot;
  Description.Size = 8;
  Description.Align = *Align;
  auto FirstSlot = Function->frameInfo().createObject(Description);
  auto SecondSlot = Function->frameInfo().createObject(Description);
  if (!FirstSlot || !SecondSlot)
    return;
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Block) ||
      buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X10), *FirstSlot,
                       rcc::MemoryAccessFlag::Store) == nullptr ||
      buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::LD,
                       physical(rcc::riscv::PhysicalRegisterID::X11), *FirstSlot,
                       rcc::MemoryAccessFlag::Load) == nullptr ||
      buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X12), *SecondSlot,
                       rcc::MemoryAccessFlag::Store) == nullptr ||
      buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::LD,
                       physical(rcc::riscv::PhysicalRegisterID::X13), *SecondSlot,
                       rcc::MemoryAccessFlag::Load) == nullptr) {
    return;
  }
  setPostRAProperties(*Function);
  Context.expect(rcc::rebuildMachineLiveness(*Function, Target).has_value(),
                 "stack-slot coloring liveness must be rebuilt");
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineStackSlotColoringPass Pass;
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "stack-slot coloring failure: " << Result.error().message() << '\n';
  const rcc::StackObject *Colored = Function->frameInfo().object(*SecondSlot);
  rcc::MachineInstr *SecondStore = Block->firstInstruction();
  SecondStore = SecondStore == nullptr ? nullptr : SecondStore->next();
  SecondStore = SecondStore == nullptr ? nullptr : SecondStore->next();
  const auto *Frame =
      SecondStore == nullptr ? nullptr : SecondStore->operandAt(1)->getIf<rcc::FrameIndexOperand>();
  Context.expect(
      Result && Result->Changed && Colored != nullptr && Colored->description() != nullptr &&
          Colored->description()->IsDead && Frame != nullptr && Frame->Index == *FirstSlot &&
          SecondStore->memOperands().front()->description().Base.frameIndex() == *FirstSlot,
      "stack-slot coloring must coalesce disjoint spills and rewrite MMOs");

  auto OverlapFunction = makeFunction(Context, "stack_slot_overlap", 17);
  if (!OverlapFunction)
    return;
  rcc::MachineBasicBlock *OverlapBlock = appendBlock(Context, *OverlapFunction);
  auto Left = OverlapFunction->frameInfo().createObject(Description);
  auto Right = OverlapFunction->frameInfo().createObject(Description);
  if (OverlapBlock == nullptr || !Left || !Right)
    return;
  rcc::MachineIRBuilder OverlapBuilder(*OverlapFunction, Target);
  if (!OverlapBuilder.setInsertPoint(*OverlapBlock) ||
      buildStackAccess(Context, OverlapBuilder, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X10), *Left,
                       rcc::MemoryAccessFlag::Store) == nullptr ||
      buildStackAccess(Context, OverlapBuilder, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X11), *Right,
                       rcc::MemoryAccessFlag::Store) == nullptr ||
      buildStackAccess(Context, OverlapBuilder, rcc::riscv::InstructionOpcode::LD,
                       physical(rcc::riscv::PhysicalRegisterID::X12), *Left,
                       rcc::MemoryAccessFlag::Load) == nullptr ||
      buildStackAccess(Context, OverlapBuilder, rcc::riscv::InstructionOpcode::LD,
                       physical(rcc::riscv::PhysicalRegisterID::X13), *Right,
                       rcc::MemoryAccessFlag::Load) == nullptr) {
    return;
  }
  setPostRAProperties(*OverlapFunction);
  Context.expect(rcc::rebuildMachineLiveness(*OverlapFunction, Target).has_value(),
                 "overlapping stack-slot fixture liveness must be rebuilt");
  auto OverlapResult = Pass.run(*OverlapFunction, Target, Analyses);
  const rcc::StackObject *RightObject = OverlapFunction->frameInfo().object(*Right);
  Context.expect(OverlapResult && !OverlapResult->Changed && RightObject != nullptr &&
                     RightObject->description() != nullptr && !RightObject->description()->IsDead,
                 "stack-slot coloring must preserve overlapping spill lifetimes");
}

void testRISCVMemoryFoldingBoundary(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "riscv_memory_folding", 16);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  auto I64 = rcc::LLT::makeInteger(64);
  auto Align = rcc::Alignment::create(8);
  if (Block == nullptr || !I64 || !Align)
    return;
  auto Loaded = Function->registerInfo().createVirtualRegister(*I64);
  auto ResultReg = Function->registerInfo().createVirtualRegister(*I64);
  if (!Loaded || !ResultReg || !constrainGPR64(Context, *Function, *Loaded) ||
      !constrainGPR64(Context, *Function, *ResultReg)) {
    return;
  }
  rcc::StackObjectDesc Description;
  Description.Size = 8;
  Description.Align = *Align;
  auto Slot = Function->frameInfo().createObject(std::move(Description));
  if (!Slot)
    return;
  rcc::MachineIRBuilder Builder(*Function, Target);
  rcc::MachineInstr *Load = nullptr;
  rcc::MachineInstr *Consumer = nullptr;
  if (Builder.setInsertPoint(*Block)) {
    Load = buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::LD, *Loaded, *Slot,
                            rcc::MemoryAccessFlag::Load);
    Consumer = buildADDI(Context, Builder, *ResultReg, *Loaded, 1);
  }
  if (Load == nullptr || Consumer == nullptr)
    return;
  setSelectedProperties(*Function);
  Function->properties().set(rcc::MachineFunctionProperty::IsSSA);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineMemoryFoldingPass Pass(rcc::riscv::memoryFoldingInfo());
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "RISC-V memory folding boundary failure: " << Result.error().message() << '\n';
  Context.expect(Result && !Result->Changed && Load->parent() == Block &&
                     Load->memOperands().size() == 1 && Consumer->parent() == Block,
                 "RV64GC must explicitly decline unsupported folded-memory ALU forms");
}

void testShrinkWrappedFrameRegion(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "shrink_wrapped_frame", 20);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Cold = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Hot = appendBlock(Context, *Function);
  auto Align = rcc::Alignment::create(8);
  auto ColdProbability = rcc::BranchProbability::fromFraction(1, 10);
  auto HotProbability = rcc::BranchProbability::fromFraction(9, 10);
  if (Entry == nullptr || Cold == nullptr || Hot == nullptr || !Align || !ColdProbability ||
      !HotProbability) {
    return;
  }
  auto CFG = Function->cfgUpdater();
  if (!CFG.addSuccessor(*Entry, *Cold, *ColdProbability) ||
      !CFG.addSuccessor(*Entry, *Hot, *HotProbability)) {
    return;
  }
  rcc::StackObjectDesc SpillDescription;
  SpillDescription.Kind = rcc::StackObjectKind::SpillSlot;
  SpillDescription.Size = 8;
  SpillDescription.Align = *Align;
  auto Spill = Function->frameInfo().createObject(SpillDescription);
  if (!Spill)
    return;

  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Cold) ||
      buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X10), *Spill,
                       rcc::MemoryAccessFlag::Store) == nullptr) {
    return;
  }
  auto ColdReturn = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoRET));
  if (!ColdReturn || !ColdReturn->addImplicitUse(physical(rcc::riscv::PhysicalRegisterID::X10)) ||
      !ColdReturn->build() || !Builder.setInsertPoint(*Hot)) {
    return;
  }
  auto HotReturn = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoRET));
  if (!HotReturn || !HotReturn->addImplicitUse(physical(rcc::riscv::PhysicalRegisterID::X10)) ||
      !HotReturn->build()) {
    return;
  }

  setPostRAProperties(*Function);
  Context.expect(rcc::rebuildMachineLiveness(*Function, Target).has_value(),
                 "shrink-wrapping fixture liveness must be rebuilt");
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineShrinkWrappingPass Shrink{rcc::riscv::frameLowering()};
  auto Shrunk = Shrink.run(*Function, Target, Analyses);
  Context.expect(Shrunk && Shrunk->Changed &&
                     Function->frameInfo().state().SavePoints.size() == 1 &&
                     Function->frameInfo().state().SavePoints.front().Block == Cold->stableID() &&
                     Function->frameInfo().state().RestorePoints.size() == 1 &&
                     Function->frameInfo().state().RestorePoints.front().Block == Cold->stableID(),
                 "shrink wrapping must select the cold frame region only");
  if (!Shrunk)
    return;

  rcc::MachinePassManager FramePipeline;
  Context.expect(FramePipeline
                     .addPass(std::make_unique<rcc::PrologueEpilogueInsertionPass>(
                         rcc::riscv::frameLowering()))
                     .has_value(),
                 "shrink-wrapped PEI pass must register");
  auto Lowered = FramePipeline.run(*Function, Target, Analyses);
  if (!Lowered)
    std::cerr << "shrink-wrapped PEI failure: " << Lowered.error().message() << '\n';
  const bool EntryHasFrameCode = std::ranges::any_of(
      Function->allInstructions(), [Entry](const rcc::MachineInstr *Instruction) {
        return Instruction != nullptr && Instruction->parent() == Entry &&
               (Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup) ||
                Instruction->flags().has(rcc::MachineInstrFlag::FrameDestroy));
      });
  const bool ColdHasSetup = std::ranges::any_of(
      Function->allInstructions(), [Cold](const rcc::MachineInstr *Instruction) {
        return Instruction != nullptr && Instruction->parent() == Cold &&
               Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup);
      });
  const bool ColdHasDestroy = std::ranges::any_of(
      Function->allInstructions(), [Cold](const rcc::MachineInstr *Instruction) {
        return Instruction != nullptr && Instruction->parent() == Cold &&
               Instruction->flags().has(rcc::MachineInstrFlag::FrameDestroy);
      });
  Context.expect(
      Lowered && !EntryHasFrameCode && ColdHasSetup && ColdHasDestroy &&
          Function->properties().has(rcc::MachineFunctionProperty::FrameFinalized),
      "PEI must materialize a shrink-wrapped prologue and epilogue only in the frame region");

  rcc::MachineBlockPlacementPass Placement;
  auto Placed = Placement.run(*Function, Target, Analyses);
  Context.expect(Placed && Cold->next() == nullptr,
                 "block placement must keep a shrink-wrapped frame region as a layout suffix");
}

void testCompressedAndConditionalRelaxation(TestContext &Context,
                                            const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "branch_relaxation", 3);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Padding = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *Far = appendBlock(Context, *Function);
  if (Entry == nullptr || Padding == nullptr || Far == nullptr)
    return;
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = Function->cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(*Entry, *Far, *Half).has_value() &&
                     CFG.addSuccessor(*Entry, *Padding, *Half).has_value() &&
                     CFG.addSuccessor(*Padding, *Far, rcc::BranchProbability::one()).has_value(),
                 "relaxation CFG construction must succeed");
  rcc::MachineIRBuilder Builder(*Function, Target);
  Context.expect(Builder.setInsertPoint(*Entry).has_value(),
                 "relaxation entry insertion point must be established");
  rcc::MachineInstr *Conditional =
      buildConditional(Context, Builder, rcc::riscv::InstructionOpcode::BEQ, *Far);
  Context.expect(Builder.setInsertPoint(*Padding).has_value(),
                 "relaxation padding insertion point must be established");
  for (std::size_t Index = 0; Index < 1100; ++Index) {
    if (buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X0),
                  physical(rcc::riscv::PhysicalRegisterID::X0), 0) == nullptr) {
      return;
    }
  }
  (void)buildJAL(Context, Builder, *Far);
  if (Conditional == nullptr)
    return;
  setRelaxationProperties(Context, *Function, Target);
  rcc::MachineAnalysisManager Analyses;
  rcc::MachineBranchRelaxationPass Pass(rcc::riscv::branchInfo());
  auto Result = Pass.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "conditional relaxation failure: " << Result.error().message() << '\n';
  Context.expect(Result && Result->Changed &&
                     Conditional->opcode() == opcode(rcc::riscv::InstructionOpcode::BNE) &&
                     branchTarget(*Conditional) == Padding,
                 "out-of-range conditional branch must invert onto the local fallthrough");
  rcc::MachineInstr *LongJump = Conditional->next();
  Context.expect(LongJump != nullptr &&
                     LongJump->opcode() == opcode(rcc::riscv::InstructionOpcode::JAL) &&
                     branchTarget(*LongJump) == Far,
                 "conditional relaxation must insert a reachable long jump to the old target");

  auto CompressedFunction = makeFunction(Context, "compressed_relaxation", 4);
  if (!CompressedFunction)
    return;
  rcc::MachineBasicBlock *CompressedEntry = appendBlock(Context, *CompressedFunction);
  rcc::MachineBasicBlock *CompressedTarget = appendBlock(Context, *CompressedFunction);
  rcc::MachineIRBuilder CompressedBuilder(*CompressedFunction, Target);
  Context.expect(CompressedEntry != nullptr && CompressedTarget != nullptr &&
                     CompressedBuilder.setInsertPoint(*CompressedEntry).has_value(),
                 "compressed relaxation fixture must be valid");
  if (CompressedEntry == nullptr || CompressedTarget == nullptr)
    return;
  auto Pending = CompressedBuilder.buildInstr(opcode(rcc::riscv::InstructionOpcode::C_J));
  if (!Pending || !Pending->addBasicBlock(*CompressedTarget)) {
    Context.expect(false, "compressed jump construction must succeed");
    return;
  }
  auto Compressed = Pending->build();
  Context.expect(Compressed.has_value(), "compressed jump insertion must succeed");
  if (!Compressed)
    return;
  auto Expanded =
      rcc::riscv::branchInfo().relaxBranch(**Compressed, 4096, *CompressedFunction, Target);
  Context.expect(Expanded && *Expanded && CompressedEntry->firstInstruction() != nullptr &&
                     CompressedEntry->firstInstruction()->opcode() ==
                         opcode(rcc::riscv::InstructionOpcode::JAL) &&
                     (*Compressed)->parent() == nullptr,
                 "compressed jump relaxation must monotonically expand to JAL");
}

void testLongJALRelaxation(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "long_jal", 5);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = appendBlock(Context, *Function);
  rcc::MachineBasicBlock *TargetBlock = appendBlock(Context, *Function);
  if (Entry == nullptr || TargetBlock == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Function, Target);
  Context.expect(Builder.setInsertPoint(*Entry).has_value(),
                 "long JAL insertion point must be established");
  rcc::MachineInstr *JAL = buildJAL(Context, Builder, *TargetBlock);
  if (JAL == nullptr)
    return;
  auto X5 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X5);
  auto X6 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X6);
  Context.expect(X5 && X6 && TargetBlock->addLiveIn(*X5).has_value() &&
                     TargetBlock->addLiveIn(*X6).has_value() &&
                     JAL->setPreInstrSymbol(".Lpreserved_long_branch").has_value() &&
                     JAL->setPostInstrSymbol(".Lafter_long_branch").has_value(),
                 "long JAL live-in and annotation fixture must be valid");
  if (!X5 || !X6)
    return;
  auto TooFar = rcc::riscv::branchInfo().relaxBranch(*JAL, std::numeric_limits<std::int64_t>::max(),
                                                     *Function, Target);
  Context.expect(!TooFar && TooFar.error().code() == rcc::ErrorCode::BranchRelaxationFailed &&
                     JAL->parent() == Entry,
                 "long JAL relaxation must reject an unencodable PC-relative distance atomically");
  auto Relaxed =
      rcc::riscv::branchInfo().relaxBranch(*JAL, std::int64_t{2} * 1024 * 1024, *Function, Target);
  if (!Relaxed)
    std::cerr << "long JAL relaxation failure: " << Relaxed.error().message() << '\n';
  rcc::MachineInstr *High = Entry->firstInstruction();
  rcc::MachineInstr *Low = High == nullptr ? nullptr : High->next();
  const rcc::Register *Scratch = High == nullptr || High->operandAt(0) == nullptr
                                     ? nullptr
                                     : High->operandAt(0)->getIf<rcc::Register>();
  const std::optional<rcc::PhysicalRegister> ScratchPhysical =
      Scratch == nullptr ? std::nullopt : Scratch->physicalRegister();
  Context.expect(Relaxed && *Relaxed && High != nullptr && Low != nullptr &&
                     High->opcode() == opcode(rcc::riscv::InstructionOpcode::AUIPC) &&
                     Low->opcode() == opcode(rcc::riscv::InstructionOpcode::JALR) &&
                     ScratchPhysical && *ScratchPhysical != *X5 && *ScratchPhysical != *X6 &&
                     High->operandAt(1)->targetFlags() == 2 &&
                     Low->operandAt(2)->targetFlags() == 4 &&
                     High->flags().has(rcc::MachineInstrFlag::BundledWithSucc) &&
                     Low->flags().has(rcc::MachineInstrFlag::BundledWithPred) &&
                     High->preInstrSymbol() == ".Lpreserved_long_branch" &&
                     Low->postInstrSymbol() == ".Lafter_long_branch" && JAL->parent() == nullptr,
                 "out-of-range JAL must use a dead scratch and preserve its paired annotations");
}

void testPseudoPreparation(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "pseudo_preparation", 6);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  auto I64 = rcc::LLT::makeInteger(64);
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  if (Block == nullptr || !I64 || !F64)
    return;
  auto Source = Function->registerInfo().createVirtualRegister(*I64);
  auto Destination = Function->registerInfo().createVirtualRegister(*F64);
  const auto GPRBank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::GPR));
  const auto FPRBank =
      rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterBank::FPR));
  const auto GPRClass =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::GPR));
  const auto FPRClass =
      rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterClass::FPR64));
  Context.expect(Source && Destination &&
                     Function->registerInfo().setRegisterBank(*Source, GPRBank).has_value() &&
                     Function->registerInfo().setRegisterClass(*Source, GPRClass).has_value() &&
                     Function->registerInfo().setRegisterBank(*Destination, FPRBank).has_value() &&
                     Function->registerInfo().setRegisterClass(*Destination, FPRClass).has_value(),
                 "pseudo preparation virtual-register constraints must be valid");
  if (!Source || !Destination)
    return;
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Block))
    return;
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!Pending || !Pending->addDef(*Destination) || !Pending->addUse(*Source))
    return;
  auto Copy = Pending->build();
  if (!Copy)
    return;
  setSelectedProperties(*Function);
  rcc::MachineAnalysisManager Analyses;
  rcc::PreparePostRAPseudosPass Pass(rcc::riscv::pseudoExpansionInfo());
  auto Result = Pass.run(*Function, Target, Analyses);
  Context.expect(Result && Result->Changed && (*Copy)->operandAt(0)->targetFlags() == 16 &&
                     (*Copy)->operandAt(1)->targetFlags() == 16,
                 "pre-RA pseudo preparation must retain a 64-bit cross-bank COPY width");
}

void testPostRAPseudoExpansionAndFinalization(TestContext &Context,
                                              const rcc::TargetDescription &Target) {
  auto Function = makeFunction(Context, "post_ra_pseudos", 7);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Block))
    return;

  auto GPRCopy = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!GPRCopy || !GPRCopy->addDef(physical(rcc::riscv::PhysicalRegisterID::X11)) ||
      !GPRCopy->addUse(physical(rcc::riscv::PhysicalRegisterID::X10)) || !GPRCopy->build()) {
    Context.expect(false, "post-RA GPR copy construction must succeed");
    return;
  }
  auto CrossCopy = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::COPY));
  if (!CrossCopy ||
      !CrossCopy->addDef(physical(rcc::riscv::PhysicalRegisterID::F10_D), {}, {}, 16) ||
      !CrossCopy->addUse(physical(rcc::riscv::PhysicalRegisterID::X10), {}, {}, 16) ||
      !CrossCopy->build()) {
    Context.expect(false, "post-RA cross-bank copy construction must succeed");
    return;
  }
  auto ReadFFLAGS = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::ReadFFLAGS));
  if (!ReadFFLAGS || !ReadFFLAGS->addDef(physical(rcc::riscv::PhysicalRegisterID::X12)) ||
      !ReadFFLAGS->build()) {
    Context.expect(false, "ReadFFLAGS pseudo construction must succeed");
    return;
  }
  auto WriteFFLAGS = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::WriteFFLAGS));
  if (!WriteFFLAGS || !WriteFFLAGS->addUse(physical(rcc::riscv::PhysicalRegisterID::X12)) ||
      !WriteFFLAGS->build()) {
    Context.expect(false, "WriteFFLAGS pseudo construction must succeed");
    return;
  }
  auto Call = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoCALL));
  auto Preserved = rcc::riscv::createCallPreservedMask(rcc::riscv::CallingConvention::LP64D);
  if (!Call || !Call->addExternalSymbol("callee", 0, 1) || !Call->addRegisterMask(Preserved) ||
      !Call->addImplicitUse(physical(rcc::riscv::PhysicalRegisterID::X10)) || !Call->build()) {
    Context.expect(false, "post-RA call pseudo construction must succeed");
    return;
  }
  auto Return = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoRET));
  if (!Return || !Return->addImplicitUse(physical(rcc::riscv::PhysicalRegisterID::X10)) ||
      !Return->build()) {
    Context.expect(false, "post-RA return pseudo construction must succeed");
    return;
  }

  setRelaxationProperties(Context, *Function, Target);
  Function->properties().clear(rcc::MachineFunctionProperty::NoExecutablePseudos);
  rcc::MachineAnalysisManager Analyses;
  rcc::ExpandPostRAPseudosPass Expansion(rcc::riscv::pseudoExpansionInfo());
  auto Expanded = Expansion.run(*Function, Target, Analyses);
  if (!Expanded)
    std::cerr << "post-RA pseudo expansion failure: " << Expanded.error().message() << '\n';
  std::size_t ADDICount = 0;
  std::size_t CrossMoveCount = 0;
  std::size_t CallCount = 0;
  std::size_t ReturnCount = 0;
  std::size_t CSRRSCount = 0;
  std::size_t CSRRWCount = 0;
  for (rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    ADDICount += Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::ADDI);
    CrossMoveCount += Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::FMV_D_X);
    CallCount += Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::JALR_CALL);
    ReturnCount += Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::JALR_RET);
    CSRRSCount += Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::CSRRS);
    CSRRWCount += Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::CSRRW);
  }
  Context.expect(Expanded && Expanded->Changed && ADDICount == 1 && CrossMoveCount == 1 &&
                     CallCount == 1 && ReturnCount == 1 && CSRRSCount == 1 && CSRRWCount == 1,
                 "post-RA expansion must lower copies, calls, returns, and FFLAGS accesses");
  if (!Expanded)
    return;

  rcc::MachinePassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<rcc::FinalizeEmissionPseudosPass>()) &&
                     Pipeline.addPass(std::make_unique<rcc::FinalEncodabilityVerifierPass>(
                         rcc::riscv::branchInfo())),
                 "finalization passes must register");
  auto Finalized = Pipeline.run(*Function, Target, Analyses);
  if (!Finalized)
    std::cerr << "final encodability failure: " << Finalized.error().message() << '\n';
  Context.expect(Finalized &&
                     Function->properties().has(rcc::MachineFunctionProperty::NoExecutablePseudos),
                 "emission finalization must exclusively publish NoExecutablePseudos");
}

void testCanonicalOptimizationPipelines(TestContext &Context,
                                        const rcc::TargetDescription &Target) {
  const rcc::MachineOptimizationPipelineTargetInfo TargetInfo =
      rcc::riscv::optimizationPipelineInfo();
  rcc::MachinePassManager SelectedPipeline;
  rcc::MachinePassManager PreRAPipeline;
  Context.expect(rcc::addSelectedSSAOptimizationPipeline(SelectedPipeline, TargetInfo) &&
                     SelectedPipeline.size() == 6,
                 "canonical selected-SSA optimization pipeline must register completely");
  Context.expect(rcc::addPreRegisterAllocationOptimizationPipeline(PreRAPipeline, TargetInfo) &&
                     PreRAPipeline.size() == 5,
                 "canonical pre-RA optimization pipeline must register completely");

  auto Function = makeFunction(Context, "canonical_post_ra_pipeline", 21);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  auto SlotAlignment = rcc::Alignment::create(8);
  if (!SlotAlignment)
    return;
  rcc::StackObjectDesc SpillDescription;
  SpillDescription.Kind = rcc::StackObjectKind::SpillSlot;
  SpillDescription.Size = 8;
  SpillDescription.Align = *SlotAlignment;
  auto Spill = Function->frameInfo().createObject(SpillDescription);
  if (!Spill)
    return;
  rcc::MachineIRBuilder Builder(*Function, Target);
  if (!Builder.setInsertPoint(*Block) ||
      buildADDI(Context, Builder, physical(rcc::riscv::PhysicalRegisterID::X10),
                physical(rcc::riscv::PhysicalRegisterID::X0), 42) == nullptr ||
      buildStackAccess(Context, Builder, rcc::riscv::InstructionOpcode::SD,
                       physical(rcc::riscv::PhysicalRegisterID::X10), *Spill,
                       rcc::MemoryAccessFlag::Store) == nullptr) {
    return;
  }
  auto Return = Builder.buildInstr(opcode(rcc::riscv::InstructionOpcode::PseudoRET));
  if (!Return || !Return->addImplicitUse(physical(rcc::riscv::PhysicalRegisterID::X10)) ||
      !Return->build()) {
    Context.expect(false, "canonical post-RA return construction must succeed");
    return;
  }
  setPostRAProperties(*Function);
  if (!rcc::rebuildMachineLiveness(*Function, Target)) {
    Context.expect(false, "canonical post-RA input liveness must be valid");
    return;
  }

  rcc::MachinePassManager Pipeline;
  auto Added = rcc::addPostRegisterAllocationPipeline(Pipeline, TargetInfo);
  Context.expect(Added && Pipeline.size() == 20,
                 "canonical post-RA pipeline must register every required pass");
  if (!Added)
    return;
  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerMachineOptimizationAnalyses(Analyses);
  Context.expect(Registered.has_value(),
                 "canonical optimization analyses must register completely");
  if (!Registered)
    return;
  auto Result = Pipeline.run(*Function, Target, Analyses);
  if (!Result)
    std::cerr << "canonical post-RA pipeline failure: " << Result.error().message() << '\n';
  const bool HasReturn =
      std::ranges::any_of(Function->allInstructions(), [](const rcc::MachineInstr *Instruction) {
        return Instruction != nullptr &&
               Instruction->opcode() == opcode(rcc::riscv::InstructionOpcode::JALR_RET);
      });
  const bool HasFrameSetup =
      std::ranges::any_of(Function->allInstructions(), [](const rcc::MachineInstr *Instruction) {
        return Instruction != nullptr &&
               Instruction->flags().has(rcc::MachineInstrFlag::FrameSetup);
      });
  Context.expect(Result && HasReturn && HasFrameSetup &&
                     Function->properties().has(rcc::MachineFunctionProperty::FrameFinalized) &&
                     Function->properties().has(rcc::MachineFunctionProperty::NoFrameIndices) &&
                     Function->properties().has(rcc::MachineFunctionProperty::NoExecutablePseudos),
                 "canonical post-RA pipeline must reach final encodable RV64GC MachineIR");
}

void testTargetLateExpansionRejectsSizeGrowth(TestContext &Context) {
  auto Target = createLateExpansionSizeTarget();
  Context.expect(Target.has_value(), "target-late size-contract target construction must succeed");
  if (!Target)
    return;
  auto Function = makeFunction(Context, "late_expansion_size_growth", 22);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Function, *Target);
  if (!Builder.setInsertPoint(*Block))
    return;
  auto Pending = Builder.buildInstr(GrowingLatePseudoOpcode);
  if (!Pending)
    return;
  auto Pseudo = Pending->build();
  Context.expect(Pseudo.has_value(), "target-late growth pseudo construction must succeed");
  if (!Pseudo)
    return;

  setPostRAProperties(*Function);
  auto StackAlignment = rcc::Alignment::create(16);
  if (!StackAlignment || !Function->frameInfo().freeze(0, *StackAlignment))
    return;
  Function->properties().set(rcc::MachineFunctionProperty::FrameFinalized);
  Function->properties().set(rcc::MachineFunctionProperty::NoFrameIndices);
  if (!rcc::rebuildMachineLiveness(*Function, *Target))
    return;

  GrowingLatePseudoInfo ExpansionInfo;
  FixedSizeBranchInfo BranchInfo;
  rcc::TargetLatePseudoExpansionPass Pass(ExpansionInfo, BranchInfo);
  rcc::MachineAnalysisManager Analyses;
  auto Result = Pass.run(*Function, *Target, Analyses);
  Context.expect(!Result && Result.error().code() == rcc::ErrorCode::PseudoExpansionFailed &&
                     (*Pseudo)->parent() == Block && Block->firstInstruction() == *Pseudo &&
                     Block->lastInstruction() == *Pseudo,
                 "target-late expansion must reject block-size growth atomically");
}

void testTargetLateExpansionRejectsCompensatedGrowth(TestContext &Context) {
  auto Target = createLateExpansionSizeTarget();
  Context.expect(Target.has_value(), "target-late compensation target construction must succeed");
  if (!Target)
    return;
  auto Function = makeFunction(Context, "late_expansion_compensated_growth", 23);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Block = appendBlock(Context, *Function);
  if (Block == nullptr)
    return;
  rcc::MachineIRBuilder Builder(*Function, *Target);
  if (!Builder.setInsertPoint(*Block))
    return;
  auto Shrinking = Builder.buildInstr(ShrinkingLatePseudoOpcode);
  auto ShrinkingPseudo =
      Shrinking ? Shrinking->build()
                : rcc::Expected<rcc::MachineInstr *>(std::unexpected(
                      rcc::Error(rcc::ErrorCode::InternalError, "no shrinking pseudo builder")));
  auto Growing = Builder.buildInstr(GrowingLatePseudoOpcode);
  auto GrowingPseudo = Growing ? Growing->build()
                               : rcc::Expected<rcc::MachineInstr *>(std::unexpected(rcc::Error(
                                     rcc::ErrorCode::InternalError, "no growing pseudo builder")));
  Context.expect(ShrinkingPseudo.has_value() && GrowingPseudo.has_value(),
                 "compensated target-late pseudos must be constructed");
  if (!ShrinkingPseudo || !GrowingPseudo)
    return;

  setPostRAProperties(*Function);
  auto StackAlignment = rcc::Alignment::create(16);
  if (!StackAlignment || !Function->frameInfo().freeze(0, *StackAlignment))
    return;
  Function->properties().set(rcc::MachineFunctionProperty::FrameFinalized);
  Function->properties().set(rcc::MachineFunctionProperty::NoFrameIndices);
  if (!rcc::rebuildMachineLiveness(*Function, *Target))
    return;

  GrowingLatePseudoInfo ExpansionInfo;
  FixedSizeBranchInfo BranchInfo;
  rcc::TargetLatePseudoExpansionPass Pass(ExpansionInfo, BranchInfo);
  rcc::MachineAnalysisManager Analyses;
  auto Result = Pass.run(*Function, *Target, Analyses);
  Context.expect(
      !Result && Result.error().code() == rcc::ErrorCode::PseudoExpansionFailed &&
          (*ShrinkingPseudo)->parent() == Block && (*GrowingPseudo)->parent() == Block &&
          Block->firstInstruction() == *ShrinkingPseudo &&
          Block->lastInstruction() == *GrowingPseudo,
      "one growing target-late expansion must not be hidden by another pseudo shrinking");
}

void testFinalEncodabilityRejectsInvalidInstruction(TestContext &Context,
                                                    const rcc::TargetDescription &Target) {
  auto ImmediateFunction = makeFunction(Context, "final_invalid_immediate", 18);
  if (!ImmediateFunction)
    return;
  rcc::MachineBasicBlock *ImmediateBlock = appendBlock(Context, *ImmediateFunction);
  if (ImmediateBlock == nullptr)
    return;
  rcc::MachineIRBuilder ImmediateBuilder(*ImmediateFunction, Target);
  if (!ImmediateBuilder.setInsertPoint(*ImmediateBlock))
    return;
  rcc::MachineInstr *InvalidImmediate =
      buildADDI(Context, ImmediateBuilder, physical(rcc::riscv::PhysicalRegisterID::X10),
                physical(rcc::riscv::PhysicalRegisterID::X0), 0);
  if (InvalidImmediate == nullptr || !InvalidImmediate->eraseOperand(2) ||
      !InvalidImmediate->insertOperand(2, rcc::MachineOperand::immediate(4096))) {
    Context.expect(false, "invalid final immediate fixture must be constructed");
    return;
  }
  setRelaxationProperties(Context, *ImmediateFunction, Target);
  rcc::MachineAnalysisManager Analyses;
  rcc::FinalEncodabilityVerifierPass Verifier(rcc::riscv::branchInfo());
  auto ImmediateResult = Verifier.run(*ImmediateFunction, Target, Analyses);
  Context.expect(!ImmediateResult &&
                     ImmediateResult.error().code() == rcc::ErrorCode::FinalEncodabilityFailed,
                 "final encodability must reject an out-of-range target immediate");

  auto BundleFunction = makeFunction(Context, "final_invalid_bundle", 19);
  if (!BundleFunction)
    return;
  rcc::MachineBasicBlock *BundleBlock = appendBlock(Context, *BundleFunction);
  if (BundleBlock == nullptr)
    return;
  rcc::MachineIRBuilder BundleBuilder(*BundleFunction, Target);
  if (!BundleBuilder.setInsertPoint(*BundleBlock))
    return;
  rcc::MachineInstr *InvalidBundle =
      buildADDI(Context, BundleBuilder, physical(rcc::riscv::PhysicalRegisterID::X10),
                physical(rcc::riscv::PhysicalRegisterID::X0), 0);
  if (InvalidBundle == nullptr)
    return;
  InvalidBundle->setFlags({rcc::MachineInstrFlag::BundledWithSucc});
  setRelaxationProperties(Context, *BundleFunction, Target);
  auto BundleResult = Verifier.run(*BundleFunction, Target, Analyses);
  Context.expect(!BundleResult &&
                     BundleResult.error().code() == rcc::ErrorCode::FinalEncodabilityFailed,
                 "final encodability must reject an inconsistent bundle boundary");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target construction must succeed");
  if (!Target)
    return Context.result();
  testBlockPlacement(Context, *Target);
  testFallthroughNormalization(Context, *Target);
  testFallthroughMaterialization(Context, *Target);
  testBranchOptimization(Context, *Target);
  testTailDuplication(Context, *Target);
  testTailMerge(Context, *Target);
  testCopyPropagation(Context, *Target);
  testStackSlotColoring(Context, *Target);
  testRISCVMemoryFoldingBoundary(Context, *Target);
  testShrinkWrappedFrameRegion(Context, *Target);
  testCompressedAndConditionalRelaxation(Context, *Target);
  testLongJALRelaxation(Context, *Target);
  testPseudoPreparation(Context, *Target);
  testPostRAPseudoExpansionAndFinalization(Context, *Target);
  testCanonicalOptimizationPipelines(Context, *Target);
  testTargetLateExpansionRejectsSizeGrowth(Context);
  testTargetLateExpansionRejectsCompensatedGrowth(Context);
  testFinalEncodabilityRejectsInvalidInstruction(Context, *Target);
  return Context.result();
}
