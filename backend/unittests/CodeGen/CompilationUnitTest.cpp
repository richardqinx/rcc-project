// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/CompilationUnit.h"

#include "rcc/CodeGen/MachineFunction.h"

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
  auto Empty = Unit.createMachineFunction("");
  if (Empty || Unit.functionCount() != 0)
    return fail("CompilationUnit must reject an empty function name atomically");

  auto First = Unit.createMachineFunction("first");
  if (!First || (*First)->stableID().value() != 1)
    return fail("CompilationUnit must allocate the first stable function identity");

  auto Duplicate = Unit.createMachineFunction("first");
  if (Duplicate || Unit.functionCount() != 1)
    return fail("CompilationUnit must reject duplicate function names atomically");

  auto Second = Unit.createMachineFunction("second");
  if (!Second || (*Second)->stableID().value() != 2)
    return fail("Rejected function creation must not consume a StableID");
  if (Unit.functionCount() != 2 || Unit.functionAt(0) != *First || Unit.functionAt(1) != *Second)
    return fail("CompilationUnit must preserve function document order");
  if (Unit.function("first") != *First || Unit.function("missing") != nullptr)
    return fail("CompilationUnit must provide exact name lookup");
  if (Unit.function((*Second)->stableID()) != *Second ||
      Unit.function(rcc::MachineFunctionStableID::fromValue(99)) != nullptr)
    return fail("CompilationUnit must provide exact StableID lookup");
  if (Unit.functionAt(2) != nullptr)
    return fail("CompilationUnit must reject an out-of-range ordered lookup");

  auto Verification = Unit.verify();
  if (!Verification)
    return fail("A valid CompilationUnit must pass verification");

  rcc::CompilationUnit Moved = std::move(Unit);
  if (Moved.functionCount() != 2 || Moved.function("first") != *First ||
      Moved.function("second") != *Second)
    return fail("Moving a CompilationUnit must preserve owned function addresses and indices");
  Verification = Moved.verify();
  if (!Verification)
    return fail("A moved CompilationUnit must pass verification");
  return 0;
}
