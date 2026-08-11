// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineLiveness.h"

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
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

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

namespace {

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

[[nodiscard]] rcc::Opcode testOpcode(std::uint32_t Value) {
  return rcc::Opcode::fromValue(Value);
}

[[nodiscard]] rcc::OperandDescriptor registerOperand(rcc::RegisterOperandRole Role,
                                                     bool EarlyClobber = false) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.EarlyClobber = EarlyClobber;
  Result.AllowSubReg = true;
  return Result;
}

[[nodiscard]] rcc::OperandDescriptor plainOperand(rcc::MachineOperand::Kind Kind) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {Kind};
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

[[nodiscard]] rcc::Expected<FunctionFixture> createFunction(std::string Name, std::uint64_t ID,
                                                            std::size_t BlockCount) {
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

[[nodiscard]] rcc::Expected<rcc::Register> createVReg(rcc::MachineFunction &Function) {
  auto Type = rcc::LLT::makeInteger(64);
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

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
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

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
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

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
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

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
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

[[nodiscard]] rcc::PhysicalRegister physical(std::uint32_t Number) {
  return *rcc::PhysicalRegister::create(Number);
}

[[nodiscard]] bool hasFlag(const rcc::MachineInstr &Instruction, std::size_t OperandIndex,
                           rcc::RegisterOperandFlag Flag) {
  const rcc::MachineOperand *Operand = Instruction.operandAt(OperandIndex);
  return Operand != nullptr && Operand->registerFlags().has(Flag);
}

void testLiveRange(TestContext &Context) {
  const rcc::SlotIndex S16 = rcc::SlotIndex::fromValue(16).registerSlot();
  const rcc::SlotIndex S24 = rcc::SlotIndex::fromValue(24).registerSlot();
  const rcc::SlotIndex S32 = rcc::SlotIndex::fromValue(32).registerSlot();
  const rcc::SlotIndex S40 = rcc::SlotIndex::fromValue(40).registerSlot();
  const rcc::SlotIndex S48 = rcc::SlotIndex::fromValue(48).registerSlot();

  rcc::LiveRange Range;
  auto First = Range.createValue(S16);
  Context.expect(First.has_value(), "live value creation must succeed");
  if (!First)
    return;
  Context.expect(Range.addSegment({S16, S32, *First}).has_value() &&
                     Range.addSegment({S32, S48, *First}).has_value() &&
                     Range.segments().size() == 1,
                 "adjacent same-value live segments must merge");
  auto Second = Range.createValue(S40, rcc::LiveValueState::Redefined);
  Context.expect(Second.has_value() && !Range.addSegment({S24, S40, *Second}),
                 "overlapping different live values must be rejected");
  Context.expect(Range.valueAt(S24) != nullptr && Range.liveAt(S40) &&
                     Range.remove(S24, S40).has_value() && Range.segments().size() == 2 &&
                     Range.verify().has_value(),
                 "live-range lookup and removal must preserve ordered half-open segments");

  rcc::LiveRange Shrunk;
  auto Value = Shrunk.createValue(S16);
  if (!Value) {
    Context.expect(false, "shrink test value creation must succeed");
    return;
  }
  const std::array Uses{S32};
  Context.expect(Shrunk.addSegment({S16, S48.deadSlot(), *Value}).has_value() &&
                     Shrunk.shrinkToUses(Uses).has_value() && Shrunk.segments().size() == 1 &&
                     Shrunk.segments().front().End == S32.deadSlot(),
                 "shrinkToUses must retain exactly through the final use dead slot");
}

void testSlotIndexes(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("slot-indexes", 1, 1);
  if (!Fixture) {
    Context.expect(false, "SlotIndexes fixture creation must succeed");
    return;
  }
  auto Reg = createVReg(*Fixture->Function);
  auto Def = Reg ? buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg)
                 : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Reg.error())};
  auto Use = Reg ? buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg)
                 : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Reg.error())};
  if (!Reg || !Def || !Use) {
    Context.expect(false, "SlotIndexes instructions must be built");
    return;
  }

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Indexes =
      Registered
          ? Analyses.getResult<rcc::SlotIndexesAnalysis>(*Fixture->Function, Target)
          : rcc::Expected<rcc::SlotIndexesAnalysis::Result *>{std::unexpected(Registered.error())};
  if (!Indexes) {
    Context.expect(false, "SlotIndexes analysis must materialize");
    return;
  }
  const rcc::SlotIndex DefIndex = (*Indexes)->instructionIndex(**Def);
  const rcc::SlotIndex UseIndex = (*Indexes)->instructionIndex(**Use);
  Context.expect((*Indexes)->blockStart(*Fixture->Blocks[0]) < DefIndex.earlyClobberSlot() &&
                     DefIndex.earlyClobberSlot() < DefIndex && DefIndex < DefIndex.deadSlot() &&
                     DefIndex.deadSlot() < UseIndex &&
                     UseIndex < (*Indexes)->blockEnd(*Fixture->Blocks[0]),
                 "SlotIndexes must order block, early-clobber, register, and dead slots");
  Context.expect((*Indexes)->instructionFromIndex(DefIndex.deadSlot()) == *Def &&
                     (*Indexes)->blockFromIndex(UseIndex) == Fixture->Blocks[0],
                 "SlotIndexes reverse mappings must accept every instruction subslot");

  auto Inserted = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, {}, *Use);
  Context.expect(Inserted && (*Indexes)->insertInstruction(**Inserted).has_value(),
                 "local SlotIndex insertion must use reserved numbering space");
  if (Inserted) {
    const rcc::SlotIndex InsertedIndex = (*Indexes)->instructionIndex(**Inserted);
    Context.expect(DefIndex < InsertedIndex && InsertedIndex < UseIndex,
                   "locally inserted SlotIndex must lie between adjacent instructions");
    Context.expect((*Indexes)->eraseInstruction(**Inserted).has_value() &&
                       !(*Indexes)->instructionIndex(**Inserted).isValid() &&
                       (*Indexes)->renumber(*Fixture->Function).has_value() &&
                       (*Indexes)->instructionIndex(**Inserted).isValid(),
                   "SlotIndex erasure and full renumbering must rebuild exact mappings");

    auto Second = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, {}, *Inserted);
    auto SecondIndexed = Second ? (*Indexes)->insertInstruction(**Second)
                                : rcc::Expected<void>{std::unexpected(Second.error())};
    auto Third = Second
                     ? buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, {}, *Second)
                     : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Second.error())};
    auto ThirdIndexed = Third ? (*Indexes)->insertInstruction(**Third)
                              : rcc::Expected<void>{std::unexpected(Third.error())};
    auto Exhausted =
        Third ? buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, {}, *Third)
              : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Third.error())};
    auto ExhaustedIndex = Exhausted ? (*Indexes)->insertInstruction(**Exhausted)
                                    : rcc::Expected<void>{std::unexpected(Exhausted.error())};
    Context.expect(SecondIndexed && ThirdIndexed && Exhausted && !ExhaustedIndex &&
                       ExhaustedIndex.error().code() == rcc::ErrorCode::Unsupported &&
                       (*Indexes)->renumber(*Fixture->Function).has_value() &&
                       (*Indexes)->instructionIndex(**Exhausted).isValid(),
                   "local SlotIndex exhaustion must request and recover through full renumbering");
  }
}

