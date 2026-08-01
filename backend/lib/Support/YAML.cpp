// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/YAML.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rcc::yaml {
namespace {

struct PhysicalLine final {
  std::size_t Begin = 0;
  std::size_t ContentEnd = 0;
  std::size_t End = 0;
  std::size_t Indent = 0;
  bool HasIndentTab = false;
  bool IsBlank = true;
};

enum class DocumentMarker : std::uint8_t {
  None,
  Start,
  End,
};

[[nodiscard]] bool isAsciiSpace(char Character) noexcept {
  return Character == ' ' || Character == '\t';
}

[[nodiscard]] bool isHexDigit(char Character) noexcept {
  return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f') ||
         (Character >= 'A' && Character <= 'F');
}

[[nodiscard]] unsigned hexValue(char Character) noexcept {
  if (Character >= '0' && Character <= '9')
    return static_cast<unsigned>(Character - '0');
  if (Character >= 'a' && Character <= 'f')
    return static_cast<unsigned>(Character - 'a') + 10U;
  return static_cast<unsigned>(Character - 'A') + 10U;
}

[[nodiscard]] bool isNullSpelling(std::string_view Value) noexcept {
  return Value == "~" || Value == "null" || Value == "Null" || Value == "NULL";
}

void appendUTF8(std::string &Output, std::uint32_t CodePoint) {
  if (CodePoint <= 0x7FU) {
    Output.push_back(static_cast<char>(CodePoint));
    return;
  }
  if (CodePoint <= 0x7FFU) {
    Output.push_back(static_cast<char>(0xC0U | (CodePoint >> 6U)));
    Output.push_back(static_cast<char>(0x80U | (CodePoint & 0x3FU)));
    return;
  }
  if (CodePoint <= 0xFFFFU) {
    Output.push_back(static_cast<char>(0xE0U | (CodePoint >> 12U)));
    Output.push_back(static_cast<char>(0x80U | ((CodePoint >> 6U) & 0x3FU)));
    Output.push_back(static_cast<char>(0x80U | (CodePoint & 0x3FU)));
    return;
  }
  Output.push_back(static_cast<char>(0xF0U | (CodePoint >> 18U)));
  Output.push_back(static_cast<char>(0x80U | ((CodePoint >> 12U) & 0x3FU)));
  Output.push_back(static_cast<char>(0x80U | ((CodePoint >> 6U) & 0x3FU)));
  Output.push_back(static_cast<char>(0x80U | (CodePoint & 0x3FU)));
}

} // namespace

std::optional<NodeID> Node::mappingValue(std::string_view Key) const noexcept {
  if (Kind != NodeKind::Mapping)
    return std::nullopt;
  const auto Iterator = std::find_if(Mapping.begin(), Mapping.end(),
                                     [Key](const MappingEntry &Entry) { return Entry.Key == Key; });
  if (Iterator == Mapping.end())
    return std::nullopt;
  return Iterator->Value;
}

const Node *Stream::node(NodeID ID) const noexcept {
  if (!ID.isValid() || ID.rawValue() >= Nodes.size())
    return nullptr;
  return &Nodes[ID.rawValue()];
}

class Parser final {
public:
  Parser(std::string SourceName, std::string SourceText, ParseOptions Options)
      : Parsed(std::move(SourceName), std::move(SourceText)), Options(Options) {
    buildLineTable();
  }

  [[nodiscard]] ParseResult run() {
    if (Options.MaximumDepth == 0) {
      diagnose(0, 0, "maximum YAML nesting depth must be greater than zero");
      return failureResult();
    }
    if (Options.MaximumNodes == 0 ||
        Options.MaximumNodes >= std::numeric_limits<std::uint32_t>::max()) {
      diagnose(0, 0, "maximum YAML node count is outside the supported range");
      return failureResult();
    }
    if (Parsed.Source.text().find('\0') != std::string_view::npos) {
      const std::size_t Offset = Parsed.Source.text().find('\0');
      diagnose(Offset, Offset + 1, "NUL bytes are not allowed in YAML input");
      return failureResult();
    }

    std::size_t LineIndex = 0;
    skipBlankLines(LineIndex, Lines.size());
    while (LineIndex < Lines.size()) {
      if (Lines[LineIndex].Indent == 0 && Parsed.Source.text()[Lines[LineIndex].Begin] == '%') {
        diagnose(Lines[LineIndex].Begin, Lines[LineIndex].ContentEnd,
                 "YAML directives are not supported");
        return failureResult();
      }
      if (markerAt(LineIndex) == DocumentMarker::End) {
        diagnose(Lines[LineIndex].Begin, Lines[LineIndex].ContentEnd,
                 "document end marker has no open document");
        return failureResult();
      }

      const std::size_t DocumentStartLine = LineIndex;
      const bool HasExplicitStart = markerAt(LineIndex) == DocumentMarker::Start;
      std::size_t InlineStart = Lines[LineIndex].ContentEnd;
      if (HasExplicitStart)
        InlineStart = markerRemainder(LineIndex, 3);

      const std::size_t SearchStart = HasExplicitStart ? LineIndex + 1 : LineIndex;
      const std::size_t LimitLine = findDocumentBoundary(SearchStart);
      NodeID Root;
      if (HasExplicitStart && InlineStart < Lines[LineIndex].ContentEnd &&
          Parsed.Source.text()[InlineStart] != '#') {
        if (Parsed.Source.text()[InlineStart] == '|' || Parsed.Source.text()[InlineStart] == '>') {
          Root = parseBlockScalar(LineIndex, InlineStart, 0, 1, LimitLine);
        } else {
          Root = parseInlineValueOnLine(LineIndex, InlineStart, 1, LimitLine);
        }
      } else {
        if (HasExplicitStart)
          ++LineIndex;
        skipBlankLines(LineIndex, LimitLine);
        if (LineIndex == LimitLine) {
          const std::size_t Offset = HasExplicitStart ? Lines[DocumentStartLine].ContentEnd
                                                      : Lines[DocumentStartLine].Begin;
          Root = addNullNode(Offset, Offset);
        } else {
          Root = parseBlockNode(LineIndex, Lines[LineIndex].Indent, 1, LimitLine);
        }
      }

      if (Failed)
        return failureResult();

      skipBlankLines(LineIndex, LimitLine);
      if (LineIndex != LimitLine) {
        diagnose(Lines[LineIndex].Begin + Lines[LineIndex].Indent, Lines[LineIndex].ContentEnd,
                 "unexpected content after YAML document root");
        return failureResult();
      }

      bool HasExplicitEnd = false;
      if (LimitLine < Lines.size() && markerAt(LimitLine) == DocumentMarker::End) {
        const std::size_t EndRemainder = markerRemainder(LimitLine, 3);
        if (EndRemainder < Lines[LimitLine].ContentEnd &&
            Parsed.Source.text()[EndRemainder] != '#') {
          diagnose(EndRemainder, Lines[LimitLine].ContentEnd,
                   "document end marker may only be followed by a comment");
          return failureResult();
        }
        HasExplicitEnd = true;
        LineIndex = LimitLine + 1;
      } else {
        LineIndex = LimitLine;
      }

      const std::size_t DocumentBegin = Lines[DocumentStartLine].Begin;
      const std::size_t DocumentEnd = HasExplicitEnd
                                          ? Lines[LimitLine].ContentEnd
                                          : (LimitLine == 0 ? 0 : Lines[LimitLine - 1].End);
      Parsed.Documents.push_back(Document{
          .Root = Root,
          .Range = Parsed.Source.range(DocumentBegin, DocumentEnd),
          .HasExplicitStart = HasExplicitStart,
          .HasExplicitEnd = HasExplicitEnd,
      });

      skipBlankLines(LineIndex, Lines.size());
    }

    ParseResult Result;
    Result.ParsedStream.emplace(std::move(Parsed));
    return Result;
  }

private:
  void buildLineTable() {
    const std::string_view Text = Parsed.Source.text();
    std::size_t Begin = 0;
    while (Begin <= Text.size()) {
      const std::size_t Newline = Text.find('\n', Begin);
      const std::size_t End = Newline == std::string_view::npos ? Text.size() : Newline + 1;
      std::size_t ContentEnd = Newline == std::string_view::npos ? Text.size() : Newline;
      if (ContentEnd > Begin && Text[ContentEnd - 1] == '\r')
        --ContentEnd;

      PhysicalLine Line{
          .Begin = Begin,
          .ContentEnd = ContentEnd,
          .End = End,
      };
      std::size_t Cursor = Begin;
      while (Cursor < ContentEnd && Text[Cursor] == ' ')
        ++Cursor;
      Line.Indent = Cursor - Begin;
      std::size_t ContentCursor = Cursor;
      while (ContentCursor < ContentEnd && isAsciiSpace(Text[ContentCursor])) {
        if (Text[ContentCursor] == '\t')
          Line.HasIndentTab = true;
        ++ContentCursor;
      }
      Line.IsBlank = ContentCursor == ContentEnd || Text[ContentCursor] == '#';
      Lines.push_back(Line);

      if (Newline == std::string_view::npos)
        break;
      Begin = End;
    }
  }

