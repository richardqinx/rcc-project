// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineAnalyses.h"

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
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

[[nodiscard]] rcc::Expected<rcc::TargetDescription> createTarget() {
  std::vector<rcc::InstructionDescriptor> Descriptors;
  std::vector<rcc::ImmediatePredicate> Predicates;
  auto Appended = rcc::appendGenericInstructionInfo(Descriptors, Predicates);
  if (!Appended)
    return std::unexpected(Appended.error());
  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors), std::move(Predicates));
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
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

void testCFGAnalyses(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function = rcc::MachineFunction::create("cfg", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Function.has_value(), "CFG analysis function creation must succeed");
  if (!Function)
    return;

  auto Entry = appendBlock(**Function);
  auto Header = appendBlock(**Function);
  auto Body = appendBlock(**Function);
  auto Exit = appendBlock(**Function);
  auto Dead = appendBlock(**Function);
  Context.expect(Entry && Header && Body && Exit && Dead,
                 "CFG analysis blocks must be appended successfully");
  if (!Entry || !Header || !Body || !Exit || !Dead)
    return;

  auto ThreeQuarters = rcc::BranchProbability::fromFraction(3, 4);
  auto OneQuarter = rcc::BranchProbability::fromFraction(1, 4);
  auto Updater = (*Function)->cfgUpdater();
  Context.expect(ThreeQuarters && OneQuarter &&
                     Updater.addSuccessor(**Entry, **Header, rcc::BranchProbability::one()) &&
                     Updater.addSuccessor(**Header, **Body, *ThreeQuarters) &&
                     Updater.addSuccessor(**Header, **Exit, *OneQuarter) &&
                     Updater.addSuccessor(**Body, **Header, rcc::BranchProbability::one()),
                 "reducible loop CFG construction must succeed");

  rcc::MachineAnalysisManager Analyses;
  Context.expect(rcc::registerStage8MachineAnalyses(Analyses).has_value(),
                 "standard Stage 8 analyses must register atomically");
  auto Dominators = Analyses.getResult<rcc::MachineDominatorTreeAnalysis>(**Function, Target);
  auto Loops = Analyses.getResult<rcc::MachineLoopAnalysis>(**Function, Target);
  auto Probabilities =
      Analyses.getResult<rcc::MachineBranchProbabilityAnalysis>(**Function, Target);
  auto Frequencies = Analyses.getResult<rcc::MachineBlockFrequencyAnalysis>(**Function, Target);
  Context.expect(Dominators && Loops && Probabilities && Frequencies,
                 "CFG analyses and their dependencies must materialize");
  if (!Dominators || !Loops || !Probabilities || !Frequencies)
    return;

  Context.expect((*Dominators)->dominates(**Entry, **Exit) &&
                     (*Dominators)->dominates(**Header, **Body) &&
                     (*Dominators)->immediateDominator(**Body) == *Header &&
                     (*Dominators)->nearestCommonDominator(**Body, **Exit) == *Header &&
                     !(*Dominators)->isReachable(**Dead),
                 "dominator analysis must model reachability, IDom, and common dominators");
  Context.expect((*Loops)->loops().size() == 1 && (*Loops)->loopFor(**Header) != nullptr &&
                     (*Loops)->loopFor(**Body) != nullptr && (*Loops)->loopDepth(**Body) == 1,
                 "loop analysis must recover the natural loop and loop depth");
  Context.expect((*Probabilities)->probability(**Header, **Body) == *ThreeQuarters &&
                     (*Probabilities)->probability(**Header, **Exit) == *OneQuarter,
                 "branch-probability analysis must retain exact integer edge probabilities");
  Context.expect((*Frequencies)->frequency(**Entry) != 0 &&
                     (*Frequencies)->frequency(**Header) > (*Frequencies)->frequency(**Entry) &&
                     (*Frequencies)->frequency(**Dead) == 0,
                 "block-frequency analysis must distinguish loop and unreachable blocks");

  auto Alias = Analyses.getResult<rcc::MachineAliasAnalysis>(**Function, Target);
  auto MemoryDependence =
      Analyses.getResult<rcc::MachineMemoryDependenceAnalysis>(**Function, Target);
  Context.expect(Alias && MemoryDependence && Analyses.cachedResultCount(**Function) == 6,
                 "the complete Stage 8 analysis set must materialize into one function cache");
  Analyses.invalidate(**Function, rcc::PreservedAnalyses::none(),
                      {rcc::MachinePassChange::Registers});
  Context.expect(Analyses.cachedResultCount(**Function) == 6,
                 "register-only changes must preserve CFG and memory analyses");
  Analyses.invalidate(**Function, rcc::PreservedAnalyses::none(), {rcc::MachinePassChange::CFG});
  Context.expect(
      Analyses.cachedResultCount(**Function) == 1 &&
          Analyses.hasCached(**Function,
                             rcc::MachineAnalysisManager::id<rcc::MachineAliasAnalysis>()),
      "CFG changes must invalidate CFG dependents and memory dependence but preserve aliasing");

  auto RefreshedDominators =
      Analyses.getResult<rcc::MachineDominatorTreeAnalysis>(**Function, Target);
  Context.expect(
      RefreshedDominators &&
          Updater.addSuccessor(**Entry, **Exit, rcc::BranchProbability::zero()).has_value() &&
          (*RefreshedDominators)->updateAfterCFGChange(**Function).has_value() &&
          (*RefreshedDominators)->immediateDominator(**Exit) == *Entry,
      "dominator result must support exact updates after CFG edge changes");
}

