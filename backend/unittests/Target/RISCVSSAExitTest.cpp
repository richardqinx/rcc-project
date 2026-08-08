// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachineSSAExit.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/Target/RISCV/RISCVCodeGen.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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

[[nodiscard]] rcc::Opcode targetOpcode(rcc::riscv::InstructionOpcode Opcode) {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Opcode));
}

[[nodiscard]] rcc::RegisterBankID bank(rcc::riscv::RegisterBank Bank) {
  return rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(Bank));
}

[[nodiscard]] rcc::RegisterClassID regClass(rcc::riscv::RegisterClass Class) {
  return rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(Class));
}

[[nodiscard]] rcc::Expected<rcc::Register> createVReg(rcc::MachineFunction &Function, rcc::LLT Type,
                                                      rcc::riscv::RegisterBank Bank,
                                                      rcc::riscv::RegisterClass Class) {
  auto Reg = Function.registerInfo().createVirtualRegister(Type);
  if (!Reg)
    return std::unexpected(Reg.error());
  auto BankResult = Function.registerInfo().setRegisterBank(*Reg, bank(Bank));
  if (!BankResult)
    return std::unexpected(BankResult.error());
  auto ClassResult = Function.registerInfo().setRegisterClass(*Reg, regClass(Class));
  if (!ClassResult)
    return std::unexpected(ClassResult.error());
  return *Reg;
}

[[nodiscard]] rcc::Expected<rcc::MachineBasicBlock *> appendBlock(rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  if (!Block)
    return std::unexpected(Block.error());
  auto Appended = Function.appendBlock(**Block);
  if (!Appended)
    return std::unexpected(Appended.error());
  return *Block;
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildAddImmediate(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                  rcc::MachineBasicBlock &Block, rcc::Register Destination, rcc::Register Source,
                  std::int64_t Immediate) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(targetOpcode(rcc::riscv::InstructionOpcode::ADDI));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination);
  if (!Def)
    return std::unexpected(Def.error());
  auto Use = Pending->addUse(Source);
  if (!Use)
    return std::unexpected(Use.error());
  auto Value = Pending->addImmediate(Immediate);
  if (!Value)
    return std::unexpected(Value.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildBranchEqual(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                 rcc::MachineBasicBlock &Block, rcc::Register Left, rcc::Register Right,
                 rcc::MachineBasicBlock &Destination) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(targetOpcode(rcc::riscv::InstructionOpcode::BEQ));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto LeftUse = Pending->addUse(Left);
  if (!LeftUse)
    return std::unexpected(LeftUse.error());
  auto RightUse = Pending->addUse(Right);
  if (!RightUse)
    return std::unexpected(RightUse.error());
  auto TargetOperand = Pending->addBasicBlock(Destination);
  if (!TargetOperand)
    return std::unexpected(TargetOperand.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildPhi(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Destination,
         const std::array<std::pair<rcc::Register, rcc::MachineBasicBlock *>, 2> &Incoming) {
  rcc::MachineIRBuilder Builder(Function, Target);
  auto Point = Builder.setInsertPoint(Block);
  if (!Point)
    return std::unexpected(Point.error());
  auto Pending = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI));
  if (!Pending)
    return std::unexpected(Pending.error());
  auto Def = Pending->addDef(Destination);
  if (!Def)
    return std::unexpected(Def.error());
  for (const auto &[Value, Predecessor] : Incoming) {
    auto Use = Pending->addUse(Value);
    if (!Use)
      return std::unexpected(Use.error());
    auto IncomingBlock = Pending->addBasicBlock(*Predecessor);
    if (!IncomingBlock)
      return std::unexpected(IncomingBlock.error());
  }
  return Pending->build();
}

[[nodiscard]] const rcc::BasicBlockOperand *blockOperand(const rcc::MachineInstr &Instruction,
                                                         std::size_t Index) {
  const rcc::MachineOperand *Operand = Instruction.operandAt(Index);
  return Operand == nullptr ? nullptr : Operand->getIf<rcc::BasicBlockOperand>();
}