  [[nodiscard]] ParseResult failureResult() {
    ParseResult Result;
    Result.Diagnostics = std::move(Diagnostics);
    return Result;
  }

  void diagnose(std::size_t Begin, std::size_t End, std::string Message) {
    if (Failed)
      return;
    Failed = true;
    Diagnostics.push_back(TextDiagnostic{
        .Severity = DiagnosticSeverity::Error,
        .Category = "yaml",
        .Message = std::move(Message),
        .SourceName = std::string(Parsed.Source.name()),
        .Range = Parsed.Source.range(Begin, End),
    });
  }

  void skipBlankLines(std::size_t &LineIndex, std::size_t LimitLine) {
    while (LineIndex < LimitLine && Lines[LineIndex].IsBlank)
      ++LineIndex;
  }

  [[nodiscard]] DocumentMarker markerAt(std::size_t LineIndex) const noexcept {
    if (LineIndex >= Lines.size())
      return DocumentMarker::None;
    const PhysicalLine &Line = Lines[LineIndex];
    if (Line.Indent != 0 || Line.ContentEnd - Line.Begin < 3)
      return DocumentMarker::None;

    const std::string_view Text = Parsed.Source.text();
    const std::string_view Prefix = Text.substr(Line.Begin, 3);
    if (Prefix != "---" && Prefix != "...")
      return DocumentMarker::None;
    if (Line.Begin + 3 < Line.ContentEnd) {
      const char Next = Text[Line.Begin + 3];
      if (!isAsciiSpace(Next) && Next != '#')
        return DocumentMarker::None;
    }
    return Prefix == "---" ? DocumentMarker::Start : DocumentMarker::End;
  }

  [[nodiscard]] std::size_t markerRemainder(std::size_t LineIndex,
                                            std::size_t MarkerWidth) const noexcept {
    const PhysicalLine &Line = Lines[LineIndex];
    std::size_t Cursor = Line.Begin + MarkerWidth;
    const std::string_view Text = Parsed.Source.text();
    while (Cursor < Line.ContentEnd && isAsciiSpace(Text[Cursor]))
      ++Cursor;
    return Cursor;
  }

  [[nodiscard]] std::size_t findDocumentBoundary(std::size_t StartLine) const noexcept {
    for (std::size_t LineIndex = StartLine; LineIndex < Lines.size(); ++LineIndex) {
      if (markerAt(LineIndex) != DocumentMarker::None)
        return LineIndex;
    }
    return Lines.size();
  }

  [[nodiscard]] bool validateIndent(std::size_t LineIndex) {
    if (!Lines[LineIndex].HasIndentTab)
      return true;
    diagnose(Lines[LineIndex].Begin, Lines[LineIndex].Begin + Lines[LineIndex].Indent,
             "tab characters are not allowed in YAML indentation");
    return false;
  }

