// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/EmbeddedIR.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rcc::mir {
namespace {

struct ParsedIdentifier final {
  std::string Name;
  TextRange Range;
  std::size_t End = 0;
};

[[nodiscard]] bool isHorizontalSpace(char Character) noexcept {
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

[[nodiscard]] bool isIdentifierStart(char Character) noexcept {
  return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
         Character == '-' || Character == '$' || Character == '.' || Character == '_';
}

[[nodiscard]] bool isIdentifierBody(char Character) noexcept {
  return isIdentifierStart(Character) || (Character >= '0' && Character <= '9');
}

[[nodiscard]] bool isWordBody(char Character) noexcept {
  return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
         (Character >= '0' && Character <= '9') || Character == '_';
}

[[nodiscard]] std::size_t skipHorizontalSpace(std::string_view Line, std::size_t Position,
                                              std::size_t End) noexcept {
  while (Position < End && isHorizontalSpace(Line[Position]))
    ++Position;
  return Position;
}

[[nodiscard]] bool startsWord(std::string_view Line, std::size_t Position, std::size_t End,
                              std::string_view Word) noexcept {
  if (End - Position < Word.size() || Line.substr(Position, Word.size()) != Word)
    return false;
  const std::size_t WordEnd = Position + Word.size();
  return WordEnd == End || !isWordBody(Line[WordEnd]);
}

} // namespace

const EmbeddedIRReference *EmbeddedIRFunction::block(std::string_view QueryName) const noexcept {
  const auto Position = BlockNameToIndex.find(QueryName);
  if (Position == BlockNameToIndex.end())
    return nullptr;
  return &Blocks[Position->second];
}

const EmbeddedIRReference *EmbeddedIRFunction::alloca(std::string_view QueryName) const noexcept {
  const auto Position = AllocaNameToIndex.find(QueryName);
  if (Position == AllocaNameToIndex.end())
    return nullptr;
  return &Allocas[Position->second];
}

const EmbeddedIRFunction *EmbeddedIRReferenceIndex::function(std::string_view Name) const noexcept {
  const auto Position = FunctionNameToIndex.find(Name);
  if (Position == FunctionNameToIndex.end())
    return nullptr;
  return &Functions[Position->second];
}

const EmbeddedIRReference *
EmbeddedIRReferenceIndex::globalObject(std::string_view Name) const noexcept {
  const auto Position = GlobalNameToIndex.find(Name);
  if (Position == GlobalNameToIndex.end())
    return nullptr;
  return &GlobalObjects[Position->second];
}

const EmbeddedIRReference *
EmbeddedIRReferenceIndex::metadata(std::string_view Name) const noexcept {
  const auto Position = MetadataNameToIndex.find(Name);
  if (Position == MetadataNameToIndex.end())
    return nullptr;
  return &MetadataDefinitions[Position->second];
}

class EmbeddedIRIndexer final {
public:
  EmbeddedIRIndexer(std::string SourceName, std::string SourceText)
      : Index(std::move(SourceName), std::move(SourceText)) {}

  [[nodiscard]] EmbeddedIRIndexResult run() {
    const std::size_t NulOffset = Index.Source.text().find('\0');
    if (NulOffset != std::string_view::npos) {
      diagnose(NulOffset, NulOffset + 1, "NUL bytes are not allowed in embedded LLVM IR");
      return failureResult();
    }

    for (std::size_t LineNumber = 1; LineNumber <= Index.Source.lineCount(); ++LineNumber) {
      const std::string_view Line = Index.Source.line(LineNumber);
      const std::size_t LineOffset = Index.Source.lineOffsets()[LineNumber - 1];
      const std::size_t ContentEnd = findCommentStart(Line);
      if (ActiveFunction) {
        parseActiveFunctionLine(Line, LineOffset, ContentEnd);
      } else {
        parseTopLevelLine(Line, LineOffset, ContentEnd);
      }
      if (Failed)
        return failureResult();
    }

    if (ActiveFunction) {
      const EmbeddedIRFunction &Function = Index.Functions[ActiveFunction.value()];
      diagnose(Function.nameRange().Begin.Offset, Function.nameRange().End.Offset,
               "embedded LLVM IR function definition has no closing brace");
      return failureResult();
    }

    EmbeddedIRIndexResult Result;
    Result.Index.emplace(std::move(Index));
    return Result;
  }

private:
  [[nodiscard]] EmbeddedIRIndexResult failureResult() {
    EmbeddedIRIndexResult Result;
    Result.Diagnostics = std::move(Diagnostics);
    return Result;
  }

