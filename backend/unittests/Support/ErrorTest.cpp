// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/Error.h"

#include <iostream>
#include <string>

namespace {

int fail(const char *Message) {
  std::cerr << Message << '\n';
  return 1;
}

} // namespace

int main() {
  rcc::Expected<int> Value = 42;
  if (!Value || *Value != 42)
    return fail("Expected must preserve a successful value");

  rcc::Expected<int> Failure = rcc::makeUnexpected(rcc::ErrorCode::ParseError, "bad MIR");
  if (Failure)
    return fail("Expected must preserve an error");
  if (Failure.error().code() != rcc::ErrorCode::ParseError)
    return fail("Error must preserve its code");
  if (Failure.error().message() != "bad MIR")
    return fail("Error must preserve its message");
  if (rcc::getErrorCodeName(Failure.error().code()) != "parse error")
    return fail("Error code must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::FrameLayoutFailed) != "frame layout failed")
    return fail("frame-layout errors must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::SchedulingFailed) != "scheduling failed")
    return fail("scheduling errors must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::BranchRelaxationFailed) != "branch relaxation failed")
    return fail("branch-relaxation errors must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::PseudoExpansionFailed) != "pseudo expansion failed")
    return fail("pseudo-expansion errors must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::FinalEncodabilityFailed) != "final encodability failed")
    return fail("final-encodability errors must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::CFGOptimizationFailed) != "CFG optimization failed")
    return fail("CFG-optimization errors must have a stable name");
  if (rcc::getErrorCodeName(rcc::ErrorCode::AssemblyLoweringFailed) != "assembly lowering failed")
    return fail("assembly-lowering errors must have a stable name");

  return 0;
}