[[nodiscard]] std::vector<rcc::MachineInstr *> linkedInstructions(rcc::MachineBasicBlock &Block) {
  std::vector<rcc::MachineInstr *> Result;
  for (rcc::MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    Result.push_back(Instruction);
  }
  return Result;
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

void testBehaviorAndCopyLegality(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = rcc::MachineFunction::create("riscv_ssa_behavior",
                                               rcc::MachineFunctionStableID::fromValue(1));
  if (!Function)
    return;
  auto First = appendBlock(**Function);
  auto Second = appendBlock(**Function);
  auto Third = appendBlock(**Function);
  if (!First || !Second || !Third)
    return;
  auto CFG = (*Function)->cfgUpdater();
  if (!CFG.addSuccessor(**First, **Second, rcc::BranchProbability::one()))
    return;

  auto Fallthrough = rcc::riscv::ssaInfo().isFallthroughEdge(**First, **Second, **Function, Target);
  Context.expect(Fallthrough && *Fallthrough,
                 "RV64GC SSA behavior must recognize an empty layout fallthrough edge");
  auto Inserted =
      rcc::riscv::ssaInfo().insertUnconditionalBranch(**First, **Second, **Function, Target);
  Context.expect(Inserted.has_value(), "RV64GC SSA behavior must insert an unconditional branch");
  if (!Inserted)
    return;
  rcc::MachineInstr *Jump = (*First)->lastInstruction();
  const auto X0 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X0);
  Context.expect(Jump != nullptr &&
                     Jump->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::JAL) &&
                     Jump->operandAt(0)->registerValue() == rcc::Register::fromPhysical(*X0) &&
                     blockOperand(*Jump, 1) != nullptr && blockOperand(*Jump, 1)->Block == *Second,
                 "RV64GC unconditional branches must use JAL with x0 as the link destination");
  auto NoFallthrough =
      rcc::riscv::ssaInfo().isFallthroughEdge(**First, **Second, **Function, Target);
  Context.expect(NoFallthrough && !*NoFallthrough, "JAL must terminate RV64GC layout fallthrough");
  auto Rewritten =
      rcc::riscv::ssaInfo().rewriteBranchTarget(**First, **Second, **Third, **Function, Target);
  Context.expect(Rewritten && *Rewritten && blockOperand(*Jump, 1)->Block == *Third,
                 "RV64GC branch target rewriting must update direct JAL operands");

  auto I64 = rcc::LLT::makeInteger(64);
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  auto F32 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEESingle);
  if (!I64 || !F64 || !F32)
    return;
  auto GPR =
      createVReg(**Function, *I64, rcc::riscv::RegisterBank::GPR, rcc::riscv::RegisterClass::GPR);
  auto OtherGPR =
      createVReg(**Function, *I64, rcc::riscv::RegisterBank::GPR, rcc::riscv::RegisterClass::GPR);
  auto FPR64 =
      createVReg(**Function, *F64, rcc::riscv::RegisterBank::FPR, rcc::riscv::RegisterClass::FPR64);
  auto FPR32 =
      createVReg(**Function, *F32, rcc::riscv::RegisterBank::FPR, rcc::riscv::RegisterClass::FPR32);
  auto Unclassified = (*Function)->registerInfo().createVirtualRegister(*I64);
  if (!GPR || !OtherGPR || !FPR64 || !FPR32 || !Unclassified)
    return;

  Context.expect(
      rcc::riscv::ssaInfo().validateCopy(*GPR, {}, *OtherGPR, {}, **Function, Target).has_value(),
      "RV64GC must allow a compatible GPR COPY");
  Context.expect(
      rcc::riscv::ssaInfo().validateCopy(*GPR, {}, *FPR64, {}, **Function, Target).has_value(),
      "RV64GC must allow supported 64-bit GPR/FPR bit moves");
  Context.expect(!rcc::riscv::ssaInfo().validateCopy(*FPR64, {}, *FPR32, {}, **Function, Target),
                 "RV64GC must reject incompatible same-bank COPY widths");
  Context.expect(!rcc::riscv::ssaInfo().validateCopy(*GPR, rcc::SubRegIndex::fromValue(1),
                                                     *OtherGPR, {}, **Function, Target),
                 "RV64GC must reject unmodeled subregister COPYs");
  Context.expect(
      !rcc::riscv::ssaInfo().validateCopy(*GPR, {}, *Unclassified, {}, **Function, Target),
      "RV64GC must reject COPYs involving an unconstrained virtual register");
}