void testCFGAndValueNumbers(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("cfg-liveness", 2, 8);
  if (!Fixture) {
    Context.expect(false, "CFG liveness fixture creation must succeed");
    return;
  }
  auto &Function = *Fixture->Function;
  auto &Entry = *Fixture->Blocks[0];
  auto &Left = *Fixture->Blocks[1];
  auto &Right = *Fixture->Blocks[2];
  auto &Join = *Fixture->Blocks[3];
  auto &Header = *Fixture->Blocks[4];
  auto &Body = *Fixture->Blocks[5];
  auto &Exit = *Fixture->Blocks[6];
  auto &Dead = *Fixture->Blocks[7];
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto Updater = Function.cfgUpdater();
  const bool CFGValid = Half && Updater.addSuccessor(Entry, Left, *Half) &&
                        Updater.addSuccessor(Entry, Right, *Half) &&
                        Updater.addSuccessor(Left, Join, rcc::BranchProbability::one()) &&
                        Updater.addSuccessor(Right, Join, rcc::BranchProbability::one()) &&
                        Updater.addSuccessor(Join, Header, rcc::BranchProbability::one()) &&
                        Updater.addSuccessor(Header, Body, *Half) &&
                        Updater.addSuccessor(Header, Exit, *Half) &&
                        Updater.addSuccessor(Body, Header, rcc::BranchProbability::one());
  Context.expect(CFGValid, "diamond and loop CFG construction must succeed");
  if (!CFGValid)
    return;

  auto Across = createVReg(Function);
  auto Merged = createVReg(Function);
  auto Unreachable = createVReg(Function);
  auto Redefined = createVReg(Function);
  if (!Across || !Merged || !Unreachable || !Redefined) {
    Context.expect(false, "CFG liveness virtual registers must be created");
    return;
  }
  auto AcrossDef = buildDef(Function, Target, Entry, *Across);
  auto LeftDef = buildDef(Function, Target, Left, *Merged);
  auto RightDef = buildDef(Function, Target, Right, *Merged);
  auto MergedUse = buildUse(Function, Target, Join, *Merged);
  auto LoopUse = buildUse(Function, Target, Body, *Across);
  auto DeadUse = buildUse(Function, Target, Dead, *Unreachable);
  auto FirstRedef = buildDef(Function, Target, Exit, *Redefined);
  auto SecondRedef = buildDef(Function, Target, Exit, *Redefined);
  if (!AcrossDef || !LeftDef || !RightDef || !MergedUse || !LoopUse || !DeadUse || !FirstRedef ||
      !SecondRedef) {
    Context.expect(false, "CFG liveness instructions must be built");
    return;
  }

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Variables = Registered ? Analyses.getResult<rcc::LiveVariablesAnalysis>(Function, Target)
                              : rcc::Expected<rcc::LiveVariablesAnalysis::Result *>{
                                    std::unexpected(Registered.error())};
  auto Intervals = Registered ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(Function, Target)
                              : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                                    std::unexpected(Registered.error())};
  if (!Variables || !Intervals) {
    Context.expect(false, "CFG liveness analyses must materialize");
    return;
  }
  Context.expect((*Variables)->isLiveOut(Entry, *Across) && (*Variables)->isLiveIn(Left, *Across) &&
                     (*Variables)->isLiveIn(Right, *Across) &&
                     (*Variables)->isLiveIn(Header, *Across) &&
                     (*Variables)->isLiveOut(Body, *Across),
                 "CFG fixed point must propagate a value through a diamond and loop backedge");
  Context.expect(
      (*Variables)->isLiveOut(Left, *Merged) && (*Variables)->isLiveOut(Right, *Merged) &&
          (*Variables)->isLiveIn(Join, *Merged) && (*Variables)->isLiveIn(Dead, *Unreachable),
      "liveness must merge predecessor definitions and retain unreachable uses");

  const rcc::LiveInterval *MergedInterval = (*Intervals)->interval(*Merged);
  const rcc::LiveInterval *AcrossInterval = (*Intervals)->interval(*Across);
  const rcc::LiveInterval *RedefinedInterval = (*Intervals)->interval(*Redefined);
  const bool HasPhi = MergedInterval != nullptr &&
                      std::ranges::any_of(MergedInterval->MainRange.values(),
                                          [](const rcc::LiveValueNumber &Value) {
                                            return Value.State == rcc::LiveValueState::Phi;
                                          });
  const bool HasRedefined =
      RedefinedInterval != nullptr &&
      std::ranges::any_of(RedefinedInterval->MainRange.values(),
                          [](const rcc::LiveValueNumber &Value) {
                            return Value.State == rcc::LiveValueState::Redefined;
                          });
  Context.expect(MergedInterval != nullptr && MergedInterval->MainRange.values().size() >= 3 &&
                     HasPhi && HasRedefined,
                 "non-SSA intervals must retain multiple definitions, PHI, and redefined values");
  Context.expect(AcrossInterval != nullptr && (*Intervals)->interferes(*Across, *Merged),
                 "live intervals must expose CFG-derived interference");
}