void testIrreducibleCFG(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function =
      rcc::MachineFunction::create("irreducible", rcc::MachineFunctionStableID::fromValue(2));
  if (!Function) {
    Context.expect(false, "irreducible CFG function creation must succeed");
    return;
  }
  auto Entry = appendBlock(**Function);
  auto Left = appendBlock(**Function);
  auto Right = appendBlock(**Function);
  auto Exit = appendBlock(**Function);
  if (!Entry || !Left || !Right || !Exit) {
    Context.expect(false, "irreducible CFG blocks must be appended");
    return;
  }
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  auto Updater = (*Function)->cfgUpdater();
  Context.expect(Half && Updater.addSuccessor(**Entry, **Left, *Half) &&
                     Updater.addSuccessor(**Entry, **Right, *Half) &&
                     Updater.addSuccessor(**Left, **Right, *Half) &&
                     Updater.addSuccessor(**Left, **Exit, *Half) &&
                     Updater.addSuccessor(**Right, **Left, *Half) &&
                     Updater.addSuccessor(**Right, **Exit, *Half),
                 "irreducible CFG construction must succeed");

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage8MachineAnalyses(Analyses);
  auto Loops =
      Registered
          ? Analyses.getResult<rcc::MachineLoopAnalysis>(**Function, Target)
          : rcc::Expected<rcc::MachineLoopAnalysis::Result *>{std::unexpected(Registered.error())};
  Context.expect(Loops && (*Loops)->irreducibleRegions().size() == 1 &&
                     (*Loops)->irreducibleRegions().front().Entries.size() == 2 &&
                     (*Loops)->isIrreducible(**Left) && (*Loops)->isIrreducible(**Right),
                 "loop analysis must explicitly identify multi-entry irreducible regions");
}

[[nodiscard]] rcc::MachineMemOperandDesc
memoryDescription(rcc::MemoryBase Base, std::int64_t Offset, rcc::MemoryAccessFlag Access) {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.Base = std::move(Base);
  Description.Offset = Offset;
  return Description;
}

