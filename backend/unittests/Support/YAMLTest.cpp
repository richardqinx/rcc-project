// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Support/YAML.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int fail(const char *Message) {
  std::cerr << Message << '\n';
  return 1;
}

const rcc::yaml::Node *mappingValue(const rcc::yaml::Stream &Stream, const rcc::yaml::Node &Mapping,
                                    std::string_view Key) {
  const std::optional<rcc::yaml::NodeID> Value = Mapping.mappingValue(Key);
  return Value ? Stream.node(*Value) : nullptr;
}

int testMIRShape() {
  const std::string Input =
      "...\n"
      "--- |\n"
      "  define void @f() {\n"
      "  entry:\n"
      "    ret void\n"
      "  }\n"
      "---\n"
      "name: f\n"
      "alignment: 16\n"
      "registers:\n"
      "  - { id: 0, class: gpr, flags: [foo, 'bar',], preferred-register:, }\n"
      "frameInfo:\n"
      "  hasCalls: true\n"
      "jumpTable:\n"
      "  blocks: [ '%bb.0',\n"
      "            '%bb.1', ]\n"
      "callSites:\n"
      "  - { bb: 0, offset: 4, fwdArgRegs:\n"
      "      - { arg: 0, reg: '$x1' }\n"
      "      - { arg: 1, reg: '$x2' } }\n"
      "body: |-\n"
      "  bb.0:\n"
      "    RET\n"
      "...\n";

  rcc::yaml::ParseResult Result = rcc::yaml::parse("input.mir", Input);
  const rcc::yaml::Stream *ParsedStream = Result.stream();
  if (!Result.succeeded() || ParsedStream == nullptr)
    return fail("YAML parser must accept the LLVM MIR document shape");

  const rcc::yaml::Stream &Stream = *ParsedStream;
  if (Stream.documents().size() != 2)
    return fail("YAML parser must retain every document in input order");

  const rcc::yaml::Node *IR = Stream.node(Stream.documents()[0].Root);
  if (IR == nullptr || IR->kind() != rcc::yaml::NodeKind::Scalar ||
      IR->scalarStyle() != rcc::yaml::ScalarStyle::Literal ||
      IR->scalar() != "define void @f() {\nentry:\n  ret void\n}\n")
    return fail("YAML parser must decode the embedded LLVM IR block scalar");

  const rcc::yaml::Node *Function = Stream.node(Stream.documents()[1].Root);
  if (Function == nullptr || Function->kind() != rcc::yaml::NodeKind::Mapping)
    return fail("YAML parser must represent a MachineFunction document as a mapping");

  const rcc::yaml::Node *Name = mappingValue(Stream, *Function, "name");
  const rcc::yaml::Node *Registers = mappingValue(Stream, *Function, "registers");
  const rcc::yaml::Node *FrameInfo = mappingValue(Stream, *Function, "frameInfo");
  const rcc::yaml::Node *JumpTable = mappingValue(Stream, *Function, "jumpTable");
  const rcc::yaml::Node *CallSites = mappingValue(Stream, *Function, "callSites");
  const rcc::yaml::Node *Body = mappingValue(Stream, *Function, "body");
  if (Name == nullptr || Name->scalar() != "f")
    return fail("YAML mapping lookup must preserve scalar values");
  if (Registers == nullptr || Registers->sequence().size() != 1)
    return fail("YAML parser must decode block and flow collections together");
  if (FrameInfo == nullptr || FrameInfo->kind() != rcc::yaml::NodeKind::Mapping)
    return fail("YAML parser must decode nested block mappings");
  const rcc::yaml::Node *Blocks =
      JumpTable == nullptr ? nullptr : mappingValue(Stream, *JumpTable, "blocks");
  if (Blocks == nullptr || Blocks->sequence().size() != 2)
    return fail("YAML parser must decode line-wrapped flow sequences");
  const rcc::yaml::Node *CallSite =
      CallSites == nullptr ? nullptr : Stream.node(CallSites->sequence()[0]);
  const rcc::yaml::Node *Forwarded =
      CallSite == nullptr ? nullptr : mappingValue(Stream, *CallSite, "fwdArgRegs");
  if (Forwarded == nullptr || Forwarded->sequence().size() != 2)
    return fail("YAML parser must decode block sequences nested in line-wrapped flow mappings");
  if (Body == nullptr || Body->scalar() != "bb.0:\n  RET")
    return fail("YAML block scalar strip chomping must remove trailing line breaks");

  const rcc::yaml::Node *Register = Stream.node(Registers->sequence()[0]);
  const rcc::yaml::Node *Flags =
      Register == nullptr ? nullptr : mappingValue(Stream, *Register, "flags");
  if (Register == nullptr || Register->collectionStyle() != rcc::yaml::CollectionStyle::Flow ||
      Flags == nullptr || Flags->sequence().size() != 2)
    return fail("YAML flow mapping and sequence structure must be retained");
  return 0;
}