  void diagnose(std::size_t Begin, std::size_t End, std::string Message) {
    if (Failed)
      return;
    Failed = true;
    Diagnostics.push_back(TextDiagnostic{
        .Severity = DiagnosticSeverity::Error,
        .Category = "mir-embedded-ir",
        .Message = std::move(Message),
        .SourceName = std::string(Index.Source.name()),
        .Range = Index.Source.range(Begin, End),
    });
  }

  [[nodiscard]] std::size_t findCommentStart(std::string_view Line) const noexcept {
    bool InQuote = false;
    for (std::size_t Position = 0; Position < Line.size(); ++Position) {
      const char Character = Line[Position];
      if (InQuote && Character == '\\' && Position + 2 < Line.size() &&
          isHexDigit(Line[Position + 1]) && isHexDigit(Line[Position + 2])) {
        Position += 2;
        continue;
      }
      if (Character == '"') {
        InQuote = !InQuote;
        continue;
      }
      if (!InQuote && Character == ';')
        return Position;
    }
    return Line.size();
  }

  [[nodiscard]] std::optional<ParsedIdentifier>
  parseIdentifier(std::string_view Line, std::size_t LineOffset, std::size_t Position,
                  std::size_t End, std::optional<char> Prefix) {
    const std::size_t Begin = Position;
    if (Prefix) {
      if (Position >= End || Line[Position] != *Prefix)
        return std::nullopt;
      ++Position;
    }
    if (Position >= End)
      return std::nullopt;

    std::string Name;
    if (Line[Position] == '"') {
      ++Position;
      while (Position < End && Line[Position] != '"') {
        const char Character = Line[Position++];
        if (Character != '\\') {
          if (static_cast<unsigned char>(Character) < 0x20U) {
            diagnose(LineOffset + Position - 1, LineOffset + Position,
                     "control character is not allowed in quoted LLVM IR identifier");
            return std::nullopt;
          }
          Name.push_back(Character);
          continue;
        }
        if (End - Position < 2 || !isHexDigit(Line[Position]) || !isHexDigit(Line[Position + 1])) {
          diagnose(LineOffset + Position - 1, LineOffset + std::min(Position + 2, End),
                   "LLVM IR identifier escape must contain two hexadecimal digits");
          return std::nullopt;
        }
        const unsigned Value = (hexValue(Line[Position]) << 4U) | hexValue(Line[Position + 1]);
        Name.push_back(static_cast<char>(Value));
        Position += 2;
      }
      if (Position >= End || Line[Position] != '"') {
        diagnose(LineOffset + Begin, LineOffset + End, "unterminated quoted LLVM IR identifier");
        return std::nullopt;
      }
      ++Position;
    } else if (Line[Position] >= '0' && Line[Position] <= '9') {
      const std::size_t NameBegin = Position;
      while (Position < End && Line[Position] >= '0' && Line[Position] <= '9')
        ++Position;
      Name = std::string(Line.substr(NameBegin, Position - NameBegin));
    } else if (isIdentifierStart(Line[Position])) {
      const std::size_t NameBegin = Position;
      while (Position < End && isIdentifierBody(Line[Position]))
        ++Position;
      Name = std::string(Line.substr(NameBegin, Position - NameBegin));
    } else {
      return std::nullopt;
    }

    return ParsedIdentifier{
        .Name = std::move(Name),
        .Range = Index.Source.range(LineOffset + Begin, LineOffset + Position),
        .End = Position,
    };
  }

  [[nodiscard]] std::optional<std::pair<std::string, TextRange>>
  parseStringLiteral(std::string_view Line, std::size_t LineOffset, std::size_t &Position,
                     std::size_t End) {
    if (Position >= End || Line[Position] != '"')
      return std::nullopt;
    const std::size_t Begin = Position++;
    std::string Value;
    while (Position < End && Line[Position] != '"') {
      const char Character = Line[Position++];
      if (Character != '\\') {
        if (static_cast<unsigned char>(Character) < 0x20U) {
          diagnose(LineOffset + Position - 1, LineOffset + Position,
                   "control character is not allowed in LLVM IR string");
          return std::nullopt;
        }
        Value.push_back(Character);
        continue;
      }
      if (End - Position < 2 || !isHexDigit(Line[Position]) || !isHexDigit(Line[Position + 1])) {
        diagnose(LineOffset + Position - 1, LineOffset + std::min(Position + 2, End),
                 "LLVM IR string escape must contain two hexadecimal digits");
        return std::nullopt;
      }
      const unsigned Decoded = (hexValue(Line[Position]) << 4U) | hexValue(Line[Position + 1]);
      Value.push_back(static_cast<char>(Decoded));
      Position += 2;
    }
    if (Position >= End || Line[Position] != '"') {
      diagnose(LineOffset + Begin, LineOffset + End, "unterminated LLVM IR string");
      return std::nullopt;
    }
    ++Position;
    return std::pair<std::string, TextRange>(
        std::move(Value), Index.Source.range(LineOffset + Begin, LineOffset + Position));
  }