void testAliasAnalysis(TestContext &Context) {
  auto First = rcc::MachineMemOperand::create(memoryDescription(
      rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0)), 0, rcc::MemoryAccessFlag::Load));
  auto Same = rcc::MachineMemOperand::create(memoryDescription(
      rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0)), 0, rcc::MemoryAccessFlag::Store));
  auto Partial = rcc::MachineMemOperand::create(memoryDescription(
      rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0)), 4, rcc::MemoryAccessFlag::Store));
  auto DisjointRange = rcc::MachineMemOperand::create(
      memoryDescription(rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(0)), 16,
                        rcc::MemoryAccessFlag::Store));
  auto DistinctObject = rcc::MachineMemOperand::create(memoryDescription(
      rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(1)), 0, rcc::MemoryAccessFlag::Store));
  auto DifferentObjectKind = rcc::MachineMemOperand::create(
      memoryDescription(rcc::MemoryBase::constantPool(rcc::ConstantPoolIndex::fromValue(0)), 0,
                        rcc::MemoryAccessFlag::Store));
  Context.expect(First && Same && Partial && DisjointRange && DistinctObject && DifferentObjectKind,
                 "alias-analysis memory operands must be valid");
  if (!First || !Same || !Partial || !DisjointRange || !DistinctObject || !DifferentObjectKind)
    return;

  rcc::MachineAliasAnalysis::Result Alias;
  Context.expect(Alias.alias(*First, *Same) == rcc::MachineAliasResult::MustAlias &&
                     Alias.alias(*First, *Partial) == rcc::MachineAliasResult::PartialAlias &&
                     Alias.alias(*First, *DisjointRange) == rcc::MachineAliasResult::NoAlias &&
                     Alias.alias(*First, *DistinctObject) == rcc::MachineAliasResult::NoAlias &&
                     Alias.alias(*First, *DifferentObjectKind) == rcc::MachineAliasResult::NoAlias,
                 "alias analysis must distinguish exact, partial, range, and object aliasing");
}

