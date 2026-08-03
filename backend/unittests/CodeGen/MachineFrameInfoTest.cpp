// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineFrameInfo.h"

#include "rcc/CodeGen/MachineDebugInfo.h"

#include <cstdint>
#include <iostream>
#include <optional>
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

[[nodiscard]] rcc::StackObjectDesc fixedObject(std::uint64_t Size, std::uint64_t Align) {
  rcc::StackObjectDesc Description;
  Description.Size = Size;
  Description.Align = *rcc::Alignment::create(Align);
  return Description;
}

void testObjectLifecycle(TestContext &Context) {
  rcc::MachineFrameInfo Frame;

  auto LocalDescription = fixedObject(8, 8);
  LocalDescription.IsAliased = true;
  LocalDescription.AddressTaken = true;
  LocalDescription.Lifetime.push_back(
      {rcc::MachineInstrStableID::fromValue(3), rcc::MachineInstrStableID::fromValue(7)});
  auto Local = Frame.createObject(LocalDescription);

  auto FixedDescription = fixedObject(16, 16);
  FixedDescription.Kind = rcc::StackObjectKind::FixedABI;
  FixedDescription.Offset = -32;
  auto Fixed = Frame.createObject(FixedDescription);

  auto SpillDescription = fixedObject(8, 8);
  SpillDescription.Kind = rcc::StackObjectKind::SpillSlot;
  auto Spill = Frame.createObject(SpillDescription);

  rcc::StackObjectDesc VariableDescription;
  VariableDescription.Kind = rcc::StackObjectKind::VariableSized;
  VariableDescription.Align = *rcc::Alignment::create(32);
  auto Variable = Frame.createObject(VariableDescription);

  Context.expect(Local && Fixed && Spill && Variable, "valid stack object kinds must be created");
  if (!Local || !Fixed || !Spill || !Variable)
    return;

  Context.expect(Local->value() == 0 && Fixed->value() == 1 && Spill->value() == 2 &&
                     Variable->value() == 3 && Frame.liveObjectCount() == 4,
                 "frame indices must be monotonic table identities");
  Context.expect(Frame.object(*Fixed)->isFixed() && Frame.object(*Spill)->isSpill() &&
                     Frame.object(*Variable)->isVariableSized(),
                 "stack object semantic categories must be derived exactly");

  Context.expect(Frame.setObjectOffset(*Local, -8).has_value() &&
                     Frame.setObjectOffset(*Spill, -16).has_value() &&
                     Frame.markObjectDead(*Spill).has_value(),
                 "live stack objects must support controlled pre-freeze updates");
  Context.expect(Frame.object(*Spill)->description()->IsDead,
                 "dead stack object state must remain explicit");

  Context.expect(Frame.eraseObject(*Fixed).has_value() && Frame.object(*Fixed) == nullptr &&
                     Frame.liveObjectCount() == 3,
                 "erasing a stack object must leave an inaccessible tombstone");
  auto Replacement = Frame.createObject(fixedObject(4, 4));
  Context.expect(Replacement && Replacement->value() == 4,
                 "erased frame indices must never be reused");
  if (!Replacement)
    return;
  Context.expect(Frame.setObjectOffset(*Replacement, -24).has_value(),
                 "replacement stack object must accept a final offset");

  Context.expect(Frame.setMaxCallFrameSize(24).has_value() &&
                     Frame.freeze(64, *rcc::Alignment::create(16)).has_value() &&
                     Frame.isFrozen() && Frame.staticStackSize() == 64 &&
                     Frame.stackAlignment().bytes() == 16 && Frame.maxCallFrameSize() == 24 &&
                     Frame.verify().has_value(),
                 "complete frame layout must freeze atomically");

  Context.expect(!Frame.createObject(fixedObject(8, 8)) && !Frame.setObjectOffset(*Local, -40) &&
                     !Frame.eraseObject(*Local) && !Frame.setMaxCallFrameSize(32) &&
                     !Frame.freeze(64, *rcc::Alignment::create(16)),
                 "frozen frames must reject every semantic mutation");
}