void testLaneSubrangesAndEarlyClobber(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("lane-subranges", 3, 1);
  if (!Fixture) {
    Context.expect(false, "lane-subrange fixture creation must succeed");
    return;
  }
  auto Reg = createVReg(*Fixture->Function);
  auto Early = createVReg(*Fixture->Function);
  if (!Reg || !Early) {
    Context.expect(false, "lane-subrange registers must be created");
    return;
  }
  auto LowDef =
      buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, rcc::test::TestSubReg::Low32);
  auto LowUse =
      buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg, rcc::test::TestSubReg::Low32);
  auto HighDef = buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg,
                          rcc::test::TestSubReg::High32);
  auto HighUse = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg,
                          rcc::test::TestSubReg::High32);
  auto EarlyDef = buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Early, {}, true);
  auto EarlyUse = buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Early);
  if (!LowDef || !LowUse || !HighDef || !HighUse || !EarlyDef || !EarlyUse) {
    Context.expect(false, "lane-subrange instructions must be built");
    return;
  }

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Intervals = Registered
                       ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target)
                       : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                             std::unexpected(Registered.error())};
  if (!Intervals) {
    Context.expect(false, "lane-subrange intervals must materialize");
    return;
  }
  const rcc::LiveInterval *Interval = (*Intervals)->interval(*Reg);
  const rcc::LiveInterval *EarlyInterval = (*Intervals)->interval(*Early);
  const bool HasLow = Interval != nullptr &&
                      std::ranges::any_of(Interval->SubRanges, [](const rcc::LiveSubRange &Range) {
                        return Range.Lanes == rcc::LaneMask::fromRaw(0x3);
                      });
  const bool HasHigh = Interval != nullptr &&
                       std::ranges::any_of(Interval->SubRanges, [](const rcc::LiveSubRange &Range) {
                         return Range.Lanes == rcc::LaneMask::fromRaw(0xC);
                       });
  Context.expect(Interval != nullptr && Interval->SubRanges.size() == 2 && HasLow && HasHigh,
                 "partial virtual-register accesses must form disjoint lane subranges");
  Context.expect(EarlyInterval != nullptr && !EarlyInterval->MainRange.values().empty() &&
                     EarlyInterval->MainRange.values().front().Definition.kind() ==
                         rcc::SlotIndexKind::EarlyClobber,
                 "early-clobber definitions must use the early-clobber slot");
}

