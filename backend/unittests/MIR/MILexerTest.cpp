// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/MILexer.h"

#include "rcc/Support/TextDiagnostic.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

using rcc::mir::MITokenKind;

struct ExpectedToken final {
  MITokenKind Kind;
  std::string_view Spelling;
  std::optional<std::string_view> StringValue;
  std::optional<std::string_view> IntegerValue;
};

void expectToken(TestContext &Context, rcc::mir::MILexer &Lexer, const ExpectedToken &Expected) {
  const rcc::mir::MIToken Token = Lexer.next();
  Context.expect(Token.kind() == Expected.Kind && Token.spelling() == Expected.Spelling,
                 "MI token kind and spelling must match the fixed grammar");
  Context.expect(Token.hasStringValue() == Expected.StringValue.has_value() &&
                     (!Expected.StringValue || Token.stringValue() == *Expected.StringValue),
                 "MI token string payload must be exact");
  Context.expect(Token.hasIntegerValue() == Expected.IntegerValue.has_value() &&
                     (!Expected.IntegerValue || Token.integerValue() == *Expected.IntegerValue),
                 "MI token integer spelling must remain exact");
}

void expectEnd(TestContext &Context, rcc::mir::MILexer &Lexer) {
  const rcc::mir::MIToken Token = Lexer.next();
  Context.expect(Token.kind() == MITokenKind::EndOfFile && Token.spelling().empty(),
                 "MI lexer must terminate with an empty end-of-file token");
}

void testPunctuationAndTrivia(TestContext &Context) {
  rcc::mir::MILexer Lexer("punctuation.mir",
                          " \t; line comment\r\n/* first */ /* second\ncomment */ , = _ : :: . ! "
                          "( ) { } + - < >");

  const std::array Expected{
      ExpectedToken{MITokenKind::Newline, "\r\n", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Comma, ",", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Equal, "=", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Underscore, "_", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Colon, ":", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::DoubleColon, "::", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Dot, ".", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Exclaim, "!", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::LeftParen, "(", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::RightParen, ")", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::LeftBrace, "{", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::RightBrace, "}", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Plus, "+", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Minus, "-", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Less, "<", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::Greater, ">", std::nullopt, std::nullopt},
  };
  for (const ExpectedToken &Token : Expected)
    expectToken(Context, Lexer, Token);
  expectEnd(Context, Lexer);

  Context.expect(!Lexer.failed() && Lexer.diagnostics().empty() &&
                     Lexer.tokenCount() == Expected.size(),
                 "whitespace and both MIR comment forms must not create tokens");
}