  void parseTopLevelLine(std::string_view Line, std::size_t LineOffset, std::size_t End) {
    std::size_t Position = skipHorizontalSpace(Line, 0, End);
    if (Position == End)
      return;

    if (startsWord(Line, Position, End, "target")) {
      parseTargetFact(Line, LineOffset, Position, End);
      return;
    }
    if (startsWord(Line, Position, End, "define")) {
      parseFunctionHeader(Line, LineOffset, Position, End, true);
      return;
    }
    if (startsWord(Line, Position, End, "declare")) {
      parseFunctionHeader(Line, LineOffset, Position, End, false);
      return;
    }
    if (Line[Position] == '@') {
      parseGlobalObject(Line, LineOffset, Position, End);
      return;
    }
    if (Line[Position] == '!')
      parseMetadataDefinition(Line, LineOffset, Position, End);
  }

  void parseTargetFact(std::string_view Line, std::size_t LineOffset, std::size_t Position,
                       std::size_t End) {
    Position += std::string_view("target").size();
    Position = skipHorizontalSpace(Line, Position, End);
    const bool IsTriple = startsWord(Line, Position, End, "triple");
    const bool IsDataLayout = startsWord(Line, Position, End, "datalayout");
    if (!IsTriple && !IsDataLayout)
      return;
    Position +=
        IsTriple ? std::string_view("triple").size() : std::string_view("datalayout").size();
    Position = skipHorizontalSpace(Line, Position, End);
    if (Position >= End || Line[Position] != '=') {
      diagnose(LineOffset + Position, LineOffset + std::min(Position + 1, End),
               "expected '=' in embedded LLVM IR target fact");
      return;
    }
    ++Position;
    Position = skipHorizontalSpace(Line, Position, End);
    auto Value = parseStringLiteral(Line, LineOffset, Position, End);
    if (!Value)
      return;
    Position = skipHorizontalSpace(Line, Position, End);
    if (Position != End) {
      diagnose(LineOffset + Position, LineOffset + End,
               "unexpected content after embedded LLVM IR target fact");
      return;
    }

    std::optional<std::string> &Storage = IsTriple ? Index.TargetTriple : Index.DataLayout;
    std::optional<TextRange> &Range = IsTriple ? Index.TargetTripleRange : Index.DataLayoutRange;
    if (Storage) {
      diagnose(Value->second.Begin.Offset, Value->second.End.Offset,
               IsTriple ? "duplicate embedded LLVM IR target triple"
                        : "duplicate embedded LLVM IR data layout");
      return;
    }
    Storage = std::move(Value->first);
    Range = Value->second;
  }

  void parseFunctionHeader(std::string_view Line, std::size_t LineOffset, std::size_t Position,
                           std::size_t End, bool IsDefinition) {
    Position +=
        IsDefinition ? std::string_view("define").size() : std::string_view("declare").size();
    std::optional<ParsedIdentifier> Name;
    while (Position < End) {
      if (Line[Position] == '"') {
        std::size_t StringPosition = Position;
        auto Ignored = parseStringLiteral(Line, LineOffset, StringPosition, End);
        if (!Ignored)
          return;
        Position = StringPosition;
        continue;
      }
      if (Line[Position] == '@') {
        Name = parseIdentifier(Line, LineOffset, Position, End, '@');
        break;
      }
      ++Position;
    }
    if (!Name) {
      diagnose(LineOffset, LineOffset + End,
               "embedded LLVM IR function header has no global identifier");
      return;
    }

    auto FunctionIndex = addFunction(*Name, IsDefinition);
    if (!FunctionIndex || Failed || !IsDefinition)
      return;
    ActiveFunction = FunctionIndex;
    HeaderParenthesisDepth = 0;
    parseFunctionHeaderRemainder(Line, LineOffset, Name->End, End);
  }