void testRV64GCCriticalEdge(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = rcc::MachineFunction::create("riscv_ssa_critical_edge",
                                               rcc::MachineFunctionStableID::fromValue(2));
  if (!Function)
    return;
  auto From = appendBlock(**Function);
  auto Side = appendBlock(**Function);
  auto Other = appendBlock(**Function);
  auto Merge = appendBlock(**Function);
  if (!From || !Side || !Other || !Merge)
    return;
  auto I64 = rcc::LLT::makeInteger(64);
  auto X0 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X0);
  if (!I64 || !X0)
    return;
  auto FromValue =
      createVReg(**Function, *I64, rcc::riscv::RegisterBank::GPR, rcc::riscv::RegisterClass::GPR);
  auto OtherValue =
      createVReg(**Function, *I64, rcc::riscv::RegisterBank::GPR, rcc::riscv::RegisterClass::GPR);
  auto Destination =
      createVReg(**Function, *I64, rcc::riscv::RegisterBank::GPR, rcc::riscv::RegisterClass::GPR);
  if (!FromValue || !OtherValue || !Destination)
    return;

  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto CFG = (*Function)->cfgUpdater();
  Context.expect(Half && CFG.addSuccessor(**From, **Side, *Half) &&
                     CFG.addSuccessor(**From, **Merge, *Half) &&
                     CFG.addSuccessor(**Other, **Merge, rcc::BranchProbability::one()),
                 "RV64GC critical-edge CFG must be connected");
  const rcc::Register Zero = rcc::Register::fromPhysical(*X0);
  Context.expect(buildAddImmediate(**Function, Target, **From, *FromValue, Zero, 1) &&
                     buildBranchEqual(**Function, Target, **From, Zero, Zero, **Merge) &&
                     buildAddImmediate(**Function, Target, **Other, *OtherValue, Zero, 2),
                 "RV64GC critical-edge instructions must be built");
  const std::array Incoming{
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*FromValue, *From},
      std::pair<rcc::Register, rcc::MachineBasicBlock *>{*OtherValue, *Other}};
  Context.expect(buildPhi(**Function, Target, **Merge, *Destination, Incoming).has_value(),
                 "RV64GC critical-edge PHI must be built");
  auto InitialLiveness = setSelectedSSAProperties(**Function, Target);
  Context.expect(InitialLiveness.has_value(), "RV64GC PHI liveness rebuild must succeed");
  if (!InitialLiveness)
    return;

  rcc::MachinePassManager Pipeline;
  Context.expect(Pipeline.addPass(std::make_unique<rcc::PHIEliminationPass>(rcc::riscv::ssaInfo()))
                     .has_value(),
                 "RV64GC PHI-elimination pass must be registered");
  rcc::MachineAnalysisManager Analyses;
  auto Run = Pipeline.run(**Function, Target, Analyses);
  Context.expect(Run.has_value(), "RV64GC critical-edge PHI elimination must succeed");
  if (!Run)
    return;

  rcc::MachineBasicBlock *Edge = (*Merge)->previous();
  Context.expect(Edge != nullptr && Edge != *Other && Edge->previous() == *Other,
                 "RV64GC critical edge must receive a dedicated layout block");
  if (Edge == nullptr || Edge == *Other)
    return;
  rcc::MachineInstr *Conditional = (*From)->lastInstruction();
  Context.expect(Conditional != nullptr &&
                     Conditional->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::BEQ) &&
                     blockOperand(*Conditional, 2) != nullptr &&
                     blockOperand(*Conditional, 2)->Block == Edge,
                 "RV64GC BEQ must be redirected to the critical-edge block");

  const auto OtherInstructions = linkedInstructions(**Other);
  const auto EdgeInstructions = linkedInstructions(*Edge);
  Context.expect(OtherInstructions.size() == 3 &&
                     OtherInstructions[1]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     OtherInstructions[2]->opcode() ==
                         targetOpcode(rcc::riscv::InstructionOpcode::JAL) &&
                     blockOperand(*OtherInstructions[2], 1)->Block == *Merge,
                 "RV64GC must materialize an unrelated fallthrough before edge-block insertion");
  Context.expect(EdgeInstructions.size() == 2 &&
                     EdgeInstructions[0]->opcode() == rcc::opcode(rcc::GenericOpcode::COPY) &&
                     EdgeInstructions[1]->opcode() ==
                         targetOpcode(rcc::riscv::InstructionOpcode::JAL) &&
                     EdgeInstructions[1]->operandAt(0)->registerValue() == Zero &&
                     blockOperand(*EdgeInstructions[1], 1)->Block == *Merge,
                 "RV64GC edge block must transfer the PHI value and jump to the merge");
  Context.expect((*Function)->properties().has(rcc::MachineFunctionProperty::NoPHIs) &&
                     !(*Function)->properties().has(rcc::MachineFunctionProperty::IsSSA),
                 "RV64GC PHI elimination must publish SSA-exit properties");
  Context.expect(rcc::MachineVerifier::verify(**Function, Target).has_value(),
                 "RV64GC SSA-exit result must pass MachineVerifier");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target must be created for SSA-exit tests");
  if (!Target)
    return Context.result();
  testBehaviorAndCopyLegality(Context, *Target);
  testRV64GCCriticalEdge(Context, *Target);
  return Context.result();
}