  [[nodiscard]] NodeID parseBlockNode(std::size_t &LineIndex, std::size_t Indent, std::size_t Depth,
                                      std::size_t LimitLine) {
    if (!checkDepth(Depth))
      return {};
    skipBlankLines(LineIndex, LimitLine);
    if (LineIndex >= LimitLine) {
      const std::size_t Offset =
          LimitLine < Lines.size() ? Lines[LimitLine].Begin : Parsed.Source.text().size();
      return addNullNode(Offset, Offset);
    }
    if (!validateIndent(LineIndex))
      return {};
    if (Lines[LineIndex].Indent != Indent) {
      diagnose(Lines[LineIndex].Begin + Lines[LineIndex].Indent, Lines[LineIndex].ContentEnd,
               "inconsistent YAML indentation");
      return {};
    }

    const std::size_t ContentStart = Lines[LineIndex].Begin + Indent;
    if (isSequenceIndicator(ContentStart, Lines[LineIndex].ContentEnd))
      return parseBlockSequence(LineIndex, Indent, Depth, LimitLine);
    if (findBlockMappingColon(ContentStart, Lines[LineIndex].ContentEnd))
      return parseBlockMapping(LineIndex, Indent, Depth, LimitLine);

    NodeID Result;
    if (Parsed.Source.text()[ContentStart] == '|' || Parsed.Source.text()[ContentStart] == '>') {
      Result = parseBlockScalar(LineIndex, ContentStart, Indent, Depth, LimitLine);
    } else {
      Result = parseInlineValueOnLine(LineIndex, ContentStart, Depth, LimitLine);
    }
    return Result;
  }

  [[nodiscard]] NodeID parseBlockMapping(std::size_t &LineIndex, std::size_t Indent,
                                         std::size_t Depth, std::size_t LimitLine) {
    const std::size_t Begin = Lines[LineIndex].Begin + Indent;
    std::vector<MappingEntry> Entries;
    while (LineIndex < LimitLine) {
      skipBlankLines(LineIndex, LimitLine);
      if (LineIndex >= LimitLine || Lines[LineIndex].Indent < Indent)
        break;
      if (!validateIndent(LineIndex))
        return {};
      if (Lines[LineIndex].Indent > Indent) {
        diagnose(Lines[LineIndex].Begin + Lines[LineIndex].Indent, Lines[LineIndex].ContentEnd,
                 "unexpected indentation in YAML mapping");
        return {};
      }

      const std::size_t KeyStart = Lines[LineIndex].Begin + Indent;
      if (!findBlockMappingColon(KeyStart, Lines[LineIndex].ContentEnd))
        break;
      if (!parseMappingEntry(LineIndex, KeyStart, Indent, Depth, LimitLine, Entries))
        return {};
    }

    const std::size_t End =
        LineIndex == 0 ? Begin : std::max(Begin, Lines[LineIndex - 1].ContentEnd);
    return addMappingNode(Begin, End, CollectionStyle::Block, std::move(Entries));
  }

  [[nodiscard]] NodeID parseInlineStartedMapping(std::size_t &LineIndex, std::size_t KeyStart,
                                                 std::size_t MappingIndent, std::size_t Depth,
                                                 std::size_t LimitLine) {
    const std::size_t Begin = KeyStart;
    std::vector<MappingEntry> Entries;
    if (!parseMappingEntry(LineIndex, KeyStart, MappingIndent, Depth, LimitLine, Entries))
      return {};

    while (LineIndex < LimitLine) {
      skipBlankLines(LineIndex, LimitLine);
      if (LineIndex >= LimitLine || Lines[LineIndex].Indent < MappingIndent)
        break;
      if (!validateIndent(LineIndex))
        return {};
      if (Lines[LineIndex].Indent > MappingIndent) {
        diagnose(Lines[LineIndex].Begin + Lines[LineIndex].Indent, Lines[LineIndex].ContentEnd,
                 "unexpected indentation in YAML mapping");
        return {};
      }
      const std::size_t ContinuationKey = Lines[LineIndex].Begin + MappingIndent;
      if (!findBlockMappingColon(ContinuationKey, Lines[LineIndex].ContentEnd))
        break;
      if (!parseMappingEntry(LineIndex, ContinuationKey, MappingIndent, Depth, LimitLine, Entries))
        return {};
    }

    const std::size_t End =
        LineIndex == 0 ? Begin : std::max(Begin, Lines[LineIndex - 1].ContentEnd);
    return addMappingNode(Begin, End, CollectionStyle::Block, std::move(Entries));
  }

  [[nodiscard]] bool parseMappingEntry(std::size_t &LineIndex, std::size_t KeyStart,
                                       std::size_t ParentIndent, std::size_t Depth,
                                       std::size_t LimitLine, std::vector<MappingEntry> &Entries) {
    const PhysicalLine &Line = Lines[LineIndex];
    const std::optional<std::size_t> Colon = findBlockMappingColon(KeyStart, Line.ContentEnd);
    if (!Colon) {
      diagnose(KeyStart, Line.ContentEnd, "expected ':' after YAML mapping key");
      return false;
    }

    std::string Key;
    TextRange KeyRange;
    if (!parseMappingKey(KeyStart, *Colon, Key, KeyRange))
      return false;
    if (std::any_of(Entries.begin(), Entries.end(),
                    [&Key](const MappingEntry &Entry) { return Entry.Key == Key; })) {
      diagnose(KeyStart, *Colon, "duplicate YAML mapping key '" + Key + "'");
      return false;
    }

    std::size_t ValueStart = *Colon + 1;
    const std::string_view Text = Parsed.Source.text();
    while (ValueStart < Line.ContentEnd && isAsciiSpace(Text[ValueStart]))
      ++ValueStart;

    NodeID Value;
    if (ValueStart == Line.ContentEnd || Text[ValueStart] == '#') {
      const std::size_t CurrentLine = LineIndex;
      std::size_t NestedLine = LineIndex + 1;
      skipBlankLines(NestedLine, LimitLine);
      if (NestedLine < LimitLine && Lines[NestedLine].Indent > ParentIndent) {
        LineIndex = NestedLine;
        Value = parseBlockNode(LineIndex, Lines[NestedLine].Indent, Depth + 1, LimitLine);
      } else {
        Value = addNullNode(*Colon + 1, *Colon + 1);
        LineIndex = CurrentLine + 1;
      }
    } else if (Text[ValueStart] == '|' || Text[ValueStart] == '>') {
      Value = parseBlockScalar(LineIndex, ValueStart, ParentIndent, Depth + 1, LimitLine);
    } else {
      Value = parseInlineValueOnLine(LineIndex, ValueStart, Depth + 1, LimitLine);
    }
    if (Failed)
      return false;

    Entries.push_back(MappingEntry{
        .Key = std::move(Key),
        .KeyRange = KeyRange,
        .Value = Value,
    });
    return true;
  }

