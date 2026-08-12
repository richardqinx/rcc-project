// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/SlotIndexes.h"

#include "LiveTestSupport.h"
#include "rcc/CodeGen/LiveVariables.h"

namespace {

using namespace rcc::test::liveness;

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

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  Context.expect(Target.has_value(), "liveness TestTarget must be created");
  if (!Target)
    return Context.result();
  testSlotIndexes(Context, *Target);
  return Context.result();
}