int testScalarRules() {
  const std::string Input = "plain: value # comment\n"
                            "single: 'it''s YAML'\n"
                            "double: \"line\\n\\u03bb\"\n"
                            "nothing: null\n"
                            "literal: |-\n"
                            "  \tcontent\n"
                            "hash: |-\n"
                            "  # scalar content\n"
                            "folded: >-\n"
                            "  one\n"
                            "  two\n";
  rcc::yaml::ParseResult Result = rcc::yaml::parse("scalars.yaml", Input);
  const rcc::yaml::Stream *ParsedStream = Result.stream();
  if (!Result.succeeded() || ParsedStream == nullptr)
    return fail("YAML parser must accept supported scalar forms");

  const rcc::yaml::Stream &Stream = *ParsedStream;
  const rcc::yaml::Node *Root = Stream.node(Stream.documents()[0].Root);
  if (Root == nullptr)
    return fail("YAML scalar test must have a root");
  const rcc::yaml::Node *Single = mappingValue(Stream, *Root, "single");
  const rcc::yaml::Node *Double = mappingValue(Stream, *Root, "double");
  const rcc::yaml::Node *Nothing = mappingValue(Stream, *Root, "nothing");
  const rcc::yaml::Node *Literal = mappingValue(Stream, *Root, "literal");
  const rcc::yaml::Node *Hash = mappingValue(Stream, *Root, "hash");
  const rcc::yaml::Node *Folded = mappingValue(Stream, *Root, "folded");
  if (Single == nullptr || Single->scalar() != "it's YAML")
    return fail("YAML single quotes must decode doubled quotes");
  if (Double == nullptr || Double->scalar() != "line\n\xCE\xBB")
    return fail("YAML double quotes must decode escapes and Unicode");
  if (Nothing == nullptr || Nothing->kind() != rcc::yaml::NodeKind::Null)
    return fail("YAML null spellings must produce null nodes");
  if (Literal == nullptr || Literal->scalar() != "\tcontent")
    return fail("YAML block scalars must preserve tab characters in scalar content");
  if (Hash == nullptr || Hash->scalar() != "# scalar content")
    return fail("YAML block scalar content must not interpret '#' as a comment");
  if (Folded == nullptr || Folded->scalar() != "one two")
    return fail("YAML folded scalars must fold ordinary line breaks");
  return 0;
}

int testNestedSequenceMapping() {
  const std::string Input = "savePoint:\n"
                            "  - point: '%bb.1'\n"
                            "    registers:\n"
                            "      - '$x1'\n"
                            "      - '$x2'\n";
  rcc::yaml::ParseResult Result = rcc::yaml::parse("nested.yaml", Input);
  const rcc::yaml::Stream *ParsedStream = Result.stream();
  if (!Result.succeeded() || ParsedStream == nullptr)
    return fail("YAML parser must accept sequence entries that start block mappings");

  const rcc::yaml::Stream &Stream = *ParsedStream;
  const rcc::yaml::Node *Root = Stream.node(Stream.documents()[0].Root);
  const rcc::yaml::Node *SavePoints =
      Root == nullptr ? nullptr : mappingValue(Stream, *Root, "savePoint");
  const rcc::yaml::Node *SavePoint =
      SavePoints == nullptr ? nullptr : Stream.node(SavePoints->sequence()[0]);
  const rcc::yaml::Node *Registers =
      SavePoint == nullptr ? nullptr : mappingValue(Stream, *SavePoint, "registers");
  if (Registers == nullptr || Registers->sequence().size() != 2)
    return fail("YAML nested sequence mappings must retain continuation keys");
  return 0;
}