  [[nodiscard]] NodeID parseBlockSequence(std::size_t &LineIndex, std::size_t Indent,
                                          std::size_t Depth, std::size_t LimitLine) {
    const std::size_t Begin = Lines[LineIndex].Begin + Indent;
    std::vector<NodeID> Items;
    const std::string_view Text = Parsed.Source.text();

    while (LineIndex < LimitLine) {
      skipBlankLines(LineIndex, LimitLine);
      if (LineIndex >= LimitLine || Lines[LineIndex].Indent < Indent)
        break;
      if (!validateIndent(LineIndex))
        return {};
      if (Lines[LineIndex].Indent > Indent) {
        diagnose(Lines[LineIndex].Begin + Lines[LineIndex].Indent, Lines[LineIndex].ContentEnd,
                 "unexpected indentation in YAML sequence");
        return {};
      }

      const std::size_t Dash = Lines[LineIndex].Begin + Indent;
      if (!isSequenceIndicator(Dash, Lines[LineIndex].ContentEnd))
        break;

      std::size_t ItemStart = Dash + 1;
      while (ItemStart < Lines[LineIndex].ContentEnd && isAsciiSpace(Text[ItemStart]))
        ++ItemStart;

      NodeID Item;
      if (ItemStart == Lines[LineIndex].ContentEnd || Text[ItemStart] == '#') {
        const std::size_t CurrentLine = LineIndex;
        std::size_t NestedLine = LineIndex + 1;
        skipBlankLines(NestedLine, LimitLine);
        if (NestedLine < LimitLine && Lines[NestedLine].Indent > Indent) {
          LineIndex = NestedLine;
          Item = parseBlockNode(LineIndex, Lines[NestedLine].Indent, Depth + 1, LimitLine);
        } else {
          Item = addNullNode(Dash + 1, Dash + 1);
          LineIndex = CurrentLine + 1;
        }
      } else if (Text[ItemStart] == '|' || Text[ItemStart] == '>') {
        Item = parseBlockScalar(LineIndex, ItemStart, Indent, Depth + 1, LimitLine);
      } else if (findBlockMappingColon(ItemStart, Lines[LineIndex].ContentEnd)) {
        const std::size_t MappingIndent = ItemStart - Lines[LineIndex].Begin;
        Item = parseInlineStartedMapping(LineIndex, ItemStart, MappingIndent, Depth + 1, LimitLine);
      } else {
        Item = parseInlineValueOnLine(LineIndex, ItemStart, Depth + 1, LimitLine);
      }
      if (Failed)
        return {};
      Items.push_back(Item);
    }

    const std::size_t End =
        LineIndex == 0 ? Begin : std::max(Begin, Lines[LineIndex - 1].ContentEnd);
    return addSequenceNode(Begin, End, CollectionStyle::Block, std::move(Items));
  }

  [[nodiscard]] NodeID parseInlineValueOnLine(std::size_t &LineIndex, std::size_t Begin,
                                              std::size_t Depth, std::size_t LimitLine) {
    const std::string_view Text = Parsed.Source.text();
    const std::size_t InitialLine = LineIndex;
    const std::size_t LineEnd = Lines[InitialLine].ContentEnd;
    std::size_t ValueStart = Begin;
    while (ValueStart < LineEnd && isAsciiSpace(Text[ValueStart]))
      ++ValueStart;

    const bool CanSpanLines =
        ValueStart < LineEnd && (Text[ValueStart] == '[' || Text[ValueStart] == '{' ||
                                 Text[ValueStart] == '\'' || Text[ValueStart] == '"');
    if (!CanSpanLines) {
      NodeID Result = parseInlineValue(ValueStart, LineEnd, Depth);
      ++LineIndex;
      return Result;
    }

    const std::size_t DocumentEnd =
        LimitLine < Lines.size() ? Lines[LimitLine].Begin : Parsed.Source.text().size();
    std::size_t Cursor = ValueStart;
    NodeID Result = parseFlowNode(Cursor, DocumentEnd, Depth, false);
    if (Failed)
      return {};

    const std::size_t LastValueOffset = Cursor == 0 ? 0 : Cursor - 1;
    const std::size_t LastLine = Parsed.Source.positionAt(LastValueOffset).Line - 1;
    if (LastLine >= Lines.size()) {
      diagnose(Cursor, Cursor, "internal YAML line tracking failure");
      return {};
    }

    std::size_t Tail = Cursor;
    while (Tail < Lines[LastLine].ContentEnd && isAsciiSpace(Text[Tail]))
      ++Tail;
    if (Tail < Lines[LastLine].ContentEnd && Text[Tail] != '#') {
      diagnose(Tail, Lines[LastLine].ContentEnd, "unexpected characters after YAML value");
      return {};
    }
    LineIndex = LastLine + 1;
    return Result;
  }

  [[nodiscard]] NodeID parseInlineValue(std::size_t Begin, std::size_t End, std::size_t Depth) {
    if (!checkDepth(Depth))
      return {};
    const std::string_view Text = Parsed.Source.text();
    while (Begin < End && isAsciiSpace(Text[Begin]))
      ++Begin;
    if (Begin == End || Text[Begin] == '#')
      return addNullNode(Begin, Begin);

    std::size_t Cursor = Begin;
    NodeID Result = parseFlowNode(Cursor, End, Depth, false);
    if (Failed)
      return {};

    while (Cursor < End && isAsciiSpace(Text[Cursor]))
      ++Cursor;
    if (Cursor < End && Text[Cursor] != '#') {
      diagnose(Cursor, End, "unexpected characters after YAML value");
      return {};
    }
    return Result;
  }