void testMemoryDependence(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function =
      rcc::MachineFunction::create("memory", rcc::MachineFunctionStableID::fromValue(3));
  if (!Function) {
    Context.expect(false, "memory-dependence function creation must succeed");
    return;
  }
  auto Block = appendBlock(**Function);
  auto OtherBlock = appendBlock(**Function);
  auto I64 = rcc::LLT::makeInteger(64);
  auto Pointer = rcc::LLT::makePointer(0, 64);
  auto Value = I64 ? (*Function)->registerInfo().createVirtualRegister(*I64)
                   : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  auto Address = Pointer ? (*Function)->registerInfo().createVirtualRegister(*Pointer)
                         : rcc::Expected<rcc::Register>{std::unexpected(Pointer.error())};
  if (!Block || !OtherBlock || !Value || !Address) {
    Context.expect(false, "memory-dependence MachineIR setup must succeed");
    return;
  }

  auto StoreMemory = (*Function)->createMemOperand(
      memoryDescription(rcc::MemoryBase::stack(), 0, rcc::MemoryAccessFlag::Store));
  auto AliasingLoadMemory = (*Function)->createMemOperand(
      memoryDescription(rcc::MemoryBase::stack(), 0, rcc::MemoryAccessFlag::Load));
  auto DisjointLoadMemory = (*Function)->createMemOperand(
      memoryDescription(rcc::MemoryBase::stack(), 32, rcc::MemoryAccessFlag::Load));
  auto VolatileStoreDescription =
      memoryDescription(rcc::MemoryBase::stack(), 64, rcc::MemoryAccessFlag::Store);
  VolatileStoreDescription.Accesses.set(rcc::MemoryAccessFlag::Volatile);
  auto VolatileLoadDescription =
      memoryDescription(rcc::MemoryBase::stack(), 96, rcc::MemoryAccessFlag::Load);
  VolatileLoadDescription.Accesses.set(rcc::MemoryAccessFlag::Volatile);
  auto VolatileStoreMemory = (*Function)->createMemOperand(std::move(VolatileStoreDescription));
  auto VolatileLoadMemory = (*Function)->createMemOperand(std::move(VolatileLoadDescription));
  auto OtherBlockLoadMemory = (*Function)->createMemOperand(
      memoryDescription(rcc::MemoryBase::stack(), 0, rcc::MemoryAccessFlag::Load));
  rcc::MachineIRBuilder Builder(**Function, Target);
  Context.expect(StoreMemory && AliasingLoadMemory && DisjointLoadMemory && VolatileStoreMemory &&
                     VolatileLoadMemory && OtherBlockLoadMemory && Builder.setInsertPoint(**Block),
                 "memory-dependence builder setup must succeed");
  if (!StoreMemory || !AliasingLoadMemory || !DisjointLoadMemory || !VolatileStoreMemory ||
      !VolatileLoadMemory || !OtherBlockLoadMemory)
    return;

  auto Store = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_STORE));
  if (!Store || !Store->addUse(*Value) || !Store->addUse(*Address) ||
      !Store->addMemOperand(**StoreMemory)) {
    Context.expect(false, "generic store construction must succeed");
    return;
  }
  auto StoreInstruction = Store->build();
  auto Load = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_LOAD));
  if (!Load || !Load->addDef(*Value) || !Load->addUse(*Address) ||
      !Load->addMemOperand(**AliasingLoadMemory)) {
    Context.expect(false, "aliasing generic load construction must succeed");
    return;
  }
  auto LoadInstruction = Load->build();
  auto DisjointLoad = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_LOAD));
  if (!DisjointLoad || !DisjointLoad->addDef(*Value) || !DisjointLoad->addUse(*Address) ||
      !DisjointLoad->addMemOperand(**DisjointLoadMemory)) {
    Context.expect(false, "disjoint generic load construction must succeed");
    return;
  }
  auto DisjointLoadInstruction = DisjointLoad->build();
  auto VolatileStore = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_STORE));
  if (!VolatileStore || !VolatileStore->addUse(*Value) || !VolatileStore->addUse(*Address) ||
      !VolatileStore->addMemOperand(**VolatileStoreMemory)) {
    Context.expect(false, "volatile store construction must succeed");
    return;
  }
  auto VolatileStoreInstruction = VolatileStore->build();
  auto VolatileLoad = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_LOAD));
  if (!VolatileLoad || !VolatileLoad->addDef(*Value) || !VolatileLoad->addUse(*Address) ||
      !VolatileLoad->addMemOperand(**VolatileLoadMemory)) {
    Context.expect(false, "volatile load construction must succeed");
    return;
  }
  auto VolatileLoadInstruction = VolatileLoad->build();
  if (!Builder.setInsertPoint(**OtherBlock)) {
    Context.expect(false, "second memory region insertion point must be valid");
    return;
  }
  auto OtherBlockLoad = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_LOAD));
  if (!OtherBlockLoad || !OtherBlockLoad->addDef(*Value) || !OtherBlockLoad->addUse(*Address) ||
      !OtherBlockLoad->addMemOperand(**OtherBlockLoadMemory)) {
    Context.expect(false, "second-region generic load construction must succeed");
    return;
  }
  auto OtherBlockLoadInstruction = OtherBlockLoad->build();
  Context.expect(StoreInstruction && LoadInstruction && DisjointLoadInstruction &&
                     VolatileStoreInstruction && VolatileLoadInstruction &&
                     OtherBlockLoadInstruction,
                 "memory instructions must be published transactionally");
  if (!StoreInstruction || !LoadInstruction || !DisjointLoadInstruction ||
      !VolatileStoreInstruction || !VolatileLoadInstruction || !OtherBlockLoadInstruction)
    return;

  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage8MachineAnalyses(Analyses);
  auto Dependencies =
      Registered ? Analyses.getResult<rcc::MachineMemoryDependenceAnalysis>(**Function, Target)
                 : rcc::Expected<rcc::MachineMemoryDependenceAnalysis::Result *>{
                       std::unexpected(Registered.error())};
  Context.expect(Dependencies && (*Dependencies)->dependencies(**LoadInstruction).size() == 1 &&
                     (*Dependencies)->dependencies(**LoadInstruction).front().Source ==
                         (*StoreInstruction)->stableID() &&
                     (*Dependencies)->dependencies(**DisjointLoadInstruction).empty(),
                 "memory dependence must add flow edges only for potentially aliasing accesses");
  Context.expect(
      Dependencies && (*Dependencies)->regions().size() == 2 &&
          (*Dependencies)->regionFor(**Block) != nullptr &&
          (*Dependencies)->regionFor(**Block)->Instructions.size() == 5 &&
          (*Dependencies)->regionFor(**OtherBlock) != nullptr &&
          (*Dependencies)->regionFor(**OtherBlock)->Instructions.size() == 1 &&
          (*Dependencies)->dependencies(**OtherBlockLoadInstruction).empty() &&
          (*Dependencies)->dependencies(**OtherBlock).empty(),
      "memory dependence must expose deterministic block regions without cross-CFG edges");
  bool HasOrderedVolatileDependency = false;
  if (Dependencies) {
    HasOrderedVolatileDependency =
        std::ranges::any_of((*Dependencies)->dependencies(**VolatileLoadInstruction),
                            [&](const rcc::MachineMemoryDependency &Dependency) {
                              return Dependency.Source == (*VolatileStoreInstruction)->stableID();
                            });
  }
  Context.expect(HasOrderedVolatileDependency,
                 "ordered memory accesses must retain dependencies across disjoint addresses");
}

