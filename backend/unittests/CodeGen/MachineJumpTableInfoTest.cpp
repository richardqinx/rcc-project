// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineJumpTableInfo.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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

[[nodiscard]] std::unique_ptr<rcc::MachineFunction>
createFunction(TestContext &Context, std::string Name, std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  Context.expect(Function.has_value(), "machine function creation must succeed");
  if (!Function)
    return nullptr;
  return std::move(*Function);
}

[[nodiscard]] rcc::MachineBasicBlock *createBlock(TestContext &Context,
                                                  rcc::MachineFunction &Function) {
  auto Block = Function.createBasicBlock();
  Context.expect(Block.has_value() && Function.appendBlock(**Block).has_value(),
                 "jump table test block creation must succeed");
  return Block ? *Block : nullptr;
}

[[nodiscard]] rcc::MachineJumpTableDesc tableDescription(rcc::MachineBasicBlock &First,
                                                         rcc::MachineBasicBlock &Second) {
  rcc::MachineJumpTableDesc Description;
  Description.Encoding = rcc::JumpTableEntryEncoding::RelativeToTableBase;
  Description.EntrySize = 4;
  Description.Align = *rcc::Alignment::create(4);
  Description.Targets = {&First, &Second, &Second};
  Description.Placement.MaximumDistance = 4096;
  return Description;
}

void testTableLifecycle(TestContext &Context) {
  auto Function = createFunction(Context, "jump_tables", 1);
  if (!Function)
    return;
  rcc::MachineBasicBlock *Entry = createBlock(Context, *Function);
  rcc::MachineBasicBlock *Exit = createBlock(Context, *Function);
  if (Entry == nullptr || Exit == nullptr)
    return;

  auto First = Function->jumpTableInfo().createTable(tableDescription(*Entry, *Exit));
  auto TargetDescription = tableDescription(*Exit, *Entry);
  TargetDescription.Encoding = rcc::JumpTableEntryEncoding::Target;
  TargetDescription.TargetEncoding = 19;
  TargetDescription.Placement.TargetFlags = 7;
  auto Target = Function->jumpTableInfo().createTable(std::move(TargetDescription));
  Context.expect(First && Target && First->value() == 0 && Target->value() == 1 &&
                     Function->jumpTableInfo().liveTableCount() == 2,
                 "valid jump tables must receive monotonic indices");
  if (!First || !Target)
    return;

  const rcc::MachineJumpTableEntry *FirstEntry = Function->jumpTableInfo().table(*First);
  Context.expect(FirstEntry != nullptr && FirstEntry->description()->Targets.size() == 3 &&
                     FirstEntry->description()->Targets[1] == Exit &&
                     FirstEntry->description()->Targets[2] == Exit,
                 "jump table target order and duplicate cases must be preserved");

  Context.expect(Function->jumpTableInfo().eraseTable(*First).has_value() &&
                     Function->jumpTableInfo().table(*First) == nullptr,
                 "erasing a jump table must leave a tombstone");
  auto CompressedDescription = tableDescription(*Entry, *Exit);
  CompressedDescription.Encoding = rcc::JumpTableEntryEncoding::CompressedOffset;
  CompressedDescription.EntrySize = 2;
  auto Compressed = Function->jumpTableInfo().createTable(std::move(CompressedDescription));
  Context.expect(Compressed && Compressed->value() == 2,
                 "jump table tombstone indices must never be reused");

  auto Updated = tableDescription(*Entry, *Exit);
  Updated.Encoding = rcc::JumpTableEntryEncoding::RelativeToEntry;
  Context.expect(Function->jumpTableInfo().updateTable(*Target, std::move(Updated)).has_value() &&
                     Function->jumpTableInfo().verify().has_value() &&
                     Function->jumpTableInfo().freeze().has_value() &&
                     Function->jumpTableInfo().isFrozen(),
                 "valid jump table updates must verify and freeze");
  Context.expect(
      !Function->jumpTableInfo().eraseTable(*Target) &&
          !Function->jumpTableInfo().updateTable(*Target, tableDescription(*Entry, *Exit)) &&
          !Function->jumpTableInfo().createTable(tableDescription(*Entry, *Exit)) &&
          !Function->jumpTableInfo().freeze(),
      "frozen jump table info must reject every mutation");
}

void testInvalidTables(TestContext &Context) {
  auto Function = createFunction(Context, "invalid_tables", 2);
  auto Foreign = createFunction(Context, "foreign_tables", 3);
  if (!Function || !Foreign)
    return;
  rcc::MachineBasicBlock *Block = createBlock(Context, *Function);
  rcc::MachineBasicBlock *ForeignBlock = createBlock(Context, *Foreign);
  if (Block == nullptr || ForeignBlock == nullptr)
    return;

  rcc::MachineJumpTableDesc Empty;
  Context.expect(!Function->jumpTableInfo().createTable(std::move(Empty)),
                 "jump table must reject an empty target list");

  auto ForeignTarget = tableDescription(*Block, *ForeignBlock);
  Context.expect(!Function->jumpTableInfo().createTable(std::move(ForeignTarget)),
                 "jump table must reject a foreign target block");

  auto InvalidSize = tableDescription(*Block, *Block);
  InvalidSize.EntrySize = 3;
  Context.expect(!Function->jumpTableInfo().createTable(std::move(InvalidSize)),
                 "jump table entry size must be a bounded power of two");

  auto MissingTargetEncoding = tableDescription(*Block, *Block);
  MissingTargetEncoding.Encoding = rcc::JumpTableEntryEncoding::Target;
  Context.expect(!Function->jumpTableInfo().createTable(std::move(MissingTargetEncoding)),
                 "target jump table must carry a target encoding ID");

  auto UnexpectedTargetEncoding = tableDescription(*Block, *Block);
  UnexpectedTargetEncoding.TargetEncoding = 1;
  Context.expect(!Function->jumpTableInfo().createTable(std::move(UnexpectedTargetEncoding)),
                 "generic jump table encoding must reject target encoding IDs");

  auto OversizedCompressed = tableDescription(*Block, *Block);
  OversizedCompressed.Encoding = rcc::JumpTableEntryEncoding::CompressedOffset;
  OversizedCompressed.EntrySize = 8;
  Context.expect(!Function->jumpTableInfo().createTable(std::move(OversizedCompressed)),
                 "compressed jump table must reject oversized entries");

  auto ZeroDistance = tableDescription(*Block, *Block);
  ZeroDistance.Placement.MaximumDistance = 0;
  Context.expect(!Function->jumpTableInfo().createTable(std::move(ZeroDistance)),
                 "jump table maximum distance must be nonzero");

  auto LiveTable = Function->jumpTableInfo().createTable(tableDescription(*Block, *Block));
  Context.expect(LiveTable && Function->removeBlock(*Block).has_value() &&
                     !Function->jumpTableInfo().verify(),
                 "jump table verification must detect a detached target");
}

} // namespace

int main() {
  TestContext Context;
  testTableLifecycle(Context);
  testInvalidTables(Context);
  return Context.result();
}