  [[nodiscard]] NodeID parseFlowNode(std::size_t &Cursor, std::size_t End, std::size_t Depth,
                                     bool IsFlowValue) {
    if (!checkDepth(Depth))
      return {};
    const std::string_view Text = Parsed.Source.text();
    while (Cursor < End && isAsciiSpace(Text[Cursor]))
      ++Cursor;
    if (Cursor >= End) {
      diagnose(End, End, "expected YAML value");
      return {};
    }

    if (Text[Cursor] == '[')
      return parseFlowSequence(Cursor, End, Depth);
    if (Text[Cursor] == '{')
      return parseFlowMapping(Cursor, End, Depth);
    if (Text[Cursor] == '\'' || Text[Cursor] == '"')
      return parseQuotedScalar(Cursor, End);
    if (Text[Cursor] == '&' || Text[Cursor] == '*' || Text[Cursor] == '!' || Text[Cursor] == '%') {
      diagnose(Cursor, Cursor + 1, "YAML anchors, aliases, tags, and directives are not supported");
      return {};
    }

    const std::size_t Begin = Cursor;
    while (Cursor < End) {
      const char Character = Text[Cursor];
      if (IsFlowValue && (Character == ',' || Character == ']' || Character == '}'))
        break;
      if (Character == '#' && (Cursor == Begin || isAsciiSpace(Text[Cursor - 1])))
        break;
      ++Cursor;
    }
    std::size_t ScalarEnd = Cursor;
    while (ScalarEnd > Begin && isAsciiSpace(Text[ScalarEnd - 1]))
      --ScalarEnd;
    if (ScalarEnd == Begin) {
      diagnose(Begin, std::min(Begin + 1, End), "expected YAML scalar");
      return {};
    }
    const std::string Value(Text.substr(Begin, ScalarEnd - Begin));
    return isNullSpelling(Value) ? addNullNode(Begin, ScalarEnd)
                                 : addScalarNode(Begin, ScalarEnd, ScalarStyle::Plain, Value);
  }

  [[nodiscard]] NodeID parseFlowSequence(std::size_t &Cursor, std::size_t End, std::size_t Depth) {
    const std::size_t Begin = Cursor++;
    std::vector<NodeID> Items;
    const std::string_view Text = Parsed.Source.text();
    skipFlowSpace(Cursor, End);
    if (Cursor < End && Text[Cursor] == ']') {
      ++Cursor;
      return addSequenceNode(Begin, Cursor, CollectionStyle::Flow, std::move(Items));
    }

    while (Cursor < End) {
      Items.push_back(parseFlowNode(Cursor, End, Depth + 1, true));
      if (Failed)
        return {};
      skipFlowSpace(Cursor, End);
      if (Cursor >= End) {
        diagnose(Begin, End, "unterminated YAML flow sequence");
        return {};
      }
      if (Text[Cursor] == ']') {
        ++Cursor;
        return addSequenceNode(Begin, Cursor, CollectionStyle::Flow, std::move(Items));
      }
      if (Text[Cursor] != ',') {
        diagnose(Cursor, Cursor + 1, "expected ',' or ']' in YAML flow sequence");
        return {};
      }
      ++Cursor;
      skipFlowSpace(Cursor, End);
      if (Cursor < End && Text[Cursor] == ']') {
        ++Cursor;
        return addSequenceNode(Begin, Cursor, CollectionStyle::Flow, std::move(Items));
      }
    }

    diagnose(Begin, End, "unterminated YAML flow sequence");
    return {};
  }

  [[nodiscard]] NodeID parseFlowMapping(std::size_t &Cursor, std::size_t End, std::size_t Depth) {
    const std::size_t Begin = Cursor++;
    std::vector<MappingEntry> Entries;
    const std::string_view Text = Parsed.Source.text();
    skipFlowSpace(Cursor, End);
    if (Cursor < End && Text[Cursor] == '}') {
      ++Cursor;
      return addMappingNode(Begin, Cursor, CollectionStyle::Flow, std::move(Entries));
    }

    while (Cursor < End) {
      std::string Key;
      TextRange KeyRange;
      if (!parseFlowKey(Cursor, End, Key, KeyRange))
        return {};
      if (std::any_of(Entries.begin(), Entries.end(),
                      [&Key](const MappingEntry &Entry) { return Entry.Key == Key; })) {
        diagnose(KeyRange.Begin.Offset, KeyRange.End.Offset,
                 "duplicate YAML mapping key '" + Key + "'");
        return {};
      }
      skipFlowSpace(Cursor, End);
      if (Cursor >= End || Text[Cursor] != ':') {
        diagnose(Cursor, std::min(Cursor + 1, End), "expected ':' after YAML flow mapping key");
        return {};
      }
      ++Cursor;
      skipFlowSpace(Cursor, End);
      NodeID Value;
      if (isFlowBlockSequenceStart(Cursor, End))
        Value = parseFlowBlockSequence(Cursor, End, Depth + 1);
      else if (Cursor < End && (Text[Cursor] == ',' || Text[Cursor] == '}'))
        Value = addNullNode(Cursor, Cursor);
      else
        Value = parseFlowNode(Cursor, End, Depth + 1, true);
      if (Failed)
        return {};
      Entries.push_back(MappingEntry{
          .Key = std::move(Key),
          .KeyRange = KeyRange,
          .Value = Value,
      });

      skipFlowSpace(Cursor, End);
      if (Cursor >= End) {
        diagnose(Begin, End, "unterminated YAML flow mapping");
        return {};
      }
      if (Text[Cursor] == '}') {
        ++Cursor;
        return addMappingNode(Begin, Cursor, CollectionStyle::Flow, std::move(Entries));
      }
      if (Text[Cursor] != ',') {
        diagnose(Cursor, Cursor + 1, "expected ',' or '}' in YAML flow mapping");
        return {};
      }
      ++Cursor;
      skipFlowSpace(Cursor, End);
      if (Cursor < End && Text[Cursor] == '}') {
        ++Cursor;
        return addMappingNode(Begin, Cursor, CollectionStyle::Flow, std::move(Entries));
      }
    }

    diagnose(Begin, End, "unterminated YAML flow mapping");
    return {};
  }