int testDiagnostics() {
  rcc::yaml::ParseResult Duplicate = rcc::yaml::parse("duplicate.yaml", "name: a\nname: b\n");
  if (Duplicate.succeeded() || Duplicate.diagnostics().size() != 1 ||
      Duplicate.diagnostics()[0].Message != "duplicate YAML mapping key 'name'" ||
      Duplicate.diagnostics()[0].Range.Begin.Line != 2)
    return fail("YAML parser must reject duplicate keys at the second definition");

  rcc::yaml::ParseResult BadIndent = rcc::yaml::parse("tab.yaml", "root:\n\tchild: value\n");
  if (BadIndent.succeeded() || BadIndent.diagnostics().empty() ||
      BadIndent.diagnostics()[0].Message != "tab characters are not allowed in YAML indentation")
    return fail("YAML parser must reject tab indentation");

  rcc::yaml::ParseResult BadFlow = rcc::yaml::parse("flow.yaml", "values: [one two\n");
  if (BadFlow.succeeded() || BadFlow.diagnostics().empty() ||
      BadFlow.diagnostics()[0].Message != "unterminated YAML flow sequence")
    return fail("YAML parser must diagnose unterminated flow collections");

  rcc::yaml::ParseResult BadEnd = rcc::yaml::parse("end.yaml", "---\nname: f\n... garbage\n");
  if (BadEnd.succeeded() || BadEnd.diagnostics().empty() ||
      BadEnd.diagnostics()[0].Message != "document end marker may only be followed by a comment")
    return fail("YAML parser must reject content after a document end marker");

  rcc::yaml::ParseResult Directive = rcc::yaml::parse("directive.yaml", "%YAML 1.2\n---\na\n");
  if (Directive.succeeded() || Directive.diagnostics().empty() ||
      Directive.diagnostics()[0].Message != "YAML directives are not supported")
    return fail("YAML parser must explicitly reject unsupported directives");

  rcc::yaml::ParseResult Alias = rcc::yaml::parse("alias.yaml", "value: *shared\n");
  if (Alias.succeeded() || Alias.diagnostics().empty() ||
      Alias.diagnostics()[0].Message !=
          "YAML anchors, aliases, tags, and directives are not supported")
    return fail("YAML parser must explicitly reject unsupported aliases");

  const std::string NulInput("name:\0f", 7);
  rcc::yaml::ParseResult Nul = rcc::yaml::parse("nul.yaml", NulInput);
  if (Nul.succeeded() || Nul.diagnostics().empty() ||
      Nul.diagnostics()[0].Message != "NUL bytes are not allowed in YAML input")
    return fail("YAML parser must reject embedded NUL bytes");

  rcc::yaml::ParseOptions Limits;
  Limits.MaximumDepth = 2;
  rcc::yaml::ParseResult Deep = rcc::yaml::parse("deep.yaml", "a:\n  b:\n    c: value\n", Limits);
  if (Deep.succeeded() || Deep.diagnostics().empty() ||
      Deep.diagnostics()[0].Message != "maximum YAML nesting depth exceeded")
    return fail("YAML parser must enforce its nesting limit");

  Limits.MaximumDepth = 128;
  Limits.MaximumNodes = 1;
  rcc::yaml::ParseResult ManyNodes = rcc::yaml::parse("nodes.yaml", "name: f\n", Limits);
  if (ManyNodes.succeeded() || ManyNodes.diagnostics().empty() ||
      ManyNodes.diagnostics()[0].Message != "maximum YAML node count exceeded")
    return fail("YAML parser must enforce its node limit");
  return 0;
}

} // namespace

int main() {
  if (const int Result = testMIRShape(); Result != 0)
    return Result;
  if (const int Result = testScalarRules(); Result != 0)
    return Result;
  if (const int Result = testNestedSequenceMapping(); Result != 0)
    return Result;
  return testDiagnostics();
}