void testPhysicalUnitsAndRegMasks(TestContext &Context, const rcc::TargetDescription &Target) {
  const rcc::RegisterUnitID MultiLaneUnit = rcc::RegisterUnitID::fromValue(9);
  rcc::RegisterUnitLaneSet PartialLanes;
  PartialLanes.add(MultiLaneUnit, rcc::LaneMask::fromRaw(0x3));
  rcc::RegisterUnitLaneSet RemovedLanes;
  RemovedLanes.add(MultiLaneUnit, rcc::LaneMask::fromRaw(0x2));
  PartialLanes.subtract(RemovedLanes);
  Context.expect(PartialLanes.lanes(MultiLaneUnit) == rcc::LaneMask::fromRaw(0x1) &&
                     PartialLanes.contains(MultiLaneUnit, rcc::LaneMask::fromRaw(0x1)) &&
                     !PartialLanes.intersects(MultiLaneUnit, rcc::LaneMask::fromRaw(0x2)),
                 "RegisterUnit lane sets must preserve partial union and subtraction");

  auto AliasFixture = createFunction("physical-alias", 4, 1);
  if (!AliasFixture) {
    Context.expect(false, "physical-alias fixture creation must succeed");
    return;
  }
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  const rcc::Register G0High =
      rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0High32));
  auto G0Def = buildDef(*AliasFixture->Function, Target, *AliasFixture->Blocks[0], G0);
  auto G0HighUse = buildUse(*AliasFixture->Function, Target, *AliasFixture->Blocks[0], G0High);
  if (!G0Def || !G0HighUse) {
    Context.expect(false, "physical-alias instructions must be built");
    return;
  }
  rcc::MachineAnalysisManager AliasAnalyses;
  auto AliasRegistered = rcc::registerStage11MachineAnalyses(AliasAnalyses);
  auto AliasIntervals =
      AliasRegistered
          ? AliasAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*AliasFixture->Function, Target)
          : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                std::unexpected(AliasRegistered.error())};
  const rcc::RegisterUnitID HighUnit = rcc::RegisterUnitID::fromValue(3);
  Context.expect(AliasIntervals && (*AliasIntervals)->regUnitRange(HighUnit) != nullptr &&
                     (*AliasIntervals)->regUnitRange(HighUnit)->segments().size() == 1,
                 "shared RegisterUnits must connect super-register defs to subregister uses");

  auto MaskFixture = createFunction("regmask", 5, 1);
  if (!MaskFixture) {
    Context.expect(false, "RegMask fixture creation must succeed");
    return;
  }
  auto Across = createVReg(*MaskFixture->Function);
  auto Output = createVReg(*MaskFixture->Function);
  if (!Across || !Output) {
    Context.expect(false, "RegMask virtual registers must be created");
    return;
  }
  const auto Mask = rcc::test::createTestCallPreservedMask();
  auto AcrossDef = buildDef(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Across);
  auto FirstMask = buildMask(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], Mask);
  auto AcrossUse = buildUse(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Across);
  auto OutputDef =
      buildMaskedDef(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Output, Mask);
  auto OutputUse = buildUse(*MaskFixture->Function, Target, *MaskFixture->Blocks[0], *Output);
  if (!AcrossDef || !FirstMask || !AcrossUse || !OutputDef || !OutputUse) {
    Context.expect(false, "RegMask instructions must be built");
    return;
  }
  rcc::MachineAnalysisManager MaskAnalyses;
  auto MaskRegistered = rcc::registerStage11MachineAnalyses(MaskAnalyses);
  auto MaskIntervals =
      MaskRegistered
          ? MaskAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*MaskFixture->Function, Target)
          : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                std::unexpected(MaskRegistered.error())};
  if (!MaskIntervals) {
    Context.expect(false, "RegMask intervals must materialize");
    return;
  }
  const rcc::PhysicalRegister Candidate = physical(rcc::test::TestRegister::G0);
  const rcc::LiveInterval *AcrossInterval = (*MaskIntervals)->interval(*Across);
  const rcc::LiveInterval *OutputInterval = (*MaskIntervals)->interval(*Output);
  Context.expect((*MaskIntervals)->regMaskPositions().size() == 2 &&
                     std::ranges::all_of((*MaskIntervals)->regMaskPositions(),
                                         [](const rcc::RegMaskPosition &Position) {
                                           return Position.Position.kind() ==
                                                  rcc::SlotIndexKind::EarlyClobber;
                                         }),
                 "RegMask interference positions must use early-clobber slots");
  Context.expect(AcrossInterval != nullptr && OutputInterval != nullptr &&
                     (*MaskIntervals)->regMaskClobbers(*AcrossInterval, Candidate) &&
                     !(*MaskIntervals)->regMaskClobbers(*OutputInterval, Candidate),
                 "RegMask queries must reject live-through values but accept same-call outputs");
  Context.expect((*MaskIntervals)->regUnitRange(rcc::RegisterUnitID::fromValue(1)) == nullptr,
                 "unused RegMask clobbers must not expand into physical live ranges");
}