  [[nodiscard]] NodeID parseFlowBlockSequence(std::size_t &Cursor, std::size_t End,
                                              std::size_t Depth) {
    if (!checkDepth(Depth))
      return {};
    const std::size_t Begin = Cursor;
    const std::size_t FirstLine = Parsed.Source.positionAt(Cursor).Line - 1;
    const std::size_t Indent = Lines[FirstLine].Indent;
    const std::string_view Text = Parsed.Source.text();
    std::vector<NodeID> Items;
    std::size_t LastItemEnd = Begin;

    while (isFlowBlockSequenceStart(Cursor, End)) {
      const std::size_t ItemLine = Parsed.Source.positionAt(Cursor).Line - 1;
      if (Lines[ItemLine].Indent != Indent)
        break;

      const std::size_t Dash = Cursor++;
      while (Cursor < Lines[ItemLine].ContentEnd && isAsciiSpace(Text[Cursor]))
        ++Cursor;
      if (Cursor == Lines[ItemLine].ContentEnd || Text[Cursor] == '#') {
        diagnose(Dash, Lines[ItemLine].ContentEnd,
                 "empty block sequence entry inside a flow mapping is not supported");
        return {};
      }

      const bool ItemCanSpan =
          Text[Cursor] == '[' || Text[Cursor] == '{' || Text[Cursor] == '\'' || Text[Cursor] == '"';
      const std::size_t ItemEnd = ItemCanSpan ? End : Lines[ItemLine].ContentEnd;
      Items.push_back(parseFlowNode(Cursor, ItemEnd, Depth + 1, true));
      if (Failed)
        return {};
      LastItemEnd = Cursor;
      skipFlowSpace(Cursor, End);
    }

    return addSequenceNode(Begin, LastItemEnd, CollectionStyle::Block, std::move(Items));
  }

  [[nodiscard]] bool parseFlowKey(std::size_t &Cursor, std::size_t End, std::string &Key,
                                  TextRange &Range) {
    const std::string_view Text = Parsed.Source.text();
    skipFlowSpace(Cursor, End);
    if (Cursor >= End) {
      diagnose(End, End, "expected YAML flow mapping key");
      return false;
    }

    const std::size_t Begin = Cursor;
    if (Text[Cursor] == '\'' || Text[Cursor] == '"') {
      ScalarStyle Style = ScalarStyle::Plain;
      return parseQuotedText(Cursor, End, Key, Style, Range);
    }

    while (Cursor < End && Text[Cursor] != ':' && Text[Cursor] != ',' && Text[Cursor] != '}')
      ++Cursor;
    std::size_t KeyEnd = Cursor;
    while (KeyEnd > Begin && isAsciiSpace(Text[KeyEnd - 1]))
      --KeyEnd;
    if (KeyEnd == Begin) {
      diagnose(Begin, std::min(Begin + 1, End), "empty YAML mapping keys are not supported");
      return false;
    }
    Key = std::string(Text.substr(Begin, KeyEnd - Begin));
    Range = Parsed.Source.range(Begin, KeyEnd);
    return true;
  }

  [[nodiscard]] NodeID parseQuotedScalar(std::size_t &Cursor, std::size_t End) {
    std::string Value;
    ScalarStyle Style = ScalarStyle::Plain;
    TextRange Range;
    if (!parseQuotedText(Cursor, End, Value, Style, Range))
      return {};
    return addScalarNode(Range.Begin.Offset, Range.End.Offset, Style, std::move(Value));
  }

  [[nodiscard]] bool parseQuotedText(std::size_t &Cursor, std::size_t End, std::string &Value,
                                     ScalarStyle &Style, TextRange &Range) {
    const std::string_view Text = Parsed.Source.text();
    const std::size_t Begin = Cursor;
    const char Quote = Text[Cursor++];

    while (Cursor < End) {
      const char Character = Text[Cursor++];
      if (Character == Quote) {
        if (Quote == '\'' && Cursor < End && Text[Cursor] == '\'') {
          Value.push_back('\'');
          ++Cursor;
          continue;
        }
        Style = Quote == '\'' ? ScalarStyle::SingleQuoted : ScalarStyle::DoubleQuoted;
        Range = Parsed.Source.range(Begin, Cursor);
        return true;
      }

      if (Quote == '\'' || Character != '\\') {
        if (static_cast<unsigned char>(Character) < 0x20U && Character != '\t') {
          diagnose(Cursor - 1, Cursor, "control character is not allowed in quoted YAML scalar");
          return false;
        }
        Value.push_back(Character);
        continue;
      }

      if (Cursor >= End) {
        diagnose(Cursor - 1, Cursor, "unterminated escape in YAML scalar");
        return false;
      }
      const char Escape = Text[Cursor++];
      switch (Escape) {
      case '0':
        Value.push_back('\0');
        break;
      case 'a':
        Value.push_back('\a');
        break;
      case 'b':
        Value.push_back('\b');
        break;
      case 't':
      case '\t':
        Value.push_back('\t');
        break;
      case 'n':
        Value.push_back('\n');
        break;
      case 'v':
        Value.push_back('\v');
        break;
      case 'f':
        Value.push_back('\f');
        break;
      case 'r':
        Value.push_back('\r');
        break;
      case 'e':
        Value.push_back(static_cast<char>(0x1B));
        break;
      case ' ':
        Value.push_back(' ');
        break;
      case '"':
        Value.push_back('"');
        break;
      case '/':
        Value.push_back('/');
        break;
      case '\\':
        Value.push_back('\\');
        break;
      case 'N':
        appendUTF8(Value, 0x85U);
        break;
      case '_':
        appendUTF8(Value, 0xA0U);
        break;
      case 'L':
        appendUTF8(Value, 0x2028U);
        break;
      case 'P':
        appendUTF8(Value, 0x2029U);
        break;
      case 'x':
        if (!parseUnicodeEscape(Cursor, End, 2, Value))
          return false;
        break;
      case 'u':
        if (!parseUnicodeEscape(Cursor, End, 4, Value))
          return false;
        break;
      case 'U':
        if (!parseUnicodeEscape(Cursor, End, 8, Value))
          return false;
        break;
      default:
        diagnose(Cursor - 1, Cursor, "unknown escape in double-quoted YAML scalar");
        return false;
      }
    }

    diagnose(Begin, End, "unterminated quoted YAML scalar");
    return false;
  }

  [[nodiscard]] bool parseUnicodeEscape(std::size_t &Cursor, std::size_t End,
                                        std::size_t DigitCount, std::string &Output) {
    if (End - Cursor < DigitCount) {
      diagnose(Cursor, End, "incomplete Unicode escape in YAML scalar");
      return false;
    }
    std::uint32_t CodePoint = 0;
    for (std::size_t Index = 0; Index < DigitCount; ++Index) {
      if (!isHexDigit(Parsed.Source.text()[Cursor + Index])) {
        diagnose(Cursor + Index, Cursor + Index + 1,
                 "invalid hexadecimal digit in YAML Unicode escape");
        return false;
      }
      CodePoint = (CodePoint << 4U) | hexValue(Parsed.Source.text()[Cursor + Index]);
    }
    Cursor += DigitCount;
    if (CodePoint > 0x10FFFFU || (CodePoint >= 0xD800U && CodePoint <= 0xDFFFU)) {
      diagnose(Cursor - DigitCount, Cursor, "invalid Unicode scalar value in YAML escape");
      return false;
    }
    appendUTF8(Output, CodePoint);
    return true;
  }