  [[nodiscard]] std::optional<std::size_t> addFunction(const ParsedIdentifier &Name,
                                                       bool IsDefinition) {
    if (Index.GlobalNameToIndex.contains(Name.Name)) {
      diagnose(Name.Range.Begin.Offset, Name.Range.End.Offset,
               "embedded LLVM IR name is used by both a function and a global object");
      return std::nullopt;
    }

    const auto Existing = Index.FunctionNameToIndex.find(Name.Name);
    if (Existing != Index.FunctionNameToIndex.end()) {
      EmbeddedIRFunction &Function = Index.Functions[Existing->second];
      if (IsDefinition && Function.IsDefinition) {
        diagnose(Name.Range.Begin.Offset, Name.Range.End.Offset,
                 "duplicate embedded LLVM IR function definition");
        return std::nullopt;
      }
      if (IsDefinition) {
        Function.IsDefinition = true;
        Function.NameRange = Name.Range;
      }
      return Existing->second;
    }

    const std::size_t IndexValue = Index.Functions.size();
    EmbeddedIRFunction Function;
    Function.Name = Name.Name;
    Function.NameRange = Name.Range;
    Function.IsDefinition = IsDefinition;
    Index.Functions.push_back(std::move(Function));
    Index.FunctionNameToIndex.emplace(Name.Name, IndexValue);
    return IndexValue;
  }

  void parseFunctionHeaderRemainder(std::string_view Line, std::size_t LineOffset,
                                    std::size_t Position, std::size_t End) {
    bool InQuote = false;
    for (; Position < End; ++Position) {
      const char Character = Line[Position];
      if (InQuote && Character == '\\' && Position + 2 < End && isHexDigit(Line[Position + 1]) &&
          isHexDigit(Line[Position + 2])) {
        Position += 2;
        continue;
      }
      if (Character == '"') {
        InQuote = !InQuote;
        continue;
      }
      if (InQuote)
        continue;
      if (Character == '(') {
        ++HeaderParenthesisDepth;
        continue;
      }
      if (Character == ')') {
        if (HeaderParenthesisDepth == 0) {
          diagnose(LineOffset + Position, LineOffset + Position + 1,
                   "unmatched ')' in embedded LLVM IR function header");
          return;
        }
        --HeaderParenthesisDepth;
        continue;
      }
      if (Character == '{' && HeaderParenthesisDepth == 0) {
        FunctionBraceDepth = 1;
        parseFunctionBodySegment(Line, LineOffset, Position + 1, End);
        return;
      }
    }
  }

  void parseActiveFunctionLine(std::string_view Line, std::size_t LineOffset, std::size_t End) {
    if (FunctionBraceDepth == 0) {
      parseFunctionHeaderRemainder(Line, LineOffset, 0, End);
      return;
    }
    parseFunctionBodySegment(Line, LineOffset, 0, End);
  }

  void parseFunctionBodySegment(std::string_view Line, std::size_t LineOffset, std::size_t Begin,
                                std::size_t End) {
    std::size_t BodyEnd = End;
    bool InQuote = false;
    std::size_t Depth = FunctionBraceDepth;
    for (std::size_t Position = Begin; Position < End; ++Position) {
      const char Character = Line[Position];
      if (InQuote && Character == '\\' && Position + 2 < End && isHexDigit(Line[Position + 1]) &&
          isHexDigit(Line[Position + 2])) {
        Position += 2;
        continue;
      }
      if (Character == '"') {
        InQuote = !InQuote;
        continue;
      }
      if (InQuote)
        continue;
      if (Character == '{') {
        ++Depth;
      } else if (Character == '}') {
        if (Depth == 0) {
          diagnose(LineOffset + Position, LineOffset + Position + 1,
                   "unmatched '}' in embedded LLVM IR function");
          return;
        }
        --Depth;
        if (Depth == 0) {
          BodyEnd = Position;
          break;
        }
      }
    }

    if (!ActiveFunction) {
      diagnose(LineOffset + Begin, LineOffset + BodyEnd,
               "internal embedded LLVM IR function indexing state is invalid");
      return;
    }
    EmbeddedIRFunction &Function = Index.Functions[ActiveFunction.value()];
    parseBlockLabel(Line, LineOffset, Begin, BodyEnd, Function);
    if (Failed)
      return;
    parseAlloca(Line, LineOffset, Begin, BodyEnd, Function);
    if (Failed)
      return;

    FunctionBraceDepth = Depth;
    if (FunctionBraceDepth == 0)
      ActiveFunction.reset();
  }

  void parseBlockLabel(std::string_view Line, std::size_t LineOffset, std::size_t Begin,
                       std::size_t End, EmbeddedIRFunction &Function) {
    std::size_t Position = skipHorizontalSpace(Line, Begin, End);
    if (Position >= End)
      return;
    auto Name = parseIdentifier(Line, LineOffset, Position, End, std::nullopt);
    if (!Name || Failed)
      return;
    Position = skipHorizontalSpace(Line, Name->End, End);
    if (Position >= End || Line[Position] != ':')
      return;

    if (Function.BlockNameToIndex.contains(Name->Name)) {
      diagnose(Name->Range.Begin.Offset, Name->Range.End.Offset,
               "duplicate embedded LLVM IR basic block label");
      return;
    }
    const std::size_t BlockIndex = Function.Blocks.size();
    Function.BlockNameToIndex.emplace(Name->Name, BlockIndex);
    Function.Blocks.push_back(EmbeddedIRReference{
        .Name = std::move(Name->Name),
        .Range = Name->Range,
    });
  }