void setRebuildProperties(rcc::MachineFunction &Function) {
  Function.properties().set(rcc::MachineFunctionProperty::Legalized);
  Function.properties().set(rcc::MachineFunctionProperty::RegBankSelected);
  Function.properties().set(rcc::MachineFunctionProperty::Selected);
  Function.properties().set(rcc::MachineFunctionProperty::NoPHIs);
  Function.properties().set(rcc::MachineFunctionProperty::TiedOpsRewritten);
}

void testLivenessRebuildAndVerifier(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("rebuild", 6, 2);
  if (!Fixture) {
    Context.expect(false, "liveness-rebuild fixture creation must succeed");
    return;
  }
  auto &Function = *Fixture->Function;
  auto &Entry = *Fixture->Blocks[0];
  auto &Exit = *Fixture->Blocks[1];
  auto Edge = Function.cfgUpdater().addSuccessor(Entry, Exit, rcc::BranchProbability::one());
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  const rcc::Register G1 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G1));
  auto LiveDef = buildDef(Function, Target, Entry, G0);
  auto FirstUse = buildUse(Function, Target, Exit, G0);
  auto LastUse = buildUse(Function, Target, Exit, G0);
  auto DeadDef = buildDef(Function, Target, Exit, G1);
  if (!Edge || !LiveDef || !FirstUse || !LastUse || !DeadDef) {
    Context.expect(false, "liveness-rebuild MachineIR must be constructed");
    return;
  }
  auto StaleLiveIn = Entry.addLiveIn(physical(rcc::test::TestRegister::F0));
  Context.expect(StaleLiveIn.has_value(), "stale live-in setup must succeed");
  setRebuildProperties(Function);

  rcc::MachinePassManager Pipeline;
  auto Added = Pipeline.addPass(std::make_unique<rcc::RebuildMachineLivenessPass>());
  rcc::MachineAnalysisManager Analyses;
  auto Run = Added ? Pipeline.run(Function, Target, Analyses)
                   : rcc::Expected<void>{std::unexpected(Added.error())};
  if (!Run)
    std::cerr << "liveness rebuild pipeline error: " << Run.error().message() << '\n';
  Context.expect(Run.has_value() &&
                     Function.properties().has(rcc::MachineFunctionProperty::TracksLiveness),
                 "RebuildMachineLiveness must publish TracksLiveness through the pass manager");
  if (!Run)
    return;
  Context.expect(
      Entry.liveIns().empty() && Exit.liveIns().size() == 1 &&
          Exit.liveIns().front().PhysReg == physical(rcc::test::TestRegister::G0) &&
          Exit.liveIns().front().Lanes == rcc::LaneMask::all(),
      "liveness rebuild must replace stale live-ins with canonical whole-register input");
  Context.expect(!hasFlag(**LiveDef, 0, rcc::RegisterOperandFlag::Dead) &&
                     !hasFlag(**FirstUse, 0, rcc::RegisterOperandFlag::Kill) &&
                     hasFlag(**LastUse, 0, rcc::RegisterOperandFlag::Kill) &&
                     hasFlag(**DeadDef, 0, rcc::RegisterOperandFlag::Dead),
                 "reverse reconstruction must derive conservative kill and dead flags");
  Context.expect(rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "rebuilt tracked liveness must pass the MachineVerifier");

  auto ConservativeLiveIn = Entry.addLiveIn(physical(rcc::test::TestRegister::F0));
  Context.expect(ConservativeLiveIn.has_value() &&
                     rcc::MachineVerifier::verify(Function, Target).has_value(),
                 "MachineVerifier must accept conservative physical live-ins");
  Entry.clearLiveIns();

  rcc::MachineOperand *FirstUseOperand = (*FirstUse)->operandAt(0);
  if (FirstUseOperand == nullptr) {
    Context.expect(false, "first use operand must exist");
    return;
  }
  rcc::RegisterOperandFlags StaleFlags = FirstUseOperand->registerFlags();
  StaleFlags.set(rcc::RegisterOperandFlag::Kill);
  auto StaleKill = FirstUseOperand->setRegisterFlags(StaleFlags);
  Context.expect(StaleKill.has_value() && !rcc::MachineVerifier::verify(Function, Target),
                 "MachineVerifier must reject an overstrong tracked kill flag");
  StaleFlags.clear(rcc::RegisterOperandFlag::Kill);
  auto ClearedKill = FirstUseOperand->setRegisterFlags(StaleFlags);
  Context.expect(ClearedKill.has_value(), "stale kill flag cleanup must succeed");
  Exit.clearLiveIns();
  Context.expect(!rcc::MachineVerifier::verify(Function, Target),
                 "MachineVerifier must reject stale tracked physical live-ins");

  rcc::RebuildMachineLivenessPass Rebuild;
  auto Repaired = Rebuild.run(Function, Target, Analyses);
  Context.expect(Repaired && rcc::MachineVerifier::verify(Function, Target),
                 "a second liveness rebuild must repair stale derived state");
}

