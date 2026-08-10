// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/MIRDocument.h"

#include "MIRDocumentParser.h"

#include <string>
#include <utility>

namespace rcc::mir {

MIRDocument::MIRDocument() = default;
MIRDocument::MIRDocument(CompilationUnit Unit) : Unit(std::move(Unit)) {}
MIRDocument::MIRDocument(MIRDocument &&) noexcept = default;
MIRDocument &MIRDocument::operator=(MIRDocument &&) noexcept = default;
MIRDocument::~MIRDocument() = default;

MIRDocumentParseResult::MIRDocumentParseResult(MIRDocumentParseResult &&) noexcept = default;
MIRDocumentParseResult &
MIRDocumentParseResult::operator=(MIRDocumentParseResult &&) noexcept = default;
MIRDocumentParseResult::~MIRDocumentParseResult() = default;

MIRDocumentParseResult parseMIRDocument(const TargetDescription &Target, std::string SourceName,
                                        std::string SourceText, MIRDocumentParserOptions Options) {
  return MIRDocumentParser(Target, std::move(SourceName), std::move(SourceText), std::move(Options))
      .run();
}

} // namespace rcc::mir