  void parseAlloca(std::string_view Line, std::size_t LineOffset, std::size_t Begin,
                   std::size_t End, EmbeddedIRFunction &Function) {
    bool InQuote = false;
    for (std::size_t Position = Begin; Position < End; ++Position) {
      if (InQuote && Line[Position] == '\\' && Position + 2 < End &&
          isHexDigit(Line[Position + 1]) && isHexDigit(Line[Position + 2])) {
        Position += 2;
        continue;
      }
      if (Line[Position] == '"') {
        InQuote = !InQuote;
        continue;
      }
      if (InQuote || Line[Position] != '%')
        continue;

      auto Name = parseIdentifier(Line, LineOffset, Position, End, '%');
      if (!Name || Failed)
        return;
      Position = skipHorizontalSpace(Line, Name->End, End);
      if (Position >= End || Line[Position] != '=')
        continue;
      ++Position;
      Position = skipHorizontalSpace(Line, Position, End);
      if (!startsWord(Line, Position, End, "alloca"))
        continue;

      if (Function.AllocaNameToIndex.contains(Name->Name)) {
        diagnose(Name->Range.Begin.Offset, Name->Range.End.Offset,
                 "duplicate embedded LLVM IR alloca name");
        return;
      }
      const std::size_t AllocaIndex = Function.Allocas.size();
      Function.AllocaNameToIndex.emplace(Name->Name, AllocaIndex);
      Function.Allocas.push_back(EmbeddedIRReference{
          .Name = std::move(Name->Name),
          .Range = Name->Range,
      });
      return;
    }
  }

  void parseGlobalObject(std::string_view Line, std::size_t LineOffset, std::size_t Position,
                         std::size_t End) {
    auto Name = parseIdentifier(Line, LineOffset, Position, End, '@');
    if (!Name || Failed)
      return;
    Position = skipHorizontalSpace(Line, Name->End, End);
    if (Position >= End || Line[Position] != '=')
      return;
    if (Index.FunctionNameToIndex.contains(Name->Name)) {
      diagnose(Name->Range.Begin.Offset, Name->Range.End.Offset,
               "embedded LLVM IR name is used by both a global object and a function");
      return;
    }
    if (Index.GlobalNameToIndex.contains(Name->Name)) {
      diagnose(Name->Range.Begin.Offset, Name->Range.End.Offset,
               "duplicate embedded LLVM IR global object");
      return;
    }
    const std::size_t GlobalIndex = Index.GlobalObjects.size();
    Index.GlobalNameToIndex.emplace(Name->Name, GlobalIndex);
    Index.GlobalObjects.push_back(EmbeddedIRReference{
        .Name = std::move(Name->Name),
        .Range = Name->Range,
    });
  }

  void parseMetadataDefinition(std::string_view Line, std::size_t LineOffset, std::size_t Position,
                               std::size_t End) {
    auto Name = parseIdentifier(Line, LineOffset, Position, End, '!');
    if (!Name || Failed)
      return;
    Position = skipHorizontalSpace(Line, Name->End, End);
    if (Position >= End || Line[Position] != '=')
      return;
    if (Index.MetadataNameToIndex.contains(Name->Name)) {
      diagnose(Name->Range.Begin.Offset, Name->Range.End.Offset,
               "duplicate embedded LLVM IR metadata definition");
      return;
    }
    const std::size_t MetadataIndex = Index.MetadataDefinitions.size();
    Index.MetadataNameToIndex.emplace(Name->Name, MetadataIndex);
    Index.MetadataDefinitions.push_back(EmbeddedIRReference{
        .Name = std::move(Name->Name),
        .Range = Name->Range,
    });
  }

  EmbeddedIRReferenceIndex Index;
  std::vector<TextDiagnostic> Diagnostics;
  std::optional<std::size_t> ActiveFunction;
  std::size_t HeaderParenthesisDepth = 0;
  std::size_t FunctionBraceDepth = 0;
  bool Failed = false;
};

EmbeddedIRIndexResult indexEmbeddedIR(std::string SourceName, std::string SourceText) {
  return EmbeddedIRIndexer(std::move(SourceName), std::move(SourceText)).run();
}

} // namespace rcc::mir
