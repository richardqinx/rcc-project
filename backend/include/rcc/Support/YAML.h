// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_SUPPORT_YAML_H
#define RCC_SUPPORT_YAML_H

#include "rcc/Support/TextDiagnostic.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::yaml {

class Parser;

class NodeID final {
public:
  constexpr NodeID() noexcept = default;

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != InvalidValue;
  }

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(NodeID Left, NodeID Right) noexcept = default;

private:
  static constexpr std::uint32_t InvalidValue = std::numeric_limits<std::uint32_t>::max();

  explicit constexpr NodeID(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = InvalidValue;

  friend class Parser;
  friend class Stream;
};

enum class NodeKind : std::uint8_t {
  Null,
  Scalar,
  Sequence,
  Mapping,
};

enum class ScalarStyle : std::uint8_t {
  Plain,
  SingleQuoted,
  DoubleQuoted,
  Literal,
  Folded,
};

enum class CollectionStyle : std::uint8_t {
  Block,
  Flow,
};

struct MappingEntry final {
  std::string Key;
  TextRange KeyRange;
  NodeID Value;
};

class Node final {
public:
  [[nodiscard]] NodeKind kind() const noexcept {
    return Kind;
  }

  [[nodiscard]] const TextRange &range() const noexcept {
    return Range;
  }

  [[nodiscard]] ScalarStyle scalarStyle() const noexcept {
    return ScalarStyleValue;
  }

  [[nodiscard]] CollectionStyle collectionStyle() const noexcept {
    return CollectionStyleValue;
  }

  [[nodiscard]] std::string_view scalar() const noexcept {
    return Scalar;
  }

  [[nodiscard]] std::span<const NodeID> sequence() const noexcept {
    return Sequence;
  }

  [[nodiscard]] std::span<const MappingEntry> mapping() const noexcept {
    return Mapping;
  }

  [[nodiscard]] std::optional<NodeID> mappingValue(std::string_view Key) const noexcept;

private:
  explicit Node(NodeKind Kind, TextRange Range) : Kind(Kind), Range(Range) {}

  NodeKind Kind;
  TextRange Range;
  ScalarStyle ScalarStyleValue = ScalarStyle::Plain;
  CollectionStyle CollectionStyleValue = CollectionStyle::Block;
  std::string Scalar;
  std::vector<NodeID> Sequence;
  std::vector<MappingEntry> Mapping;

  friend class Parser;
};

struct Document final {
  NodeID Root;
  TextRange Range;
  bool HasExplicitStart = false;
  bool HasExplicitEnd = false;
};

class Stream final {
public:
  Stream(const Stream &) = delete;
  Stream &operator=(const Stream &) = delete;
  Stream(Stream &&) noexcept = default;
  Stream &operator=(Stream &&) noexcept = default;
  ~Stream() = default;

  [[nodiscard]] const TextSource &source() const noexcept {
    return Source;
  }

  [[nodiscard]] std::span<const Document> documents() const noexcept {
    return Documents;
  }

  [[nodiscard]] const Node *node(NodeID ID) const noexcept;
  [[nodiscard]] std::size_t nodeCount() const noexcept {
    return Nodes.size();
  }

private:
  Stream(std::string SourceName, std::string SourceText)
      : Source(std::move(SourceName), std::move(SourceText)) {}

  TextSource Source;
  std::vector<Node> Nodes;
  std::vector<Document> Documents;

  friend class Parser;
};

struct ParseOptions final {
  std::size_t MaximumDepth = 128;
  std::size_t MaximumNodes = 1U << 20U;
};

class ParseResult final {
public:
  ParseResult(const ParseResult &) = delete;
  ParseResult &operator=(const ParseResult &) = delete;
  ParseResult(ParseResult &&) noexcept = default;
  ParseResult &operator=(ParseResult &&) noexcept = default;
  ~ParseResult() = default;

  [[nodiscard]] bool succeeded() const noexcept {
    return ParsedStream.has_value() && Diagnostics.empty();
  }

  [[nodiscard]] const Stream *stream() const noexcept {
    return ParsedStream.has_value() ? ParsedStream.operator->() : nullptr;
  }

  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept {
    return Diagnostics;
  }

private:
  ParseResult() = default;

  std::optional<Stream> ParsedStream;
  std::vector<TextDiagnostic> Diagnostics;

  friend class Parser;
};

[[nodiscard]] ParseResult parse(std::string SourceName, std::string SourceText,
                                ParseOptions Options = {});

} // namespace rcc::yaml

#endif // RCC_SUPPORT_YAML_H