void testKeywordCorpus(TestContext &Context) {
  // Compatibility corpus from llvm/lib/CodeGen/MIRParser/MILexer.cpp at
  // 2b40c303e79f64c5c7642032554eda026594d604.
  constexpr std::array Keywords{
      std::string_view{"implicit"},
      std::string_view{"implicit-def"},
      std::string_view{"def"},
      std::string_view{"dead"},
      std::string_view{"killed"},
      std::string_view{"undef"},
      std::string_view{"internal"},
      std::string_view{"early-clobber"},
      std::string_view{"debug-use"},
      std::string_view{"renamable"},
      std::string_view{"tied-def"},
      std::string_view{"frame-setup"},
      std::string_view{"frame-destroy"},
      std::string_view{"nnan"},
      std::string_view{"ninf"},
      std::string_view{"nsz"},
      std::string_view{"arcp"},
      std::string_view{"contract"},
      std::string_view{"afn"},
      std::string_view{"reassoc"},
      std::string_view{"nuw"},
      std::string_view{"nsw"},
      std::string_view{"nusw"},
      std::string_view{"exact"},
      std::string_view{"nneg"},
      std::string_view{"disjoint"},
      std::string_view{"samesign"},
      std::string_view{"inbounds"},
      std::string_view{"nofpexcept"},
      std::string_view{"unpredictable"},
      std::string_view{"debug-location"},
      std::string_view{"debug-instr-number"},
      std::string_view{"dbg-instr-ref"},
      std::string_view{"same_value"},
      std::string_view{"offset"},
      std::string_view{"rel_offset"},
      std::string_view{"def_cfa_register"},
      std::string_view{"def_cfa_offset"},
      std::string_view{"adjust_cfa_offset"},
      std::string_view{"escape"},
      std::string_view{"def_cfa"},
      std::string_view{"llvm_def_aspace_cfa"},
      std::string_view{"remember_state"},
      std::string_view{"restore"},
      std::string_view{"restore_state"},
      std::string_view{"undefined"},
      std::string_view{"register"},
      std::string_view{"window_save"},
      std::string_view{"negate_ra_sign_state"},
      std::string_view{"negate_ra_sign_state_with_pc"},
      std::string_view{"llvm_register_pair"},
      std::string_view{"llvm_vector_registers"},
      std::string_view{"llvm_vector_offset"},
      std::string_view{"llvm_vector_register_mask"},
      std::string_view{"blockaddress"},
      std::string_view{"intrinsic"},
      std::string_view{"target-index"},
      std::string_view{"half"},
      std::string_view{"bfloat"},
      std::string_view{"float"},
      std::string_view{"double"},
      std::string_view{"x86_fp80"},
      std::string_view{"fp128"},
      std::string_view{"ppc_fp128"},
      std::string_view{"target-flags"},
      std::string_view{"volatile"},
      std::string_view{"non-temporal"},
      std::string_view{"dereferenceable"},
      std::string_view{"invariant"},
      std::string_view{"align"},
      std::string_view{"basealign"},
      std::string_view{"addrspace"},
      std::string_view{"stack"},
      std::string_view{"got"},
      std::string_view{"jump-table"},
      std::string_view{"constant-pool"},
      std::string_view{"call-entry"},
      std::string_view{"custom"},
      std::string_view{"lanemask"},
      std::string_view{"liveout"},
      std::string_view{"landing-pad"},
      std::string_view{"inlineasm-br-indirect-target"},
      std::string_view{"ehscope-entry"},
      std::string_view{"ehfunclet-entry"},
      std::string_view{"liveins"},
      std::string_view{"successors"},
      std::string_view{"floatpred"},
      std::string_view{"intpred"},
      std::string_view{"shufflemask"},
      std::string_view{"pre-instr-symbol"},
      std::string_view{"post-instr-symbol"},
      std::string_view{"heap-alloc-marker"},
      std::string_view{"pcsections"},
      std::string_view{"cfi-type"},
      std::string_view{"deactivation-symbol"},
      std::string_view{"bbsections"},
      std::string_view{"bb_id"},
      std::string_view{"unknown-size"},
      std::string_view{"unknown-address"},
      std::string_view{"distinct"},
      std::string_view{"ir-block-address-taken"},
      std::string_view{"machine-block-address-taken"},
      std::string_view{"call-frame-size"},
      std::string_view{"noconvergent"},
      std::string_view{"mmra"},
  };

  std::string Source;
  for (const std::string_view Keyword : Keywords) {
    Source += Keyword;
    Source.push_back(' ');
  }
  Source += "TEST_OPCODE keyword-like";

  rcc::mir::MILexer Lexer("keywords.mir", std::move(Source));
  for (const std::string_view Keyword : Keywords) {
    const rcc::mir::MIToken Token = Lexer.next();
    Context.expect(Token.kind() == MITokenKind::Keyword && Token.stringValue() == Keyword &&
                       rcc::mir::isMIKeyword(Keyword),
                   "every fixed LLVM MIR keyword must be reserved");
  }
  expectToken(Context, Lexer,
              {MITokenKind::Identifier, "TEST_OPCODE", "TEST_OPCODE", std::nullopt});
  expectToken(Context, Lexer,
              {MITokenKind::Identifier, "keyword-like", "keyword-like", std::nullopt});
  expectEnd(Context, Lexer);
  Context.expect(!rcc::mir::isMIKeyword("Implicit") && !rcc::mir::isMIKeyword("unknown-keyword"),
                 "MIR keyword classification must remain case-sensitive");
}

