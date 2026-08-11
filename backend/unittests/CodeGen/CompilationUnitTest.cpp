// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CompilationUnit.h"

#include "rcc/CodeGen/MachineFunction.h"

#include <cstddef>
#include <iostream>
#include <utility>

namespace {

int fail(const char *Message) {
  std::cerr << Message << '\n';
  return 1;
}

} // namespace

int main() {
  rcc::CompilationUnit Unit;
  auto FirstAnonymous = Unit.createMachineFunction("");
  auto SecondAnonymous = Unit.createMachineFunction("");
  if (!FirstAnonymous || !SecondAnonymous || Unit.functionCount() != 2 ||
      (*FirstAnonymous)->stableID().value() != 1 || (*SecondAnonymous)->stableID().value() != 2 ||
      Unit.function("") != nullptr)
    return fail("CompilationUnit must retain anonymous functions by order and StableID");

  auto First = Unit.createMachineFunction("first");
  if (!First || (*First)->stableID().value() != 3)
    return fail("CompilationUnit must allocate the first stable function identity");

  auto Duplicate = Unit.createMachineFunction("first");
  if (Duplicate || Unit.functionCount() != 3)
    return fail("CompilationUnit must reject duplicate function names atomically");

  auto Second = Unit.createMachineFunction("second");
  if (!Second || (*Second)->stableID().value() != 4)
    return fail("Rejected function creation must not consume a StableID");
  if (Unit.functionCount() != 4 || Unit.functionAt(0) != *FirstAnonymous ||
      Unit.functionAt(1) != *SecondAnonymous || Unit.functionAt(2) != *First ||
      Unit.functionAt(3) != *Second)
    return fail("CompilationUnit must preserve function document order");
  if (Unit.function("first") != *First || Unit.function("missing") != nullptr)
    return fail("CompilationUnit must provide exact name lookup");
  if (Unit.function((*Second)->stableID()) != *Second ||
      Unit.function(rcc::MachineFunctionStableID::fromValue(99)) != nullptr)
    return fail("CompilationUnit must provide exact StableID lookup");
  if (Unit.functionAt(4) != nullptr)
    return fail("CompilationUnit must reject an out-of-range ordered lookup");

  auto Zero = rcc::GlobalInitializer::zeroFill(4);
  if (!Zero)
    return fail("Global initializer creation must succeed");
  rcc::GlobalObjectDesc Object;
  Object.Symbol = "global";
  Object.ObjectType = rcc::GenericConstantType::byte();
  Object.Size = 4;
  Object.Initializer.emplace(std::move(*Zero));
  auto Global = Unit.addGlobalObject(std::move(Object));
  if (!Global || Unit.globalObjectCount() != 1 || Unit.globalObject("global") != *Global ||
      Unit.globalObjectAt(0) != *Global || Unit.globalObjectAt(1) != nullptr)
    return fail("CompilationUnit must own and index global objects");

  auto ConflictingFunction = Unit.createMachineFunction("global");
  if (ConflictingFunction || Unit.functionCount() != 4)
    return fail("CompilationUnit must reject function/global symbol conflicts atomically");

  rcc::GlobalObjectDesc ConflictingGlobal;
  ConflictingGlobal.Symbol = "first";
  ConflictingGlobal.ObjectType = rcc::GenericConstantType::byte();
  ConflictingGlobal.Size = 1;
  auto Byte = rcc::GlobalInitializer::bytes({std::byte{1}});
  if (!Byte)
    return fail("Byte initializer creation must succeed");
  ConflictingGlobal.Initializer.emplace(std::move(*Byte));
  if (Unit.addGlobalObject(std::move(ConflictingGlobal)) || Unit.globalObjectCount() != 1)
    return fail("CompilationUnit must reject global/function symbol conflicts atomically");

  auto MismatchedBytes = rcc::GlobalInitializer::bytes({std::byte{1}, std::byte{2}});
  if (!MismatchedBytes)
    return fail("Mismatched initializer construction must succeed before object verification");
  rcc::GlobalObjectDesc MismatchedObject;
  MismatchedObject.Symbol = "mismatched";
  MismatchedObject.ObjectType = rcc::GenericConstantType::byte();
  MismatchedObject.Size = 1;
  MismatchedObject.Initializer.emplace(std::move(*MismatchedBytes));
  if (Unit.addGlobalObject(std::move(MismatchedObject)) || Unit.globalObjectCount() != 1)
    return fail("CompilationUnit must reject mismatched global initializer sizes atomically");

  auto Verification = Unit.verify();
  if (!Verification)
    return fail("A valid CompilationUnit must pass verification");

  rcc::CompilationUnit Moved = std::move(Unit);
  if (Moved.functionCount() != 4 || Moved.functionAt(0) != *FirstAnonymous ||
      Moved.functionAt(1) != *SecondAnonymous || Moved.function("first") != *First ||
      Moved.function("second") != *Second || Moved.globalObject("global") != *Global)
    return fail("Moving a CompilationUnit must preserve owned object addresses and indices");
  Verification = Moved.verify();
  if (!Verification)
    return fail("A moved CompilationUnit must pass verification");
  return 0;
}