class ConflictingAnalysis final {
public:
  struct Result final {};
  [[nodiscard]] static rcc::Expected<Result>
  run(rcc::MachineFunction &, const rcc::TargetDescription &, rcc::MachineAnalysisManager &) {
    return Result{};
  }
};

void testAtomicStandardRegistration(TestContext &Context) {
  rcc::MachineAnalysisManager Analyses;
  Context.expect(Analyses
                     .registerAnalysis<ConflictingAnalysis>({
                         .Name = "MachineLoopInfo",
                         .Dependencies = {},
                     })
                     .has_value(),
                 "analysis-name conflict setup must succeed");
  Context.expect(!rcc::registerStage8MachineAnalyses(Analyses) &&
                     !Analyses.isRegistered(
                         rcc::MachineAnalysisManager::id<rcc::MachineDominatorTreeAnalysis>()),
                 "failed standard-analysis registration must not publish a partial set");
}

void testCFGUpdaterPHIs(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Function =
      rcc::MachineFunction::create("cfg_phi", rcc::MachineFunctionStableID::fromValue(4));
  if (!Function) {
    Context.expect(false, "CFG PHI function creation must succeed");
    return;
  }
  auto Source = appendBlock(**Function);
  auto Other = appendBlock(**Function);
  auto Merge = appendBlock(**Function);
  auto Unconnected = appendBlock(**Function);
  if (!Source || !Other || !Merge || !Unconnected) {
    Context.expect(false, "CFG PHI blocks must be appended");
    return;
  }
  auto Updater = (*Function)->cfgUpdater();
  if (!Updater.addSuccessor(**Source, **Merge, rcc::BranchProbability::one()) ||
      !Updater.addSuccessor(**Other, **Merge, rcc::BranchProbability::one())) {
    Context.expect(false, "CFG PHI predecessor edges must be created");
    return;
  }

  auto I64 = rcc::LLT::makeInteger(64);
  auto Destination = I64 ? (*Function)->registerInfo().createVirtualRegister(*I64)
                         : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  auto SourceValue = I64 ? (*Function)->registerInfo().createVirtualRegister(*I64)
                         : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  auto OtherValue = I64 ? (*Function)->registerInfo().createVirtualRegister(*I64)
                        : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  if (!Destination || !SourceValue || !OtherValue) {
    Context.expect(false, "CFG PHI registers must be created");
    return;
  }

  rcc::MachineIRBuilder Builder(**Function, Target);
  if (!Builder.setInsertPoint(**Source))
    return;
  auto SourceDefinition = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_IMPLICIT_DEF));
  if (!SourceDefinition || !SourceDefinition->addDef(*SourceValue) || !SourceDefinition->build()) {
    Context.expect(false, "CFG PHI source definition must build");
    return;
  }
  auto Branch = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_BR));
  if (!Branch || !Branch->addBasicBlock(**Merge)) {
    Context.expect(false, "CFG PHI branch must be assembled");
    return;
  }
  auto BranchInstruction = Branch->build();
  if (!BranchInstruction || !Builder.setInsertPoint(**Merge)) {
    Context.expect(false, "CFG PHI branch must publish");
    return;
  }
  auto Phi = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_PHI));
  if (!Phi || !Phi->addDef(*Destination) || !Phi->addUse(*SourceValue) ||
      !Phi->addBasicBlock(**Source) || !Phi->addUse(*OtherValue) || !Phi->addBasicBlock(**Other)) {
    Context.expect(false, "CFG PHI instruction must be assembled");
    return;
  }
  auto PhiInstruction = Phi->build();
  Context.expect(PhiInstruction && rcc::MachineVerifier::verify(**Function, Target),
                 "initial CFG PHI function must verify");
  if (!PhiInstruction)
    return;

  auto Split = Updater.splitBlock(**Source, **BranchInstruction);
  Context.expect(
      Split && (*Split)->predecessors().size() == 1 &&
          (*Split)->predecessors().front() == *Source && (*Split)->successors().size() == 1 &&
          (*PhiInstruction)->operandAt(2)->getIf<rcc::BasicBlockOperand>()->Block == *Split &&
          rcc::MachineVerifier::verify(**Function, Target),
      "block split must redirect CFG edges and PHI predecessor identity");
  if (!Split)
    return;

  Context.expect(Updater.removeSuccessor(**Other, **Merge) &&
                     (*PhiInstruction)->operandCount() == 3 &&
                     rcc::MachineVerifier::verify(**Function, Target),
                 "edge deletion must remove the corresponding PHI incoming pair");
  Context.expect(!Updater.addSuccessor(**Unconnected, **Merge, rcc::BranchProbability::one()),
                 "new PHI predecessors without incoming values must be rejected");
  Context.expect(Updater.deleteBlock(**Other) && !(*Other)->isLinked() &&
                     rcc::MachineVerifier::verify(**Function, Target),
                 "CFG block deletion must detach a disconnected block cleanly");
}