void testInvalidDescriptions(TestContext &Context) {
  rcc::MachineFrameInfo Frame;

  rcc::StackObjectDesc MissingSize;
  Context.expect(!Frame.createObject(MissingSize),
                 "fixed-size stack object must reject a missing size");

  auto ZeroSize = fixedObject(0, 1);
  Context.expect(!Frame.createObject(ZeroSize), "fixed-size stack object must reject zero size");

  rcc::StackObjectDesc InvalidVariable;
  InvalidVariable.Kind = rcc::StackObjectKind::DynamicAlloca;
  InvalidVariable.Size = 8;
  Context.expect(!Frame.createObject(InvalidVariable),
                 "variable-sized stack object must reject a fixed size");

  auto MissingFixedOffset = fixedObject(8, 8);
  MissingFixedOffset.Kind = rcc::StackObjectKind::FixedABI;
  Context.expect(!Frame.createObject(MissingFixedOffset),
                 "fixed ABI stack object must require an offset");

  auto InvalidTarget = fixedObject(8, 8);
  InvalidTarget.Kind = rcc::StackObjectKind::Target;
  Context.expect(!Frame.createObject(InvalidTarget),
                 "target stack object must carry target identity");

  auto InvalidLifetime = fixedObject(8, 8);
  InvalidLifetime.Lifetime.push_back(
      {rcc::MachineInstrStableID::fromValue(4), rcc::MachineInstrStableID::fromValue(4)});
  Context.expect(!Frame.createObject(InvalidLifetime),
                 "stack object lifetime must reject equal boundaries");

  auto InvalidDebugInfo = fixedObject(8, 8);
  InvalidDebugInfo.DebugInfo.emplace();
  Context.expect(!Frame.createObject(InvalidDebugInfo),
                 "stack object debug information must not be structurally empty");

  auto DebugObject = fixedObject(8, 8);
  DebugObject.DebugInfo = rcc::MachineVariableDebugInfo{
      .Variable = rcc::MachineMetadataReference::embeddedIR(1),
      .Expression = rcc::MachineMetadataValue::expression(rcc::MachineDIExpression{}),
      .Location = std::nullopt,
  };
  auto DebugIndex = Frame.createObject(DebugObject);
  Context.expect(DebugIndex.has_value() &&
                     Frame.object(*DebugIndex)->description()->DebugInfo == DebugObject.DebugInfo,
                 "stack object debug information must remain structured");

  Context.expect(
      rcc::StackID::fromValue(0) && rcc::StackID::fromValue(4) && rcc::StackID::fromValue(255) &&
          !rcc::StackID::fromValue(5) && !rcc::StackID::fromValue(256) &&
          rcc::parseStackIDName("sgpr-spill") == rcc::StackID::sgprSpillStack() &&
          rcc::parseStackIDName("scalable-vector") == rcc::StackID::scalableVectorStack() &&
          rcc::parseStackIDName("scalable-predicate-vector") ==
              rcc::StackID::scalablePredicateVectorStack() &&
          rcc::parseStackIDName("wasm-local") == rcc::StackID::wasmLocalStack() &&
          rcc::parseStackIDName("noalloc") == rcc::StackID::noAllocStack() &&
          !rcc::parseStackIDName("safe") &&
          rcc::getStackIDName(rcc::StackID::noAllocStack()) == "noalloc",
      "stack IDs and names must match the fixed LLVM MIR enumeration");

  auto Target = fixedObject(8, 8);
  Target.Kind = rcc::StackObjectKind::Target;
  Target.Stack = rcc::StackID::sgprSpillStack();
  auto TargetIndex = Frame.createObject(Target);
  Context.expect(TargetIndex.has_value(), "special stack identity must admit target stack objects");

  auto Unresolved = Frame.createObject(fixedObject(8, 8));
  Context.expect(Unresolved && !Frame.freeze(16, *rcc::Alignment::create(8)) && !Frame.isFrozen() &&
                     !Frame.staticStackSize() && Frame.verify().has_value(),
                 "failed frame freeze must not mutate frame state");
}

} // namespace

int main() {
  TestContext Context;
  testObjectLifecycle(Context);
  testInvalidDescriptions(Context);
  return Context.result();
}