void testMetadataKeywords(TestContext &Context) {
  constexpr std::array Keywords{
      std::string_view{"!tbaa"},       std::string_view{"!alias.scope"},
      std::string_view{"!noalias"},    std::string_view{"!noalias.addrspace"},
      std::string_view{"!range"},      std::string_view{"!DIExpression"},
      std::string_view{"!DILocation"},
  };
  std::string Source;
  for (const std::string_view Keyword : Keywords) {
    Source += Keyword;
    Source.push_back(' ');
  }
  Source += "! !12";

  rcc::mir::MILexer Lexer("metadata.mir", std::move(Source));
  for (const std::string_view Keyword : Keywords) {
    const rcc::mir::MIToken Token = Lexer.next();
    Context.expect(Token.kind() == MITokenKind::MetadataKeyword && Token.stringValue() == Keyword &&
                       rcc::mir::isMIMetadataKeyword(Keyword),
                   "every fixed named metadata token must be recognized");
  }
  expectToken(Context, Lexer, {MITokenKind::Exclaim, "!", std::nullopt, std::nullopt});
  expectToken(Context, Lexer, {MITokenKind::Exclaim, "!", std::nullopt, std::nullopt});
  expectToken(Context, Lexer, {MITokenKind::IntegerLiteral, "12", std::nullopt, "12"});
  expectEnd(Context, Lexer);
}

void testNamedEntities(TestContext &Context) {
  rcc::mir::MILexer Lexer("entities.mir",
                          R"(bb.12.entry %bb.13.exit %jump-table.2 %stack.3.slot )"
                          R"(%fixed-stack.4 %const.5 %subreg.low32 %ir-block.7 )"
                          R"(%ir-block."quoted\20block" %ir.8 %ir.named %12 %named-vreg $x0 )"
                          R"(@9 @global @"quoted\20global" &external &"quoted\20external" )"
                          R"(<mcsymbol local.label> <mcsymbol "quoted\20symbol"> `i32 7` )"
                          R"("plain\20string\\tail")");

  const std::array Expected{
      ExpectedToken{MITokenKind::MachineBasicBlockLabel, "bb.12.entry", "entry", "12"},
      ExpectedToken{MITokenKind::MachineBasicBlock, "%bb.13.exit", "exit", "13"},
      ExpectedToken{MITokenKind::JumpTableIndex, "%jump-table.2", std::nullopt, "2"},
      ExpectedToken{MITokenKind::StackObject, "%stack.3.slot", "slot", "3"},
      ExpectedToken{MITokenKind::FixedStackObject, "%fixed-stack.4", std::nullopt, "4"},
      ExpectedToken{MITokenKind::ConstantPoolItem, "%const.5", std::nullopt, "5"},
      ExpectedToken{MITokenKind::SubRegisterIndex, "%subreg.low32", "low32", std::nullopt},
      ExpectedToken{MITokenKind::IRBlock, "%ir-block.7", std::nullopt, "7"},
      ExpectedToken{MITokenKind::NamedIRBlock, R"(%ir-block."quoted\20block")", "quoted block",
                    std::nullopt},
      ExpectedToken{MITokenKind::IRValue, "%ir.8", std::nullopt, "8"},
      ExpectedToken{MITokenKind::NamedIRValue, "%ir.named", "named", std::nullopt},
      ExpectedToken{MITokenKind::VirtualRegister, "%12", std::nullopt, "12"},
      ExpectedToken{MITokenKind::NamedVirtualRegister, "%named-vreg", "named-vreg", std::nullopt},
      ExpectedToken{MITokenKind::NamedRegister, "$x0", "x0", std::nullopt},
      ExpectedToken{MITokenKind::GlobalValue, "@9", std::nullopt, "9"},
      ExpectedToken{MITokenKind::NamedGlobalValue, "@global", "global", std::nullopt},
      ExpectedToken{MITokenKind::NamedGlobalValue, R"(@"quoted\20global")", "quoted global",
                    std::nullopt},
      ExpectedToken{MITokenKind::ExternalSymbol, "&external", "external", std::nullopt},
      ExpectedToken{MITokenKind::ExternalSymbol, R"(&"quoted\20external")", "quoted external",
                    std::nullopt},
      ExpectedToken{MITokenKind::MCSymbol, "<mcsymbol local.label>", "local.label", std::nullopt},
      ExpectedToken{MITokenKind::MCSymbol, R"(<mcsymbol "quoted\20symbol">)", "quoted symbol",
                    std::nullopt},
      ExpectedToken{MITokenKind::QuotedIRValue, "`i32 7`", "i32 7", std::nullopt},
      ExpectedToken{MITokenKind::StringConstant, R"("plain\20string\\tail")",
                    R"(plain string\tail)", std::nullopt},
  };
  for (const ExpectedToken &Token : Expected)
    expectToken(Context, Lexer, Token);
  expectEnd(Context, Lexer);
  Context.expect(!Lexer.failed() && Lexer.diagnostics().empty(),
                 "all fixed named MIR entity forms must lex without diagnostics");
}