void testFailedRebuildIsAtomic(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("failed-rebuild", 9, 2);
  if (!Fixture) {
    Context.expect(false, "failed-rebuild fixture creation must succeed");
    return;
  }
  auto Destination = createVReg(*Fixture->Function);
  auto Source = createVReg(*Fixture->Function);
  if (!Destination || !Source) {
    Context.expect(false, "failed-rebuild virtual registers must be created");
    return;
  }

  auto &Predecessor = *Fixture->Blocks[0];
  auto &PhiBlock = *Fixture->Blocks[1];
  const auto LiveIn =
      PhiBlock.addLiveIn(physical(rcc::test::TestRegister::G0), rcc::LaneMask::fromRaw(0x1));
  rcc::RegisterOperandFlags KillFlags;
  KillFlags.set(rcc::RegisterOperandFlag::Kill);
  rcc::MachineIRBuilder Builder(*Fixture->Function, Target);
  auto Point = Builder.setInsertPoint(PhiBlock);
  auto Pending = Point ? Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::PHI))
                       : rcc::Expected<rcc::MachineInstrBuilder>{std::unexpected(Point.error())};
  auto Def = Pending ? Pending->addDef(*Destination)
                     : rcc::Expected<void>{std::unexpected(Pending.error())};
  auto Use =
      Def ? Pending->addUse(*Source, KillFlags) : rcc::Expected<void>{std::unexpected(Def.error())};
  auto Incoming =
      Use ? Pending->addBasicBlock(Predecessor) : rcc::Expected<void>{std::unexpected(Use.error())};
  auto Phi = Incoming ? Pending->build()
                      : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Incoming.error())};
  if (!LiveIn || !Phi) {
    Context.expect(false, "malformed-edge PHI setup must succeed");
    return;
  }

  const std::vector<rcc::MachineBasicBlockLiveIn> OriginalLiveIns(PhiBlock.liveIns().begin(),
                                                                  PhiBlock.liveIns().end());
  const rcc::RegisterOperandFlags OriginalFlags = (*Phi)->operandAt(1)->registerFlags();
  auto Rebuilt = rcc::rebuildMachineLiveness(*Fixture->Function, Target);
  Context.expect(!Rebuilt && Rebuilt.error().code() == rcc::ErrorCode::VerificationError &&
                     std::ranges::equal(PhiBlock.liveIns(), OriginalLiveIns) &&
                     (*Phi)->operandAt(1)->registerFlags() == OriginalFlags,
                 "malformed PHI edges must fail without changing live-ins or operand flags");
}