  [[nodiscard]] NodeID parseBlockScalar(std::size_t &LineIndex, std::size_t IndicatorStart,
                                        std::size_t ParentIndent, std::size_t Depth,
                                        std::size_t LimitLine) {
    if (!checkDepth(Depth))
      return {};
    const std::string_view Text = Parsed.Source.text();
    const char Indicator = Text[IndicatorStart];
    char Chomping = '\0';
    std::optional<unsigned> ExplicitIndent;
    std::size_t Cursor = IndicatorStart + 1;
    while (Cursor < Lines[LineIndex].ContentEnd && !isAsciiSpace(Text[Cursor]) &&
           Text[Cursor] != '#') {
      const char Header = Text[Cursor++];
      if ((Header == '+' || Header == '-') && Chomping == '\0') {
        Chomping = Header;
        continue;
      }
      if (Header >= '1' && Header <= '9' && !ExplicitIndent) {
        ExplicitIndent = static_cast<unsigned>(Header - '0');
        continue;
      }
      diagnose(Cursor - 1, Cursor, "invalid YAML block scalar header");
      return {};
    }
    while (Cursor < Lines[LineIndex].ContentEnd && isAsciiSpace(Text[Cursor]))
      ++Cursor;
    if (Cursor < Lines[LineIndex].ContentEnd && Text[Cursor] != '#') {
      diagnose(Cursor, Lines[LineIndex].ContentEnd,
               "unexpected characters after YAML block scalar header");
      return {};
    }

    const std::size_t HeaderLine = LineIndex;
    std::size_t FirstContentLine = LineIndex + 1;
    std::size_t FirstNonBlank = FirstContentLine;
    while (FirstNonBlank < LimitLine && isWhitespaceOnlyLine(FirstNonBlank))
      ++FirstNonBlank;

    std::size_t ContentIndent = ParentIndent + 1;
    if (ExplicitIndent)
      ContentIndent = ParentIndent + *ExplicitIndent;
    else if (FirstNonBlank < LimitLine)
      ContentIndent = Lines[FirstNonBlank].Indent;

    if (FirstNonBlank < LimitLine && Lines[FirstNonBlank].Indent <= ParentIndent) {
      FirstContentLine = FirstNonBlank;
      ContentIndent = ParentIndent + 1;
    } else if (FirstNonBlank < LimitLine && Lines[FirstNonBlank].Indent < ContentIndent) {
      diagnose(Lines[FirstNonBlank].Begin, Lines[FirstNonBlank].ContentEnd,
               "block scalar content is less indented than its header");
      return {};
    }

    struct ScalarLine final {
      std::string Text;
      bool HasLineBreak = false;
      bool IsBlank = false;
      bool IsMoreIndented = false;
    };
    std::vector<ScalarLine> ScalarLines;
    std::size_t CurrentLine = FirstContentLine;
    while (CurrentLine < LimitLine) {
      const PhysicalLine &Line = Lines[CurrentLine];
      const bool IsWhitespaceOnly = isWhitespaceOnlyLine(CurrentLine);
      if (!IsWhitespaceOnly && Line.Indent < ContentIndent)
        break;

      ScalarLine ScalarPart;
      ScalarPart.HasLineBreak = Line.End > Line.ContentEnd;
      ScalarPart.IsBlank = IsWhitespaceOnly;
      ScalarPart.IsMoreIndented = !IsWhitespaceOnly && Line.Indent > ContentIndent;
      if (!IsWhitespaceOnly) {
        const std::size_t TextBegin = Line.Begin + ContentIndent;
        ScalarPart.Text = std::string(Text.substr(TextBegin, Line.ContentEnd - TextBegin));
      }
      ScalarLines.push_back(std::move(ScalarPart));
      ++CurrentLine;
    }

    std::string Value;
    for (std::size_t Index = 0; Index < ScalarLines.size(); ++Index) {
      const ScalarLine &Line = ScalarLines[Index];
      Value += Line.Text;
      if (!Line.HasLineBreak)
        continue;

      bool FoldBreak = false;
      if (Indicator == '>' && Index + 1 < ScalarLines.size()) {
        const ScalarLine &Next = ScalarLines[Index + 1];
        FoldBreak = !Line.IsBlank && !Next.IsBlank && !Line.IsMoreIndented && !Next.IsMoreIndented;
      }
      Value.push_back(FoldBreak ? ' ' : '\n');
    }

    if (Chomping != '+') {
      while (!Value.empty() && Value.back() == '\n')
        Value.pop_back();
      if (Chomping != '-' && !ScalarLines.empty() && ScalarLines.back().HasLineBreak)
        Value.push_back('\n');
    }

    LineIndex = CurrentLine;
    const std::size_t End = CurrentLine > FirstContentLine ? Lines[CurrentLine - 1].ContentEnd
                                                           : Lines[HeaderLine].ContentEnd;
    return addScalarNode(IndicatorStart, End,
                         Indicator == '|' ? ScalarStyle::Literal : ScalarStyle::Folded,
                         std::move(Value));
  }

  [[nodiscard]] bool parseMappingKey(std::size_t Begin, std::size_t End, std::string &Key,
                                     TextRange &Range) {
    const std::string_view Text = Parsed.Source.text();
    while (Begin < End && isAsciiSpace(Text[Begin]))
      ++Begin;
    while (End > Begin && isAsciiSpace(Text[End - 1]))
      --End;
    if (Begin == End) {
      diagnose(Begin, End, "empty YAML mapping keys are not supported");
      return false;
    }

    if (Text[Begin] == '\'' || Text[Begin] == '"') {
      std::size_t Cursor = Begin;
      ScalarStyle Style = ScalarStyle::Plain;
      if (!parseQuotedText(Cursor, End, Key, Style, Range))
        return false;
      while (Cursor < End && isAsciiSpace(Text[Cursor]))
        ++Cursor;
      if (Cursor != End) {
        diagnose(Cursor, End, "unexpected characters after quoted YAML mapping key");
        return false;
      }
      return true;
    }
    if (Text[Begin] == '?' || Text[Begin] == '[' || Text[Begin] == '{') {
      diagnose(Begin, End, "complex YAML mapping keys are not supported");
      return false;
    }
    Key = std::string(Text.substr(Begin, End - Begin));
    Range = Parsed.Source.range(Begin, End);
    return true;
  }

