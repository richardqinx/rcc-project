// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/TextDiagnostic.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <utility>

namespace rcc {

std::string_view getDiagnosticSeverityName(DiagnosticSeverity Severity) noexcept {
  switch (Severity) {
  case DiagnosticSeverity::Note:
    return "note";
  case DiagnosticSeverity::Warning:
    return "warning";
  case DiagnosticSeverity::Error:
    return "error";
  }

  return "unknown";
}

TextSource::TextSource(std::string Name, std::string Text)
    : Name(std::move(Name)), Text(std::move(Text)), LineOffsets{0} {
  for (std::size_t Index = 0; Index < this->Text.size(); ++Index) {
    if (this->Text[Index] == '\n')
      LineOffsets.push_back(Index + 1);
  }
}

TextPosition TextSource::positionAt(std::size_t Offset) const noexcept {
  Offset = std::min(Offset, Text.size());
  const auto LineIterator = std::upper_bound(LineOffsets.begin(), LineOffsets.end(), Offset);
  const std::size_t LineIndex =
      static_cast<std::size_t>(std::distance(LineOffsets.begin(), LineIterator) - 1);
  return TextPosition{
      .Offset = Offset,
      .Line = LineIndex + 1,
      .Column = Offset - LineOffsets[LineIndex] + 1,
  };
}

TextRange TextSource::range(std::size_t Begin, std::size_t End) const noexcept {
  Begin = std::min(Begin, Text.size());
  End = std::clamp(End, Begin, Text.size());
  return TextRange{.Begin = positionAt(Begin), .End = positionAt(End)};
}

std::string_view TextSource::line(std::size_t OneBasedLine) const noexcept {
  if (OneBasedLine == 0 || OneBasedLine > LineOffsets.size())
    return {};

  const std::size_t Begin = LineOffsets[OneBasedLine - 1];
  std::size_t End = OneBasedLine < LineOffsets.size() ? LineOffsets[OneBasedLine] - 1 : Text.size();
  if (End > Begin && Text[End - 1] == '\r')
    --End;
  return std::string_view(Text).substr(Begin, End - Begin);
}

std::string formatTextDiagnostic(const TextDiagnostic &Diagnostic, const TextSource &Source) {
  std::ostringstream Output;
  const TextPosition Position = Diagnostic.Range.Begin;
  const std::string_view SourceName =
      Diagnostic.SourceName.empty() ? Source.name() : std::string_view(Diagnostic.SourceName);

  Output << SourceName << ':' << Position.Line << ':' << Position.Column << ": "
         << getDiagnosticSeverityName(Diagnostic.Severity);
  if (!Diagnostic.Category.empty())
    Output << '[' << Diagnostic.Category << ']';
  Output << ": " << Diagnostic.Message;

  const std::string_view SourceLine = Source.line(Position.Line);
  if (SourceLine.empty())
    return Output.str();

  Output << '\n' << SourceLine << '\n';
  const std::size_t PrefixWidth = Position.Column > 0 ? Position.Column - 1 : 0;
  for (std::size_t Index = 0; Index < PrefixWidth && Index < SourceLine.size(); ++Index)
    Output << (SourceLine[Index] == '\t' ? '\t' : ' ');

  Output << '^';
  if (Diagnostic.Range.End.Line == Position.Line &&
      Diagnostic.Range.End.Column > Position.Column + 1) {
    Output << std::string(Diagnostic.Range.End.Column - Position.Column - 1, '~');
  }
  return Output.str();
}

} // namespace rcc