class DummyAnalysis final {
public:
  struct Result final {};

  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

void testAnalysisRegistrationAndInvalidation(TestContext &Context,
                                             const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("analysis-invalidation", 7, 1);
  if (!Fixture) {
    Context.expect(false, "analysis-invalidation fixture creation must succeed");
    return;
  }
  auto Reg = createVReg(*Fixture->Function);
  auto Def = Reg ? buildDef(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg)
                 : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Reg.error())};
  auto Use = Reg ? buildUse(*Fixture->Function, Target, *Fixture->Blocks[0], *Reg)
                 : rcc::Expected<rcc::MachineInstr *>{std::unexpected(Reg.error())};
  if (!Reg || !Def || !Use) {
    Context.expect(false, "analysis-invalidation instructions must be built");
    return;
  }

  rcc::MachineAnalysisManager Analyses;
  auto FirstRegistration = rcc::registerStage11MachineAnalyses(Analyses);
  auto SecondRegistration = rcc::registerStage11MachineAnalyses(Analyses);
  auto Intervals = FirstRegistration
                       ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target)
                       : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                             std::unexpected(FirstRegistration.error())};
  Context.expect(FirstRegistration && SecondRegistration && Intervals &&
                     Analyses.cachedResultCount(*Fixture->Function) == 3,
                 "Stage 11 analyses must register idempotently and materialize dependencies");
  Analyses.invalidate(*Fixture->Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::Frame});
  Context.expect(Analyses.cachedResultCount(*Fixture->Function) == 3,
                 "debug-only changes must preserve all liveness analyses");
  Analyses.invalidate(*Fixture->Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::Operands});
  Context.expect(
      Analyses.cachedResultCount(*Fixture->Function) == 1 &&
          Analyses.hasCached(*Fixture->Function,
                             rcc::MachineAnalysisManager::id<rcc::SlotIndexesAnalysis>()),
      "operand changes must retain SlotIndexes while invalidating liveness dependents");
  auto Recomputed = Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target);
  Context.expect(Recomputed && Analyses.cachedResultCount(*Fixture->Function) == 3,
                 "invalidated liveness dependencies must recompute on demand");
  Analyses.invalidate(*Fixture->Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::BlockLayout});
  Context.expect(
      Analyses.cachedResultCount(*Fixture->Function) == 1 &&
          Analyses.hasCached(*Fixture->Function,
                             rcc::MachineAnalysisManager::id<rcc::LiveVariablesAnalysis>()),
      "layout changes must invalidate SlotIndexes and LiveIntervals only");

  rcc::MachineAnalysisManager Atomic;
  rcc::MachineAnalysisRegistration DummyRegistration;
  DummyRegistration.Name = "SlotIndexes";
  auto Dummy = Atomic.registerAnalysis<DummyAnalysis>(std::move(DummyRegistration));
  auto Rejected = rcc::registerStage11MachineAnalyses(Atomic);
  Context.expect(
      Dummy && !Rejected && Atomic.isRegistered(rcc::MachineAnalysisManager::id<DummyAnalysis>()) &&
          !Atomic.isRegistered(rcc::MachineAnalysisManager::id<rcc::SlotIndexesAnalysis>()) &&
          !Atomic.isRegistered(rcc::MachineAnalysisManager::id<rcc::LiveVariablesAnalysis>()),
      "failed standard-analysis registration must leave the manager unchanged");
}

