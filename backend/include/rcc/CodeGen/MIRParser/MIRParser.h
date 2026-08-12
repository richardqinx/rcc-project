// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MIRPARSER_MIRPARSER_H
#define RCC_CODEGEN_MIRPARSER_MIRPARSER_H

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/MIRParser/MIParser.h"
#include "rcc/CodeGen/StableID.h"
#include "rcc/MIR/EmbeddedIR.h"
#include "rcc/Support/TextDiagnostic.h"
#include "rcc/Support/YAML.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcc {

class TargetDescription;

namespace mir {

struct MIRFunctionDocumentInfo final {
  MachineFunctionStableID Function;
  std::optional<bool> NoPHIs;
  std::optional<bool> IsSSA;
  std::optional<bool> NoVRegs;
  std::optional<bool> HasFakeUses;

  friend bool operator==(const MIRFunctionDocumentInfo &Left,
                         const MIRFunctionDocumentInfo &Right) = default;
};

class MIRDocument final {
public:
  MIRDocument();
  explicit MIRDocument(CompilationUnit Unit);
  MIRDocument(const MIRDocument &) = delete;
  MIRDocument &operator=(const MIRDocument &) = delete;
  MIRDocument(MIRDocument &&) noexcept;
  MIRDocument &operator=(MIRDocument &&) noexcept;
  ~MIRDocument();

  [[nodiscard]] CompilationUnit &unit() noexcept {
    return Unit;
  }

  [[nodiscard]] const CompilationUnit &unit() const noexcept {
    return Unit;
  }

  [[nodiscard]] const EmbeddedIRReferenceIndex *embeddedIR() const noexcept {
    return EmbeddedIR ? &*EmbeddedIR : nullptr;
  }

  [[nodiscard]] std::span<const MIRFunctionDocumentInfo> functionDocumentInfo() const noexcept {
    return FunctionInfo;
  }

  [[nodiscard]] std::span<const MISyncScopeSlot> syncScopes() const noexcept {
    return SyncScopes;
  }

private:
  CompilationUnit Unit;
  std::optional<EmbeddedIRReferenceIndex> EmbeddedIR;
  std::vector<MIRFunctionDocumentInfo> FunctionInfo;
  std::vector<MISyncScopeSlot> SyncScopes;

  friend class MIRDocumentParser;
};

struct MIRDocumentParserOptions final {
  yaml::ParseOptions YAML;
  MIParserOptions MachineInstructions;
  std::string DataLayout;
  bool VerifyMachineFunctions = true;
};

class MIRDocumentParseResult final {
public:
  MIRDocumentParseResult(const MIRDocumentParseResult &) = delete;
  MIRDocumentParseResult &operator=(const MIRDocumentParseResult &) = delete;
  MIRDocumentParseResult(MIRDocumentParseResult &&) noexcept;
  MIRDocumentParseResult &operator=(MIRDocumentParseResult &&) noexcept;
  ~MIRDocumentParseResult();

  [[nodiscard]] bool succeeded() const noexcept {
    return Document.has_value() && Diagnostics.empty();
  }

  [[nodiscard]] MIRDocument *document() noexcept {
    return Document ? &*Document : nullptr;
  }

  [[nodiscard]] const MIRDocument *document() const noexcept {
    return Document ? &*Document : nullptr;
  }

  [[nodiscard]] const TextSource &source() const noexcept {
    return Source;
  }

  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept {
    return Diagnostics;
  }

private:
  MIRDocumentParseResult(std::string SourceName, std::string SourceText)
      : Source(std::move(SourceName), std::move(SourceText)) {}

  TextSource Source;
  std::optional<MIRDocument> Document;
  std::vector<TextDiagnostic> Diagnostics;

  friend class MIRDocumentParser;
};

[[nodiscard]] MIRDocumentParseResult parseMIRDocument(const TargetDescription &Target,
                                                      std::string SourceName,
                                                      std::string SourceText,
                                                      MIRDocumentParserOptions Options = {});

} // namespace mir
} // namespace rcc

#endif // RCC_CODEGEN_MIRPARSER_MIRPARSER_H