void testNumericalLiterals(TestContext &Context) {
  rcc::mir::MILexer Lexer("numbers.mir", "-42 17 1. 1.25 -2.5e-3 6.02E23 0xFF 0XK3FFF000000000000 "
                                         "f0x3F800000 1e3 0x f0x");

  const std::array Expected{
      ExpectedToken{MITokenKind::IntegerLiteral, "-42", std::nullopt, "-42"},
      ExpectedToken{MITokenKind::IntegerLiteral, "17", std::nullopt, "17"},
      ExpectedToken{MITokenKind::FloatingPointLiteral, "1.", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::FloatingPointLiteral, "1.25", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::FloatingPointLiteral, "-2.5e-3", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::FloatingPointLiteral, "6.02E23", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::HexLiteral, "0xFF", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::FloatingPointLiteral, "0XK3FFF000000000000", std::nullopt,
                    std::nullopt},
      ExpectedToken{MITokenKind::FloatingPointLiteral, "f0x3F800000", std::nullopt, std::nullopt},
      ExpectedToken{MITokenKind::IntegerLiteral, "1", std::nullopt, "1"},
      ExpectedToken{MITokenKind::Identifier, "e3", "e3", std::nullopt},
      ExpectedToken{MITokenKind::IntegerLiteral, "0", std::nullopt, "0"},
      ExpectedToken{MITokenKind::Identifier, "x", "x", std::nullopt},
      ExpectedToken{MITokenKind::Identifier, "f0x", "f0x", std::nullopt},
  };
  for (const ExpectedToken &Token : Expected)
    expectToken(Context, Lexer, Token);
  expectEnd(Context, Lexer);

  std::string LargeNumber(10000, '9');
  rcc::mir::MILexer LargeLexer("large-number.mir", LargeNumber);
  const rcc::mir::MIToken LargeToken = LargeLexer.next();
  Context.expect(LargeToken.kind() == MITokenKind::IntegerLiteral &&
                     LargeToken.integerValue() == LargeNumber,
                 "integer tokenization must preserve arbitrary-width decimal spelling");
}

void expectSingleError(TestContext &Context, std::string Source, std::string_view MessageFragment,
                       rcc::mir::MILexerOptions Options = {}) {
  rcc::mir::MILexer Lexer("invalid.mir", std::move(Source), Options);
  const rcc::mir::MIToken Token = Lexer.next();
  Context.expect(Token.kind() == MITokenKind::Error && Lexer.failed() &&
                     Lexer.diagnostics().size() == 1 &&
                     Lexer.diagnostics().front().Category == "mir-lexer" &&
                     Lexer.diagnostics().front().Message.find(MessageFragment) != std::string::npos,
                 "invalid MI input must produce one structured lexer diagnostic");
  expectEnd(Context, Lexer);
  Context.expect(Lexer.diagnostics().size() == 1,
                 "reading after an MI lexer failure must not duplicate diagnostics");
}