  [[nodiscard]] std::optional<std::size_t> findBlockMappingColon(std::size_t Begin,
                                                                 std::size_t End) const noexcept {
    const std::string_view Text = Parsed.Source.text();
    char Quote = '\0';
    unsigned FlowDepth = 0;
    for (std::size_t Cursor = Begin; Cursor < End; ++Cursor) {
      const char Character = Text[Cursor];
      if (Quote == '\'') {
        if (Character == '\'' && Cursor + 1 < End && Text[Cursor + 1] == '\'') {
          ++Cursor;
        } else if (Character == '\'') {
          Quote = '\0';
        }
        continue;
      }
      if (Quote == '"') {
        if (Character == '\\' && Cursor + 1 < End) {
          ++Cursor;
        } else if (Character == '"') {
          Quote = '\0';
        }
        continue;
      }
      if (Character == '\'' || Character == '"') {
        Quote = Character;
        continue;
      }
      if (Character == '[' || Character == '{') {
        ++FlowDepth;
        continue;
      }
      if ((Character == ']' || Character == '}') && FlowDepth > 0) {
        --FlowDepth;
        continue;
      }
      if (Character == '#' && (Cursor == Begin || isAsciiSpace(Text[Cursor - 1])))
        return std::nullopt;
      if (Character != ':' || FlowDepth != 0)
        continue;
      if (Cursor + 1 == End || isAsciiSpace(Text[Cursor + 1]) || Text[Cursor + 1] == '#')
        return Cursor;
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isSequenceIndicator(std::size_t Offset, std::size_t End) const noexcept {
    if (Offset >= End || Parsed.Source.text()[Offset] != '-')
      return false;
    return Offset + 1 == End || isAsciiSpace(Parsed.Source.text()[Offset + 1]) ||
           Parsed.Source.text()[Offset + 1] == '#';
  }

  void skipFlowSpace(std::size_t &Cursor, std::size_t End) const noexcept {
    const std::string_view Text = Parsed.Source.text();
    while (Cursor < End) {
      if (isAsciiSpace(Text[Cursor]) || Text[Cursor] == '\r' || Text[Cursor] == '\n') {
        ++Cursor;
        continue;
      }
      if (Text[Cursor] != '#')
        return;
      while (Cursor < End && Text[Cursor] != '\n')
        ++Cursor;
    }
  }

  [[nodiscard]] bool isFlowBlockSequenceStart(std::size_t Cursor, std::size_t End) const noexcept {
    if (Cursor >= End)
      return false;
    const std::size_t LineIndex = Parsed.Source.positionAt(Cursor).Line - 1;
    if (LineIndex >= Lines.size())
      return false;
    const PhysicalLine &Line = Lines[LineIndex];
    return Cursor == Line.Begin + Line.Indent && isSequenceIndicator(Cursor, Line.ContentEnd);
  }

  [[nodiscard]] bool isWhitespaceOnlyLine(std::size_t LineIndex) const noexcept {
    const PhysicalLine &Line = Lines[LineIndex];
    const std::string_view Text = Parsed.Source.text();
    for (std::size_t Cursor = Line.Begin; Cursor < Line.ContentEnd; ++Cursor) {
      if (!isAsciiSpace(Text[Cursor]))
        return false;
    }
    return true;
  }

  [[nodiscard]] bool checkDepth(std::size_t Depth) {
    if (Depth <= Options.MaximumDepth)
      return true;
    diagnose(0, 0, "maximum YAML nesting depth exceeded");
    return false;
  }

  [[nodiscard]] NodeID addNullNode(std::size_t Begin, std::size_t End) {
    return addNode(Node(NodeKind::Null, Parsed.Source.range(Begin, End)));
  }

  [[nodiscard]] NodeID addScalarNode(std::size_t Begin, std::size_t End, ScalarStyle Style,
                                     std::string Value) {
    Node NewNode(NodeKind::Scalar, Parsed.Source.range(Begin, End));
    NewNode.ScalarStyleValue = Style;
    NewNode.Scalar = std::move(Value);
    return addNode(std::move(NewNode));
  }

  [[nodiscard]] NodeID addSequenceNode(std::size_t Begin, std::size_t End, CollectionStyle Style,
                                       std::vector<NodeID> Items) {
    Node NewNode(NodeKind::Sequence, Parsed.Source.range(Begin, End));
    NewNode.CollectionStyleValue = Style;
    NewNode.Sequence = std::move(Items);
    return addNode(std::move(NewNode));
  }

  [[nodiscard]] NodeID addMappingNode(std::size_t Begin, std::size_t End, CollectionStyle Style,
                                      std::vector<MappingEntry> Entries) {
    Node NewNode(NodeKind::Mapping, Parsed.Source.range(Begin, End));
    NewNode.CollectionStyleValue = Style;
    NewNode.Mapping = std::move(Entries);
    return addNode(std::move(NewNode));
  }

  [[nodiscard]] NodeID addNode(Node NewNode) {
    if (Parsed.Nodes.size() >= Options.MaximumNodes) {
      diagnose(NewNode.range().Begin.Offset, NewNode.range().End.Offset,
               "maximum YAML node count exceeded");
      return {};
    }
    const auto RawID = static_cast<std::uint32_t>(Parsed.Nodes.size());
    Parsed.Nodes.push_back(std::move(NewNode));
    return NodeID(RawID);
  }

  Stream Parsed;
  ParseOptions Options;
  std::vector<PhysicalLine> Lines;
  std::vector<TextDiagnostic> Diagnostics;
  bool Failed = false;
};

ParseResult parse(std::string SourceName, std::string SourceText, ParseOptions Options) {
  return Parser(std::move(SourceName), std::move(SourceText), Options).run();
}

} // namespace rcc::yaml