void testRISCVLiveness(TestContext &Context) {
  auto Target = rcc::riscv::createTargetDescription();
  auto Fixture = createFunction("riscv-liveness", 8, 1);
  if (!Target || !Fixture) {
    Context.expect(false, "RV64GC liveness fixture creation must succeed");
    return;
  }
  auto X5 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X5);
  auto X6 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X6);
  auto X7 = rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X7);
  if (!X5 || !X6 || !X7) {
    Context.expect(false, "RV64GC physical registers must resolve");
    return;
  }
  auto BuildADDI = [&](rcc::PhysicalRegister Destination, rcc::PhysicalRegister Source,
                       std::int64_t Immediate) -> rcc::Expected<rcc::MachineInstr *> {
    rcc::MachineIRBuilder Builder(*Fixture->Function, *Target);
    auto Point = Builder.setInsertPoint(*Fixture->Blocks[0]);
    if (!Point)
      return std::unexpected(Point.error());
    auto Pending = Builder.buildInstr(
        rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADDI)));
    if (!Pending)
      return std::unexpected(Pending.error());
    auto Def = Pending->addDef(rcc::Register::fromPhysical(Destination));
    if (!Def)
      return std::unexpected(Def.error());
    auto Use = Pending->addUse(rcc::Register::fromPhysical(Source));
    if (!Use)
      return std::unexpected(Use.error());
    auto Imm = Pending->addImmediate(Immediate);
    if (!Imm)
      return std::unexpected(Imm.error());
    return Pending->build();
  };
  auto First = BuildADDI(*X6, *X5, 1);
  auto Second = BuildADDI(*X7, *X6, 2);
  if (!First || !Second) {
    Context.expect(false, "RV64GC ADDI instructions must be built");
    return;
  }
  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  auto Intervals = Registered
                       ? Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, *Target)
                       : rcc::Expected<rcc::LiveIntervalsAnalysis::Result *>{
                             std::unexpected(Registered.error())};
  const auto Unit =
      rcc::RegisterUnitID::fromValue(static_cast<std::uint32_t>(rcc::riscv::RegisterUnit::U_X6));
  Context.expect(Intervals && (*Intervals)->regUnitRange(Unit) != nullptr &&
                     (*Intervals)->regUnitRange(Unit)->segments().size() == 1,
                 "production RV64GC register units must participate in LiveIntervals");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();

  testLiveRange(Context);
  testSlotIndexes(Context, *Target);
  testCFGAndValueNumbers(Context, *Target);
  testLaneSubrangesAndEarlyClobber(Context, *Target);
  testPhysicalUnitsAndRegMasks(Context, *Target);
  testLivenessRebuildAndVerifier(Context, *Target);
  testFailedRebuildIsAtomic(Context, *Target);
  testAnalysisRegistrationAndInvalidation(Context, *Target);
  testRISCVLiveness(Context);
  return Context.result();
}
