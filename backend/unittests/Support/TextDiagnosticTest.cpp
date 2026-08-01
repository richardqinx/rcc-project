// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/TextDiagnostic.h"

#include <iostream>
#include <string>

namespace {

int fail(const char *Message) {
  std::cerr << Message << '\n';
  return 1;
}

} // namespace

int main() {
  rcc::TextSource Source("input.mir", "first\r\nsecond line\n");
  if (Source.lineCount() != 3)
    return fail("TextSource must index every physical line");
  if (Source.line(1) != "first" || Source.line(2) != "second line" || !Source.line(3).empty())
    return fail("TextSource must expose lines without line endings");

  const rcc::TextRange Range = Source.range(9, 15);
  if (Range.Begin.Line != 2 || Range.Begin.Column != 3 || Range.End.Line != 2 ||
      Range.End.Column != 9)
    return fail("TextSource must map byte offsets to one-based line and column values");

  const rcc::TextDiagnostic Diagnostic{
      .Severity = rcc::DiagnosticSeverity::Error,
      .Category = "yaml",
      .Message = "invalid scalar",
      .SourceName = "input.mir",
      .Range = Range,
  };
  const std::string Formatted = rcc::formatTextDiagnostic(Diagnostic, Source);
  if (Formatted != "input.mir:2:3: error[yaml]: invalid scalar\nsecond line\n  ^~~~~~")
    return fail("Text diagnostics must have deterministic source rendering");

  if (rcc::getDiagnosticSeverityName(rcc::DiagnosticSeverity::Warning) != "warning")
    return fail("Diagnostic severities must have stable names");
  return 0;
}
