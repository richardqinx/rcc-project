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

  return 0;
}