void testCFGUpdaterFailureAtomicity(TestContext &Context, const rcc::TargetDescription &Target) {
  auto SplitFunction =
      rcc::MachineFunction::create("cfg_split_atomic", rcc::MachineFunctionStableID::fromValue(5));
  if (!SplitFunction) {
    Context.expect(false, "CFG split atomicity function creation must succeed");
    return;
  }
  auto Source = appendBlock(**SplitFunction);
  auto Merge = appendBlock(**SplitFunction);
  if (!Source || !Merge)
    return;
  auto SplitUpdater = (*SplitFunction)->cfgUpdater();
  if (!SplitUpdater.addSuccessor(**Source, **Merge, rcc::BranchProbability::one()))
    return;

  auto I64 = rcc::LLT::makeInteger(64);
  auto Destination = I64 ? (*SplitFunction)->registerInfo().createVirtualRegister(*I64)
                         : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  auto Value = I64 ? (*SplitFunction)->registerInfo().createVirtualRegister(*I64)
                   : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  if (!Destination || !Value)
    return;

  rcc::MachineIRBuilder Builder(**SplitFunction, Target);
  if (!Builder.setInsertPoint(**Source))
    return;
  auto Branch = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_BR));
  auto BranchInstruction =
      Branch && Branch->addBasicBlock(**Merge)
          ? Branch->build()
          : rcc::Expected<rcc::MachineInstr *>{rcc::makeUnexpected(
                rcc::ErrorCode::InternalError, "failed to build CFG split branch")};
  if (!BranchInstruction || !Builder.setInsertPoint(**Merge))
    return;
  auto Phi = Builder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_PHI));
  auto PhiInstruction =
      Phi && Phi->addDef(*Destination) && Phi->addUse(*Value) && Phi->addBasicBlock(**Source)
          ? Phi->build()
          : rcc::Expected<rcc::MachineInstr *>{rcc::makeUnexpected(
                rcc::ErrorCode::InternalError, "failed to build CFG split PHI")};
  if (!PhiInstruction || !(*PhiInstruction)->eraseOperand(2))
    return;

  const std::size_t LinkedBlocksBefore = (*SplitFunction)->blockCount();
  const std::size_t AllocatedBlocksBefore = (*SplitFunction)->allBlocks().size();
  auto FailedSplit = SplitUpdater.splitBlock(**Source, **BranchInstruction);
  Context.expect(
      !FailedSplit && (*SplitFunction)->blockCount() == LinkedBlocksBefore &&
          (*SplitFunction)->allBlocks().size() == AllocatedBlocksBefore &&
          (*BranchInstruction)->parent() == *Source && (*Source)->successors().size() == 1 &&
          (*Source)->successors().front().Block == *Merge && (*Merge)->predecessors().size() == 1 &&
          (*Merge)->predecessors().front() == *Source,
      "failed block split must not mutate layout, instructions, or CFG edges");

  auto FailedDelete = SplitUpdater.deleteBlock(**Source);
  Context.expect(!FailedDelete && (*Source)->isLinked() &&
                     (*SplitFunction)->blockCount() == LinkedBlocksBefore &&
                     (*Source)->successors().size() == 1 && (*Merge)->predecessors().size() == 1,
                 "failed block deletion must not partially disconnect CFG edges");

  auto TransferFunction = rcc::MachineFunction::create("cfg_transfer_atomic",
                                                       rcc::MachineFunctionStableID::fromValue(6));
  if (!TransferFunction)
    return;
  auto From = appendBlock(**TransferFunction);
  auto To = appendBlock(**TransferFunction);
  auto Successor = appendBlock(**TransferFunction);
  if (!From || !To || !Successor)
    return;
  auto TransferUpdater = (*TransferFunction)->cfgUpdater();
  if (!TransferUpdater.addSuccessor(**From, **Successor, rcc::BranchProbability::one()) ||
      !TransferUpdater.addSuccessor(**To, **Successor, rcc::BranchProbability::one())) {
    return;
  }
  auto TransferDestination = I64 ? (*TransferFunction)->registerInfo().createVirtualRegister(*I64)
                                 : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  auto TransferValue = I64 ? (*TransferFunction)->registerInfo().createVirtualRegister(*I64)
                           : rcc::Expected<rcc::Register>{std::unexpected(I64.error())};
  if (!TransferDestination || !TransferValue)
    return;
  rcc::MachineIRBuilder TransferBuilder(**TransferFunction, Target);
  if (!TransferBuilder.setInsertPoint(**Successor))
    return;
  auto TransferPhi = TransferBuilder.buildInstr(rcc::opcode(rcc::GenericOpcode::G_PHI));
  auto TransferPhiInstruction =
      TransferPhi && TransferPhi->addDef(*TransferDestination) &&
              TransferPhi->addUse(*TransferValue) && TransferPhi->addBasicBlock(**From)
          ? TransferPhi->build()
          : rcc::Expected<rcc::MachineInstr *>{rcc::makeUnexpected(
                rcc::ErrorCode::InternalError, "failed to build CFG transfer PHI")};
  if (!TransferPhiInstruction)
    return;

  auto FailedTransfer = TransferUpdater.transferSuccessors(**From, **To);
  Context.expect(
      !FailedTransfer && (*From)->successors().size() == 1 && (*To)->successors().size() == 1 &&
          (*Successor)->predecessors().size() == 2 &&
          (*TransferPhiInstruction)->operandCount() == 3 &&
          (*TransferPhiInstruction)->operandAt(2)->getIf<rcc::BasicBlockOperand>()->Block == *From,
      "failed successor transfer must preserve both CFG edges and PHI operands");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "machine analysis tests require a valid target");
  if (!Target)
    return Context.result();

  testCFGAnalyses(Context, *Target);
  testIrreducibleCFG(Context, *Target);
  testAliasAnalysis(Context);
  testMemoryDependence(Context, *Target);
  testCFGUpdaterPHIs(Context, *Target);
  testCFGUpdaterFailureAtomicity(Context, *Target);
  testAtomicStandardRegistration(Context);
  return Context.result();
}