void testDiagnosticsAndLimits(TestContext &Context) {
  expectSingleError(Context, "/* never closed", "closing '*/'");
  expectSingleError(Context, R"("never closed)", "closing '\"'");
  expectSingleError(Context, "`never closed", "closing '`'");
  expectSingleError(Context, "<mcsymbol local", "closed by '>'");
  expectSingleError(Context, "!unknown", "unknown metadata keyword");
  expectSingleError(Context, "bb.name", "decimal block number");
  expectSingleError(Context, "#", "unexpected character '#'");
  expectSingleError(Context, std::string(1, '\0'), "unexpected byte 0x00");
  expectSingleError(Context, "four", "length limit",
                    rcc::mir::MILexerOptions{.MaximumTokenLength = 3});

  rcc::mir::MILexer Limited("limited.mir", "one two",
                            rcc::mir::MILexerOptions{.MaximumTokenLength = 0, .MaximumTokens = 1});
  expectToken(Context, Limited, {MITokenKind::Identifier, "one", "one", std::nullopt});
  const rcc::mir::MIToken LimitError = Limited.next();
  Context.expect(LimitError.kind() == MITokenKind::Error && Limited.diagnostics().size() == 1 &&
                     Limited.diagnostics().front().Message.find("token count") !=
                         std::string::npos &&
                     Limited.tokenCount() == 1,
                 "MI lexer token count limits must fail before publishing excess tokens");
}

void testRangesAndClassifiers(TestContext &Context) {
  rcc::mir::MILexer Lexer("ranges.mir", "implicit\n  $x0 \"flag\"");
  const rcc::mir::MIToken Flag = Lexer.next();
  const rcc::mir::MIToken Newline = Lexer.next();
  const rcc::mir::MIToken Register = Lexer.next();
  const rcc::mir::MIToken StringFlag = Lexer.next();

  Context.expect(Flag.isRegisterFlag() && !Flag.isRegister() && Flag.range().Begin.Line == 1 &&
                     Flag.range().Begin.Column == 1 && Flag.range().End.Column == 9,
                 "register flags must retain exact source ranges");
  Context.expect(Newline.kind() == MITokenKind::Newline && Register.isRegister() &&
                     Register.stringValue() == "x0" && Register.range().Begin.Line == 2 &&
                     Register.range().Begin.Column == 3,
                 "line and column tracking must account for skipped indentation");
  Context.expect(StringFlag.isMemoryOperandFlag() &&
                     rcc::mir::getMITokenKindName(StringFlag.kind()) == "string-constant",
                 "memory operand and token-kind classifiers must cover string flags");

  rcc::mir::MILexer Invalid("body.mir", "ok\n  #");
  static_cast<void>(Invalid.next());
  static_cast<void>(Invalid.next());
  static_cast<void>(Invalid.next());
  const std::string Formatted =
      rcc::formatTextDiagnostic(Invalid.diagnostics().front(), Invalid.source());
  Context.expect(Formatted.starts_with("body.mir:2:3: error[mir-lexer]: unexpected character '#'"),
                 "formatted MI diagnostics must report stable input line and column");

  rcc::mir::MILexer Original("move.mir", R"("owned\20value")");
  rcc::mir::MILexer Moved(std::move(Original));
  const rcc::mir::MIToken Owned = Moved.next();
  Context.expect(Owned.stringValue() == "owned value",
                 "moving an MI lexer must preserve owned decoded token values");
}

} // namespace

int main() {
  TestContext Context;
  testPunctuationAndTrivia(Context);
  testKeywordCorpus(Context);
  testMetadataKeywords(Context);
  testNamedEntities(Context);
  testNumericalLiterals(Context);
  testDiagnosticsAndLimits(Context);
  testRangesAndClassifiers(Context);
  return Context.result();
}
