// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_SUPPORT_TEXTDIAGNOSTIC_H
#define RCC_SUPPORT_TEXTDIAGNOSTIC_H

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rcc {

struct TextPosition final {
  std::size_t Offset = 0;
  std::size_t Line = 1;
  std::size_t Column = 1;

  friend constexpr bool operator==(TextPosition Left, TextPosition Right) noexcept = default;
};

struct TextRange final {
  TextPosition Begin;
  TextPosition End;

  [[nodiscard]] constexpr bool isOrdered() const noexcept {
    return Begin.Offset <= End.Offset;
  }

  friend constexpr bool operator==(TextRange Left, TextRange Right) noexcept = default;
};

enum class DiagnosticSeverity : unsigned char {
  Note,
  Warning,
  Error,
};

[[nodiscard]] std::string_view getDiagnosticSeverityName(DiagnosticSeverity Severity) noexcept;

struct TextDiagnostic final {
  DiagnosticSeverity Severity = DiagnosticSeverity::Error;
  std::string Category;
  std::string Message;
  std::string SourceName;
  TextRange Range;
};

class TextSource final {
public:
  TextSource(std::string Name, std::string Text);

  [[nodiscard]] std::string_view name() const noexcept {
    return Name;
  }

  [[nodiscard]] std::string_view text() const noexcept {
    return Text;
  }

  [[nodiscard]] std::size_t lineCount() const noexcept {
    return LineOffsets.size();
  }

  [[nodiscard]] TextPosition positionAt(std::size_t Offset) const noexcept;
  [[nodiscard]] TextRange range(std::size_t Begin, std::size_t End) const noexcept;
  [[nodiscard]] std::string_view line(std::size_t OneBasedLine) const noexcept;
  [[nodiscard]] std::span<const std::size_t> lineOffsets() const noexcept {
    return LineOffsets;
  }

private:
  std::string Name;
  std::string Text;
  std::vector<std::size_t> LineOffsets;
};

[[nodiscard]] std::string formatTextDiagnostic(const TextDiagnostic &Diagnostic,
                                               const TextSource &Source);

} // namespace rcc

#endif // RCC_SUPPORT_TEXTDIAGNOSTIC_H
