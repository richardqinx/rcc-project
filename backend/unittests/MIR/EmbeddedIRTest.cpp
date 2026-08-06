// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/EmbeddedIR.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int fail(const char *Message) {
  std::cerr << Message << '\n';
  return 1;
}

int testReferenceIndex() {
  const std::string Input = "; @commented = global i8 0\n"
                            "target datalayout = \"e-m:e-p:64:64\"\n"
                            "target triple = \"riscv64-unknown-linux-gnu\"\n"
                            "%pair = type { i32, i32 }\n"
                            "@ordinary = global i32 0\n"
                            "@\"space\\20global\" = external global i8\n"
                            "declare void @declared()\n"
                            "define void @\"function\\20name\"(\n"
                            "  { i32, i32 } %argument\n"
                            ") {\n"
                            "entry:\n"
                            "  %slot = alloca i32, align 4\n"
                            "  %not_an_alloca = add i32 %argument, 2\n"
                            "  br label %\"exit\\20block\"\n"
                            "\"exit\\20block\":\n"
                            "  ret void\n"
                            "}\n"
                            "!0 = !DILocation(line: 1, scope: !1)\n"
                            "!llvm.module.flags = !{!0}\n";

  rcc::mir::EmbeddedIRIndexResult Result = rcc::mir::indexEmbeddedIR("embedded.ll", Input);
  const rcc::mir::EmbeddedIRReferenceIndex *Index = Result.index();
  if (!Result.succeeded() || Index == nullptr)
    return fail("Embedded IR indexing must accept the MIR reference subset");
  if (Index->source().text() != Input)
    return fail("Embedded IR indexing must preserve the original text");
  if (Index->targetTriple() != std::optional<std::string_view>("riscv64-unknown-linux-gnu") ||
      Index->dataLayout() != std::optional<std::string_view>("e-m:e-p:64:64"))
    return fail("Embedded IR indexing must preserve target facts");
  if (Index->functions().size() != 2 || Index->function("declared") == nullptr ||
      Index->function("declared")->isDefinition())
    return fail("Embedded IR indexing must retain function declarations");

  const rcc::mir::EmbeddedIRFunction *Function = Index->function("function name");
  if (Function == nullptr || !Function->isDefinition() || Function->blocks().size() != 2)
    return fail("Embedded IR indexing must decode function and block identifiers");
  if (Function->block("entry") == nullptr || Function->block("exit block") == nullptr)
    return fail("Embedded IR block lookup must use decoded names");
  if (Function->allocas().size() != 1 || Function->alloca("slot") == nullptr ||
      Function->alloca("not_an_alloca") != nullptr)
    return fail("Embedded IR indexing must distinguish named allocas from other local values");
  if (Function->values().size() != 3 || Function->value("argument") == nullptr ||
      Function->value("slot") == nullptr || Function->value("not_an_alloca") == nullptr)
    return fail("Embedded IR indexing must retain arguments and instruction results");

  if (!Index->hasGlobalValue("ordinary") || !Index->hasGlobalValue("space global") ||
      !Index->hasGlobalValue("declared") || Index->hasGlobalValue("commented"))
    return fail("Embedded IR global lookup must include objects and functions but ignore comments");
  if (Index->metadata("0") == nullptr || Index->metadata("llvm.module.flags") == nullptr ||
      Index->metadataDefinitions().size() != 2)
    return fail("Embedded IR indexing must retain numeric and named metadata definitions");
  if (Index->metadata("0")->DynamicType != "DILocation" ||
      Index->metadata("llvm.module.flags")->DynamicType != "MDTuple")
    return fail("Embedded IR indexing must classify metadata dynamic types");
  if (Index->typeDefinitions().size() != 1 || Index->typeDefinition("pair") == nullptr ||
      Index->typeDefinition("pair")->Definition != "{ i32, i32 }")
    return fail("Embedded IR indexing must retain named type definitions");
  return 0;
}

int testDeclarationDefinitionMerge() {
  const std::string Input = "declare void @shared()\n"
                            "define void @shared() {\n"
                            "0:\n"
                            "  ret void\n"
                            "}\n";
  rcc::mir::EmbeddedIRIndexResult Result = rcc::mir::indexEmbeddedIR("merge.ll", Input);
  const rcc::mir::EmbeddedIRReferenceIndex *Index = Result.index();
  if (!Result.succeeded() || Index == nullptr || Index->functions().size() != 1)
    return fail("Embedded IR indexing must merge a declaration with its definition");
  const rcc::mir::EmbeddedIRFunction *Function = Index->function("shared");
  if (Function == nullptr || !Function->isDefinition() || Function->block("0") == nullptr)
    return fail("A later definition must complete its indexed declaration");
  return 0;
}

bool hasSingleDiagnostic(const rcc::mir::EmbeddedIRIndexResult &Result, std::string_view Message) {
  return !Result.succeeded() && Result.diagnostics().size() == 1 &&
         Result.diagnostics()[0].Message == Message;
}

int testDiagnostics() {
  auto DuplicateBlock =
      rcc::mir::indexEmbeddedIR("blocks.ll", "define void @f() {\nentry:\nentry:\n  ret void\n}\n");
  if (!hasSingleDiagnostic(DuplicateBlock, "duplicate embedded LLVM IR basic block label") ||
      DuplicateBlock.diagnostics()[0].Range.Begin.Line != 3)
    return fail("Embedded IR indexing must reject a duplicate block at its second label");

  auto BadEscape = rcc::mir::indexEmbeddedIR("escape.ll", "@\"bad\\q0\" = global i8 0\n");
  if (!hasSingleDiagnostic(BadEscape,
                           "LLVM IR identifier escape must contain two hexadecimal digits"))
    return fail("Embedded IR indexing must reject malformed identifier escapes");

  auto Unterminated =
      rcc::mir::indexEmbeddedIR("function.ll", "define void @f() {\nentry:\n  ret void\n");
  if (!hasSingleDiagnostic(Unterminated,
                           "embedded LLVM IR function definition has no closing brace"))
    return fail("Embedded IR indexing must reject an unterminated function definition");

  auto DuplicateTriple =
      rcc::mir::indexEmbeddedIR("triple.ll", "target triple = \"riscv64-unknown-linux-gnu\"\n"
                                             "target triple = \"riscv64-unknown-linux-gnu\"\n");
  if (!hasSingleDiagnostic(DuplicateTriple, "duplicate embedded LLVM IR target triple"))
    return fail("Embedded IR indexing must reject duplicate target facts");

  auto NameConflict =
      rcc::mir::indexEmbeddedIR("names.ll", "@same = global i32 0\ndeclare void @same()\n");
  if (!hasSingleDiagnostic(NameConflict,
                           "embedded LLVM IR name is used by both a function and a global object"))
    return fail("Embedded IR indexing must reject conflicting global value kinds");

  const std::string NulInput("target triple = \"x\"\0", 20);
  auto Nul = rcc::mir::indexEmbeddedIR("nul.ll", NulInput);
  if (!hasSingleDiagnostic(Nul, "NUL bytes are not allowed in embedded LLVM IR"))
    return fail("Embedded IR indexing must reject embedded NUL bytes");
  return 0;
}

} // namespace

int main() {
  if (const int Result = testReferenceIndex(); Result != 0)
    return Result;
  if (const int Result = testDeclarationDefinitionMerge(); Result != 0)
    return Result;
  return testDiagnostics();
}
