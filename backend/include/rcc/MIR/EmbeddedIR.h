// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MIR_EMBEDDEDIR_H
#define RCC_MIR_EMBEDDEDIR_H

#include "rcc/Support/TextDiagnostic.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc::mir {

class EmbeddedIRIndexer;

struct EmbeddedIRReference final {
  std::string Name;
  TextRange Range;
};

struct EmbeddedIRMetadataDefinition final {
  std::string Name;
  TextRange Range;
  std::string DynamicType;
};

struct EmbeddedIRTypeDefinition final {
  std::string Name;
  TextRange Range;
  std::string Definition;
  TextRange DefinitionRange;
  bool NumericName = false;
};

class EmbeddedIRFunction final {
public:
  [[nodiscard]] std::string_view name() const noexcept {
    return Name;
  }

  [[nodiscard]] const TextRange &nameRange() const noexcept {
    return NameRange;
  }

  [[nodiscard]] bool isDefinition() const noexcept {
    return IsDefinition;
  }

  [[nodiscard]] std::span<const EmbeddedIRReference> blocks() const noexcept {
    return Blocks;
  }

  [[nodiscard]] std::span<const EmbeddedIRReference> allocas() const noexcept {
    return Allocas;
  }

  [[nodiscard]] std::span<const EmbeddedIRReference> values() const noexcept {
    return Values;
  }

  [[nodiscard]] const EmbeddedIRReference *block(std::string_view QueryName) const noexcept;
  [[nodiscard]] const EmbeddedIRReference *alloca(std::string_view QueryName) const noexcept;
  [[nodiscard]] const EmbeddedIRReference *value(std::string_view QueryName) const noexcept;

private:
  struct StringHash final {
    using is_transparent = void; // NOLINT(readability-identifier-naming)

    [[nodiscard]] std::size_t operator()(std::string_view Value) const noexcept {
      return std::hash<std::string_view>{}(Value);
    }
  };

  std::string Name;
  TextRange NameRange;
  bool IsDefinition = false;
  std::vector<EmbeddedIRReference> Blocks;
  std::vector<EmbeddedIRReference> Allocas;
  std::vector<EmbeddedIRReference> Values;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> BlockNameToIndex;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> AllocaNameToIndex;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> ValueNameToIndex;

  friend class EmbeddedIRIndexer;
};

class EmbeddedIRReferenceIndex final {
public:
  EmbeddedIRReferenceIndex(const EmbeddedIRReferenceIndex &) = delete;
  EmbeddedIRReferenceIndex &operator=(const EmbeddedIRReferenceIndex &) = delete;
  EmbeddedIRReferenceIndex(EmbeddedIRReferenceIndex &&) noexcept = default;
  EmbeddedIRReferenceIndex &operator=(EmbeddedIRReferenceIndex &&) noexcept = default;
  ~EmbeddedIRReferenceIndex() = default;

  [[nodiscard]] const TextSource &source() const noexcept {
    return Source;
  }

  [[nodiscard]] std::optional<std::string_view> targetTriple() const noexcept {
    if (!TargetTriple)
      return std::nullopt;
    return *TargetTriple;
  }

  [[nodiscard]] std::optional<std::string_view> dataLayout() const noexcept {
    if (!DataLayout)
      return std::nullopt;
    return *DataLayout;
  }

  [[nodiscard]] std::span<const EmbeddedIRFunction> functions() const noexcept {
    return Functions;
  }

  [[nodiscard]] std::span<const EmbeddedIRReference> globalObjects() const noexcept {
    return GlobalObjects;
  }

  [[nodiscard]] std::span<const EmbeddedIRMetadataDefinition>
  metadataDefinitions() const noexcept {
    return MetadataDefinitions;
  }

  [[nodiscard]] std::span<const EmbeddedIRTypeDefinition> typeDefinitions() const noexcept {
    return TypeDefinitions;
  }

  [[nodiscard]] const EmbeddedIRFunction *function(std::string_view Name) const noexcept;
  [[nodiscard]] const EmbeddedIRReference *globalObject(std::string_view Name) const noexcept;
  [[nodiscard]] const EmbeddedIRMetadataDefinition *metadata(std::string_view Name) const noexcept;
  [[nodiscard]] const EmbeddedIRTypeDefinition *typeDefinition(std::string_view Name) const noexcept;
  [[nodiscard]] bool hasGlobalValue(std::string_view Name) const noexcept {
    return function(Name) != nullptr || globalObject(Name) != nullptr;
  }

private:
  struct StringHash final {
    using is_transparent = void; // NOLINT(readability-identifier-naming)

    [[nodiscard]] std::size_t operator()(std::string_view Value) const noexcept {
      return std::hash<std::string_view>{}(Value);
    }
  };

  EmbeddedIRReferenceIndex(std::string SourceName, std::string SourceText)
      : Source(std::move(SourceName), std::move(SourceText)) {}

  TextSource Source;
  std::optional<std::string> TargetTriple;
  std::optional<TextRange> TargetTripleRange;
  std::optional<std::string> DataLayout;
  std::optional<TextRange> DataLayoutRange;
  std::vector<EmbeddedIRFunction> Functions;
  std::vector<EmbeddedIRReference> GlobalObjects;
  std::vector<EmbeddedIRMetadataDefinition> MetadataDefinitions;
  std::vector<EmbeddedIRTypeDefinition> TypeDefinitions;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> FunctionNameToIndex;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> GlobalNameToIndex;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> MetadataNameToIndex;
  std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> TypeNameToIndex;

  friend class EmbeddedIRIndexer;
};

class EmbeddedIRIndexResult final {
public:
  EmbeddedIRIndexResult(const EmbeddedIRIndexResult &) = delete;
  EmbeddedIRIndexResult &operator=(const EmbeddedIRIndexResult &) = delete;
  EmbeddedIRIndexResult(EmbeddedIRIndexResult &&) noexcept = default;
  EmbeddedIRIndexResult &operator=(EmbeddedIRIndexResult &&) noexcept = default;
  ~EmbeddedIRIndexResult() = default;

  [[nodiscard]] bool succeeded() const noexcept {
    return Index.has_value() && Diagnostics.empty();
  }

  [[nodiscard]] const EmbeddedIRReferenceIndex *index() const noexcept {
    return Index.has_value() ? Index.operator->() : nullptr;
  }

  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept {
    return Diagnostics;
  }

  [[nodiscard]] std::optional<EmbeddedIRReferenceIndex> takeIndex() && {
    return std::move(Index);
  }

private:
  EmbeddedIRIndexResult() = default;

  std::optional<EmbeddedIRReferenceIndex> Index;
  std::vector<TextDiagnostic> Diagnostics;

  friend class EmbeddedIRIndexer;
};

[[nodiscard]] EmbeddedIRIndexResult indexEmbeddedIR(std::string SourceName, std::string SourceText);

} // namespace rcc::mir

#endif // RCC_MIR_EMBEDDEDIR_H
