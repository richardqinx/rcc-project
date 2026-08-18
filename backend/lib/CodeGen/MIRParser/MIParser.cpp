// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MIRParser/MIParser.h"

#include "MILexer.h"
#include "rcc/AsmParser/LLParser.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/InlineAsm.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MIRPrinter.h"
#include "rcc/CodeGen/MIRYamlMapping.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc::mir {
namespace {

[[nodiscard]] bool parseUnsignedDecimal(std::string_view Spelling, std::uint64_t &Value) {
  if (Spelling.empty() || Spelling.front() == '-')
    return false;
  const char *Begin = Spelling.data();
  const char *End = Begin + Spelling.size();
  const auto Result = std::from_chars(Begin, End, Value, 10);
  return Result.ec == std::errc{} && Result.ptr == End;
}

[[nodiscard]] bool parseSignedDecimal(std::string_view Spelling, std::int64_t &Value) {
  if (Spelling.empty())
    return false;
  const char *Begin = Spelling.data();
  const char *End = Begin + Spelling.size();
  const auto Result = std::from_chars(Begin, End, Value, 10);
  return Result.ec == std::errc{} && Result.ptr == End;
}

[[nodiscard]] bool parseHexadecimal(std::string_view Spelling, std::uint64_t &Value) {
  if (Spelling.size() < 3 || Spelling[0] != '0' || (Spelling[1] != 'x' && Spelling[1] != 'X'))
    return false;
  const char *Begin = Spelling.data() + 2;
  const char *End = Spelling.data() + Spelling.size();
  const auto Result = std::from_chars(Begin, End, Value, 16);
  return Result.ec == std::errc{} && Result.ptr == End;
}

[[nodiscard]] bool parseUnsignedDigits(std::string_view Spelling, std::uint64_t &Value) {
  if (Spelling.empty())
    return false;
  const char *Begin = Spelling.data();
  const char *End = Begin + Spelling.size();
  const auto Result = std::from_chars(Begin, End, Value, 10);
  return Result.ec == std::errc{} && Result.ptr == End;
}

[[nodiscard]] std::optional<FloatSemantics> floatSemanticsForKeyword(std::string_view Keyword) {
  if (Keyword == "half")
    return FloatSemantics::IEEEHalf;
  if (Keyword == "bfloat")
    return FloatSemantics::BFloat;
  if (Keyword == "float")
    return FloatSemantics::IEEESingle;
  if (Keyword == "double")
    return FloatSemantics::IEEEDouble;
  if (Keyword == "x86_fp80")
    return FloatSemantics::X87DoubleExtended;
  if (Keyword == "fp128")
    return FloatSemantics::IEEEQuad;
  if (Keyword == "ppc_fp128")
    return FloatSemantics::PPCDoubleDouble;
  return std::nullopt;
}

[[nodiscard]] bool isFloatingSemanticsKeyword(const MIToken &Token) {
  return Token.kind() == MITokenKind::Keyword &&
         floatSemanticsForKeyword(Token.stringValue()).has_value();
}

[[nodiscard]] bool isCFIOperationToken(const MIToken &Token) {
  return Token.isKeyword("same_value") || Token.isKeyword("remember_state") ||
         Token.isKeyword("restore_state") || Token.isKeyword("offset") ||
         Token.isKeyword("llvm_def_aspace_cfa") || Token.isKeyword("def_cfa_register") ||
         Token.isKeyword("def_cfa_offset") || Token.isKeyword("def_cfa") ||
         Token.isKeyword("rel_offset") || Token.isKeyword("adjust_cfa_offset") ||
         Token.isKeyword("escape") || Token.isKeyword("restore") || Token.isKeyword("undefined") ||
         Token.isKeyword("register") || Token.isKeyword("window_save") ||
         Token.isKeyword("negate_ra_sign_state") ||
         Token.isKeyword("negate_ra_sign_state_with_pc") || Token.isKeyword("llvm_register_pair") ||
         Token.isKeyword("llvm_vector_registers") || Token.isKeyword("llvm_vector_offset") ||
         Token.isKeyword("llvm_vector_register_mask");
}

[[nodiscard]] Expected<APInt> parseIntegerImmediateValue(std::uint32_t BitWidth,
                                                         std::string_view Spelling) {
  if (BitWidth == 0 || BitWidth > APInt::MaxBitWidth)
    return makeUnexpected(ErrorCode::ParseError, "typed integer width is out of range");

  const bool Negative = Spelling.starts_with('-');
  if (Negative)
    Spelling.remove_prefix(1);
  if (Spelling.empty())
    return makeUnexpected(ErrorCode::ParseError, "typed integer has no digits");

  std::vector<std::uint64_t> Words((static_cast<std::size_t>(BitWidth) + 63U) / 64U, 0);
  for (char Character : Spelling) {
    if (Character < '0' || Character > '9')
      return makeUnexpected(ErrorCode::ParseError, "typed integer contains a non-decimal digit");
    std::uint64_t Carry = static_cast<unsigned>(Character - '0');
    for (std::uint64_t &Word : Words) {
      const unsigned __int128 Product = static_cast<unsigned __int128>(Word) * 10U + Carry;
      Word = static_cast<std::uint64_t>(Product);
      Carry = static_cast<std::uint64_t>(Product >> 64U);
    }
  }

  // LLVM parses the arbitrary-precision literal first and then truncates it to
  // the declared integer width.  Keep exactly that modulo-2^N bit pattern.
  if (BitWidth % 64U != 0)
    Words.back() &= (std::uint64_t{1} << (BitWidth % 64U)) - 1U;
  if (Negative) {
    for (std::uint64_t &Word : Words)
      Word = ~Word;
    std::uint64_t Carry = 1;
    for (std::uint64_t &Word : Words) {
      const std::uint64_t Previous = Word;
      Word += Carry;
      Carry = Carry != 0 && Word < Previous ? 1 : 0;
    }
    if (BitWidth % 64U != 0)
      Words.back() &= (std::uint64_t{1} << (BitWidth % 64U)) - 1U;
  }
  return APInt::create(BitWidth, Words);
}

[[nodiscard]] std::optional<std::uint64_t> minimumTypeSizeInBits(LowLevelType Type) noexcept {
  if (!Type.isValid() || Type.isToken())
    return std::nullopt;
  const std::uint64_t ScalarSize = Type.scalarBitWidth();
  if (!Type.isVector())
    return ScalarSize;
  const std::uint64_t ElementCount = Type.minimumElementCount();
  if (ScalarSize != 0 && ElementCount > std::numeric_limits<std::uint64_t>::max() / ScalarSize)
    return std::nullopt;
  return ScalarSize * ElementCount;
}

[[nodiscard]] std::optional<AtomicOrdering> atomicOrdering(std::string_view Name) noexcept {
  if (Name == "unordered")
    return AtomicOrdering::Unordered;
  if (Name == "monotonic")
    return AtomicOrdering::Monotonic;
  if (Name == "acquire")
    return AtomicOrdering::Acquire;
  if (Name == "release")
    return AtomicOrdering::Release;
  if (Name == "acq_rel")
    return AtomicOrdering::AcquireRelease;
  if (Name == "seq_cst")
    return AtomicOrdering::SequentiallyConsistent;
  return std::nullopt;
}

[[nodiscard]] bool isInstructionFlag(const MIToken &Token) {
  return Token.isKeyword("frame-setup") || Token.isKeyword("frame-destroy") ||
         Token.isKeyword("nnan") || Token.isKeyword("ninf") || Token.isKeyword("nsz") ||
         Token.isKeyword("arcp") || Token.isKeyword("contract") || Token.isKeyword("afn") ||
         Token.isKeyword("reassoc") || Token.isKeyword("nuw") || Token.isKeyword("nsw") ||
         Token.isKeyword("nusw") || Token.isKeyword("exact") || Token.isKeyword("nofpexcept") ||
         Token.isKeyword("unpredictable") || Token.isKeyword("noconvergent") ||
         Token.isKeyword("nneg") || Token.isKeyword("disjoint") || Token.isKeyword("samesign") ||
         Token.isKeyword("inbounds");
}

[[nodiscard]] std::optional<MachineInstrFlag> instructionFlag(std::string_view Name) {
  if (Name == "frame-setup")
    return MachineInstrFlag::FrameSetup;
  if (Name == "frame-destroy")
    return MachineInstrFlag::FrameDestroy;
  if (Name == "nnan")
    return MachineInstrFlag::FastMathNoNaNs;
  if (Name == "ninf")
    return MachineInstrFlag::FastMathNoInfs;
  if (Name == "nsz")
    return MachineInstrFlag::FastMathNoSignedZeros;
  if (Name == "arcp")
    return MachineInstrFlag::FastMathAllowReciprocal;
  if (Name == "contract")
    return MachineInstrFlag::FastMathAllowContract;
  if (Name == "afn")
    return MachineInstrFlag::FastMathApproxFunc;
  if (Name == "reassoc")
    return MachineInstrFlag::FastMathAllowReassoc;
  if (Name == "nuw")
    return MachineInstrFlag::NoUnsignedWrap;
  if (Name == "nsw")
    return MachineInstrFlag::NoSignedWrap;
  if (Name == "nusw")
    return MachineInstrFlag::NoUSWrap;
  if (Name == "exact")
    return MachineInstrFlag::Exact;
  if (Name == "nofpexcept")
    return MachineInstrFlag::NoFPExcept;
  if (Name == "unpredictable")
    return MachineInstrFlag::Unpredictable;
  if (Name == "noconvergent")
    return MachineInstrFlag::NoConvergent;
  if (Name == "nneg")
    return MachineInstrFlag::NonNeg;
  if (Name == "disjoint")
    return MachineInstrFlag::Disjoint;
  if (Name == "samesign")
    return MachineInstrFlag::SameSign;
  if (Name == "inbounds")
    return MachineInstrFlag::InBounds;
  return std::nullopt;
}

[[nodiscard]] std::optional<IntegerPredicate> integerPredicate(std::string_view Name) {
  if (Name == "eq")
    return IntegerPredicate::Equal;
  if (Name == "ne")
    return IntegerPredicate::NotEqual;
  if (Name == "sgt")
    return IntegerPredicate::SignedGreaterThan;
  if (Name == "sge")
    return IntegerPredicate::SignedGreaterEqual;
  if (Name == "slt")
    return IntegerPredicate::SignedLessThan;
  if (Name == "sle")
    return IntegerPredicate::SignedLessEqual;
  if (Name == "ugt")
    return IntegerPredicate::UnsignedGreaterThan;
  if (Name == "uge")
    return IntegerPredicate::UnsignedGreaterEqual;
  if (Name == "ult")
    return IntegerPredicate::UnsignedLessThan;
  if (Name == "ule")
    return IntegerPredicate::UnsignedLessEqual;
  return std::nullopt;
}

[[nodiscard]] std::optional<FloatingPredicate> floatingPredicate(std::string_view Name) {
  if (Name == "false")
    return FloatingPredicate::AlwaysFalse;
  if (Name == "oeq")
    return FloatingPredicate::OrderedEqual;
  if (Name == "ogt")
    return FloatingPredicate::OrderedGreaterThan;
  if (Name == "oge")
    return FloatingPredicate::OrderedGreaterEqual;
  if (Name == "olt")
    return FloatingPredicate::OrderedLessThan;
  if (Name == "ole")
    return FloatingPredicate::OrderedLessEqual;
  if (Name == "one")
    return FloatingPredicate::OrderedNotEqual;
  if (Name == "ord")
    return FloatingPredicate::Ordered;
  if (Name == "uno")
    return FloatingPredicate::Unordered;
  if (Name == "ueq")
    return FloatingPredicate::UnorderedEqual;
  if (Name == "ugt")
    return FloatingPredicate::UnorderedGreaterThan;
  if (Name == "uge")
    return FloatingPredicate::UnorderedGreaterEqual;
  if (Name == "ult")
    return FloatingPredicate::UnorderedLessThan;
  if (Name == "ule")
    return FloatingPredicate::UnorderedLessEqual;
  if (Name == "une")
    return FloatingPredicate::UnorderedNotEqual;
  if (Name == "true")
    return FloatingPredicate::AlwaysTrue;
  return std::nullopt;
}

} // namespace

class MIParser::Impl final {
public:
  Impl(MachineFunction &Function, const TargetDescription &Target, std::string SourceName,
       std::string SourceText, MIParserContext Context, MIParserOptions Options)
      : Function(Function), Target(Target),
        Lexer(std::move(SourceName), std::move(SourceText),
              MILexerOptions{.MaximumTokenLength = Options.MaximumTokenLength,
                             .MaximumTokens = Options.MaximumTokens}),
        Context(std::move(Context)), Options(Options) {}

  [[nodiscard]] Expected<void> parse();

  [[nodiscard]] const TextSource &source() const noexcept {
    return Lexer.source();
  }

  [[nodiscard]] std::span<const TextDiagnostic> diagnostics() const noexcept {
    return Diagnostics;
  }

  [[nodiscard]] bool failed() const noexcept {
    return !Diagnostics.empty();
  }

private:
  struct ParsedOperand final {
    MachineOperand Operand;
    std::optional<std::size_t> TiedDef;
    TextRange Range;
  };

  [[nodiscard]] const MIToken &token() const noexcept {
    return Tokens[Position];
  }

  [[nodiscard]] bool at(MITokenKind Kind) const noexcept {
    return token().is(Kind);
  }

  [[nodiscard]] bool nextIs(MITokenKind Kind) const noexcept {
    return Position + 1U < Tokens.size() && Tokens[Position + 1U].is(Kind);
  }

  [[nodiscard]] bool atKeyword(std::string_view Keyword) const noexcept {
    return token().isKeyword(Keyword);
  }

  void advance() noexcept {
    if (!at(MITokenKind::EndOfFile))
      ++Position;
  }

  [[nodiscard]] bool consume(MITokenKind Kind) noexcept {
    if (!at(Kind))
      return false;
    advance();
    return true;
  }

  [[nodiscard]] bool expect(MITokenKind Kind, std::string_view Description);
  [[nodiscard]] bool fail(const MIToken &At, std::string Message);
  [[nodiscard]] bool fail(std::string Message) {
    return fail(token(), std::move(Message));
  }
  [[nodiscard]] Expected<void> failureResult() const;
  [[nodiscard]] bool importError(const MIToken &At, const Error &Failure);
  [[nodiscard]] bool tokenize();

  [[nodiscard]] bool parseUnsigned(const MIToken &Token, std::uint64_t &Value);
  [[nodiscard]] bool parseUnsigned32(const MIToken &Token, std::uint32_t &Value);
  [[nodiscard]] bool parseSigned64(const MIToken &Token, std::int64_t &Value);
  [[nodiscard]] bool parseOptionalOffset(std::int64_t &Offset);
  [[nodiscard]] bool parseAlignment(Alignment &Result);
  [[nodiscard]] bool parseLowLevelType(LowLevelType &Result);
  [[nodiscard]] bool parseScalarLowLevelType(const MIToken &Token, LowLevelType &Result);

  [[nodiscard]] bool parseFirstPass();
  [[nodiscard]] bool parseSecondPass();
  [[nodiscard]] bool parseBlockDefinition(bool Create, std::uint32_t &ID);
  [[nodiscard]] bool parseBlockAttributes(MachineBasicBlock *Block);
  [[nodiscard]] bool parseBlockLiveIns(MachineBasicBlock &Block);
  [[nodiscard]] bool parseBlockSuccessors(MachineBasicBlock &Block);
  [[nodiscard]] bool parseInstruction(MachineBasicBlock &Block, bool InBundle,
                                      MachineInstr *PreviousInstruction, MachineInstr *&Result);
  [[nodiscard]] bool inferSuccessors();

  [[nodiscard]] bool parseRegister(Register &Result);
  [[nodiscard]] bool parseRegisterOperand(bool IsDef, ParsedOperand &Result);
  [[nodiscard]] bool parseTargetFlags(std::uint32_t &Flags);
  [[nodiscard]] bool parseOperand(bool IsDef, ParsedOperand &Result);
  [[nodiscard]] bool parseInlineAsmExtraInfoOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseInlineAsmOperandFlag(ParsedOperand &Result);
  [[nodiscard]] bool parseTypedIntegerOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseFloatingImmediateOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseMetadataReference(MachineMetadataReference &Result);
  [[nodiscard]] bool parseMetadataOperand(ParsedOperand &Result);
  [[nodiscard]] bool
  parseInlineDILocation(std::vector<MachineDILocationFrame> &InlineChain,
                        std::optional<MachineMetadataReference> &TerminalInlinedAt);
  [[nodiscard]] bool parseCFIRegister(DwarfRegisterNumber &Result);
  [[nodiscard]] bool parseCFISigned(std::int64_t &Result);
  [[nodiscard]] bool parseCFIUnsigned(std::uint32_t &Result);
  [[nodiscard]] bool parseCFIOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseMemoryBase(MemoryBase &Result, std::int64_t &Offset);
  [[nodiscard]] bool parseMemoryOperand(const MachineMemOperand *&Result, Opcode InstructionOpcode);
  [[nodiscard]] bool parsePredicateOperand(ParsedOperand &Result, bool IsFloating);
  [[nodiscard]] bool parseShuffleMaskOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseDebugInstrRefOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseLaneMaskOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseIntrinsicOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseTargetIndexOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseLiveOutMaskOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseCustomMaskOperand(ParsedOperand &Result);
  [[nodiscard]] bool parseBlockAddressOperand(ParsedOperand &Result);

  [[nodiscard]] Register getOrCreateVirtualRegister(const MIToken &Token);
  [[nodiscard]] MachineBasicBlock *blockFor(const MIToken &Token);
  [[nodiscard]] std::optional<FrameIndex> frameIndexFor(const MIToken &Token, bool Fixed);
  [[nodiscard]] std::optional<ConstantPoolIndex> constantPoolIndexFor(const MIToken &Token);
  [[nodiscard]] std::optional<JumpTableIndex> jumpTableIndexFor(const MIToken &Token);
  [[nodiscard]] std::string symbolName(const MIToken &Token) const;

  MachineFunction &Function;
  const TargetDescription &Target;
  MILexer Lexer;
  MIParserContext Context;
  MIParserOptions Options;
  std::vector<MIToken> Tokens;
  std::size_t Position = 0;
  std::vector<TextDiagnostic> Diagnostics;
  std::unordered_map<std::uint32_t, MachineBasicBlock *> Blocks;
  std::unordered_map<std::uint32_t, Register> NumericVirtualRegisters;
  std::unordered_map<std::string, Register> NamedVirtualRegisters;
  std::unordered_set<MachineBasicBlock *> ExplicitSuccessorBlocks;
  std::size_t ParsedInstructionCount = 0;
  bool ParseAttempted = false;
};

bool MIParser::Impl::expect(MITokenKind Kind, std::string_view Description) {
  if (consume(Kind))
    return false;
  return fail("expected " + std::string(Description) + ", found " +
              std::string(getMITokenKindName(token().kind())));
}

bool MIParser::Impl::fail(const MIToken &At, std::string Message) {
  if (Diagnostics.empty()) {
    Diagnostics.push_back(TextDiagnostic{
        .Severity = DiagnosticSeverity::Error,
        .Category = "mir-parser",
        .Message = std::move(Message),
        .SourceName = std::string(Lexer.source().name()),
        .Range = At.range(),
    });
  }
  return true;
}

Expected<void> MIParser::Impl::failureResult() const {
  const std::string Message =
      Diagnostics.empty() ? "machine instruction parsing failed" : Diagnostics.front().Message;
  return makeUnexpected(ErrorCode::ParseError, Message);
}

bool MIParser::Impl::importError(const MIToken &At, const Error &Failure) {
  return fail(At, std::string(Failure.message()));
}

bool MIParser::Impl::tokenize() {
  while (true) {
    MIToken Token = Lexer.next();
    const MITokenKind Kind = Token.kind();
    Tokens.push_back(std::move(Token));
    if (Kind == MITokenKind::Error) {
      const auto LexerDiagnostics = Lexer.diagnostics();
      Diagnostics.assign(LexerDiagnostics.begin(), LexerDiagnostics.end());
      return true;
    }
    if (Kind == MITokenKind::EndOfFile)
      return false;
  }
}

bool MIParser::Impl::parseUnsigned(const MIToken &Token, std::uint64_t &Value) {
  if (Token.kind() == MITokenKind::HexLiteral)
    return !parseHexadecimal(Token.spelling(), Value);
  if (Token.hasIntegerValue())
    return !parseUnsignedDecimal(Token.integerValue(), Value);
  return true;
}

bool MIParser::Impl::parseUnsigned32(const MIToken &Token, std::uint32_t &Value) {
  std::uint64_t Wide = 0;
  if (parseUnsigned(Token, Wide) || Wide > std::numeric_limits<std::uint32_t>::max())
    return true;
  Value = static_cast<std::uint32_t>(Wide);
  return false;
}

bool MIParser::Impl::parseSigned64(const MIToken &Token, std::int64_t &Value) {
  if (Token.kind() != MITokenKind::IntegerLiteral)
    return true;
  if (parseSignedDecimal(Token.integerValue(), Value))
    return false;
  std::uint64_t Unsigned = 0;
  if (!parseUnsignedDecimal(Token.integerValue(), Unsigned))
    return true;
  Value = std::bit_cast<std::int64_t>(Unsigned);
  return false;
}

bool MIParser::Impl::parseOptionalOffset(std::int64_t &Offset) {
  Offset = 0;
  if (!at(MITokenKind::Plus) && !at(MITokenKind::Minus))
    return false;
  const bool Negative = at(MITokenKind::Minus);
  advance();
  std::uint64_t Magnitude = 0;
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Magnitude) ||
      Magnitude > (std::uint64_t{1} << 63U)) {
    return fail("expected a 64-bit integer offset");
  }
  if (Negative) {
    if (Magnitude == (std::uint64_t{1} << 63U))
      Offset = std::numeric_limits<std::int64_t>::min();
    else
      Offset = -static_cast<std::int64_t>(Magnitude);
  } else {
    if (Magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return fail("positive offset exceeds signed 64-bit range");
    Offset = static_cast<std::int64_t>(Magnitude);
  }
  advance();
  return false;
}

bool MIParser::Impl::parseAlignment(Alignment &Result) {
  advance();
  std::uint64_t Bytes = 0;
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Bytes))
    return fail("expected an unsigned alignment");
  auto Parsed = Alignment::create(Bytes);
  if (!Parsed)
    return importError(token(), Parsed.error());
  Result = *Parsed;
  advance();
  return false;
}

bool MIParser::Impl::parseScalarLowLevelType(const MIToken &TypeToken, LowLevelType &Result) {
  if (TypeToken.kind() != MITokenKind::Identifier)
    return fail(TypeToken, "expected a scalar low-level type");
  std::string_view Spelling = TypeToken.stringValue();
  enum class ScalarKind : std::uint8_t { CategoryNeutral, Pointer };
  ScalarKind Kind = ScalarKind::CategoryNeutral;
  std::string_view Digits;
  if (Spelling.starts_with("bf")) {
    Digits = Spelling.substr(2);
  } else if (Spelling.starts_with('s')) {
    Digits = Spelling.substr(1);
  } else if (Spelling.starts_with('i')) {
    Digits = Spelling.substr(1);
  } else if (Spelling.starts_with('f')) {
    Digits = Spelling.substr(1);
  } else if (Spelling.starts_with('p')) {
    Kind = ScalarKind::Pointer;
    Digits = Spelling.substr(1);
  } else {
    return fail(TypeToken, "unknown scalar low-level type '" + std::string(Spelling) + "'");
  }

  std::uint64_t Value = 0;
  if (!parseUnsignedDigits(Digits, Value))
    return fail(TypeToken, "expected integers after 's'/'i'/'f'/'bf'/'p' type identifier");

  Expected<LowLevelType> Created = makeUnexpected(ErrorCode::ParseError, "invalid low-level type");
  switch (Kind) {
  case ScalarKind::CategoryNeutral:
    if (Value == 0) {
      Result = LowLevelType::token();
      return false;
    }
    Created = LowLevelType::makeCategoryNeutralScalar(Value);
    break;
  case ScalarKind::Pointer:
    if (!Context.PointerSizeInBits)
      return fail(TypeToken, "pointer low-level type requires an explicit pointer size");
    if (auto PointerSize = Context.PointerSizeInBits(static_cast<std::uint32_t>(Value)))
      Created = LowLevelType::makePointer(Value, *PointerSize);
    else
      return importError(TypeToken, PointerSize.error());
    break;
  }
  if (!Created)
    return importError(TypeToken, Created.error());
  Result = *Created;
  return false;
}

bool MIParser::Impl::parseLowLevelType(LowLevelType &Result) {
  const MIToken Begin = token();
  const auto HasScalarPrefix = [](std::string_view Spelling) {
    return Spelling.starts_with('s') || Spelling.starts_with('i') || Spelling.starts_with('f') ||
           Spelling.starts_with("bf") || Spelling.starts_with('p');
  };
  if (at(MITokenKind::Identifier)) {
    if (!HasScalarPrefix(token().stringValue())) {
      return fail(Begin, "expected tN, pA, <M x tN>, <M x pA>, <vscale x M x tN>, or "
                         "<vscale x M x pA> for GlobalISel type, where t = {'s', 'i', 'f', 'bf'}");
    }
    if (parseScalarLowLevelType(token(), Result))
      return true;
    advance();
    return false;
  }
  if (!consume(MITokenKind::Less)) {
    return fail(Begin, "expected tN, pA, <M x tN>, <M x pA>, <vscale x M x tN>, or "
                       "<vscale x M x pA> for GlobalISel type, where t = {'s', 'i', 'f', 'bf'}");
  }

  bool Scalable = false;
  if (at(MITokenKind::Identifier) && token().stringValue() == "vscale") {
    Scalable = true;
    advance();
    if (!at(MITokenKind::Identifier) || token().stringValue() != "x") {
      return fail("expected <vscale x M x tN>, where t = {'s', 'i', 'f', 'bf', 'p'}");
    }
    advance();
  }

  const auto VectorError = [this, &Begin, Scalable] {
    if (Scalable) {
      return fail(Begin, "expected <vscale x M x tN> for vector type, where t = "
                         "{'s', 'i', 'f', 'bf', 'p'}");
    }
    return fail(Begin, "expected <M x tN> for vector type, where t = {'s', 'i', 'f', 'bf', 'p'}");
  };

  std::uint64_t ElementCount = 0;
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), ElementCount))
    return VectorError();
  advance();
  if (!at(MITokenKind::Identifier) || token().stringValue() != "x")
    return VectorError();
  advance();

  if (!at(MITokenKind::Identifier) || !HasScalarPrefix(token().stringValue()))
    return VectorError();
  LowLevelType ElementType;
  if (parseScalarLowLevelType(token(), ElementType))
    return true;
  if (ElementType.isToken())
    return fail("token cannot be a vector element type");
  advance();
  if (!consume(MITokenKind::Greater))
    return VectorError();

  Expected<LowLevelType> Created = Scalable
                                       ? LowLevelType::makeScalableVector(ElementCount, ElementType)
                                       : LowLevelType::makeFixedVector(ElementCount, ElementType);
  if (!Created)
    return importError(Begin, Created.error());
  Result = *Created;
  return false;
}

bool MIParser::Impl::parseBlockAttributes(MachineBasicBlock *Block) {
  std::unordered_set<std::string> Seen;
  while (!at(MITokenKind::RightParen)) {
    const MIToken AttributeToken = token();
    if (atKeyword("machine-block-address-taken") || atKeyword("landing-pad") ||
        atKeyword("inlineasm-br-indirect-target") || atKeyword("ehfunclet-entry") ||
        atKeyword("ehscope-entry")) {
      const std::string Name(AttributeToken.stringValue());
      if (!Seen.insert(Name).second)
        return fail("duplicate basic block attribute '" + Name + "'");
      if (Block != nullptr) {
        if (Name == "machine-block-address-taken")
          Block->attributes().set(MachineBasicBlockAttribute::MachineBlockAddressTaken);
        else if (Name == "landing-pad")
          Block->attributes().set(MachineBasicBlockAttribute::LandingPad);
        else if (Name == "inlineasm-br-indirect-target")
          Block->attributes().set(MachineBasicBlockAttribute::InlineAsmBrIndirectTarget);
        else if (Name == "ehfunclet-entry")
          Block->attributes().set(MachineBasicBlockAttribute::EHFuncletEntry);
        else
          Block->attributes().set(MachineBasicBlockAttribute::EHScopeEntry);
      }
      advance();
    } else if (atKeyword("ir-block-address-taken")) {
      if (!Seen.insert("ir-block-address-taken").second)
        return fail("duplicate basic block attribute 'ir-block-address-taken'");
      advance();
      if (!at(MITokenKind::IRBlock) && !at(MITokenKind::NamedIRBlock))
        return fail("expected an IR block after 'ir-block-address-taken'");
      if (Block != nullptr) {
        Block->attributes().set(MachineBasicBlockAttribute::IRBlockAddressTaken);
        auto SetName = Block->attributes().setIRBlockName(symbolName(token()));
        if (!SetName)
          return importError(token(), SetName.error());
      }
      advance();
    } else if (at(MITokenKind::IRBlock) || at(MITokenKind::NamedIRBlock)) {
      if (!Seen.insert("ir-block").second)
        return fail("duplicate IR block attribute");
      if (Context.HasIRBlock && !Context.HasIRBlock(symbolName(token())))
        return fail(token(), "use of undefined IR block '" + std::string(token().spelling()) + "'");
      if (Block != nullptr) {
        auto SetName = Block->attributes().setIRBlockName(symbolName(token()));
        if (!SetName)
          return importError(token(), SetName.error());
      }
      advance();
    } else if (atKeyword("align")) {
      if (!Seen.insert("align").second)
        return fail("duplicate basic block alignment");
      Alignment Align;
      if (parseAlignment(Align))
        return true;
      if (Block != nullptr)
        Block->attributes().setAlignment(Align);
    } else if (atKeyword("bbsections")) {
      if (!Seen.insert("bbsections").second)
        return fail("duplicate basic block section");
      advance();
      MachineBasicBlockSectionID Section;
      std::uint32_t Number = 0;
      if (at(MITokenKind::IntegerLiteral) && !parseUnsigned32(token(), Number))
        Section = MachineBasicBlockSectionID::numbered(Number);
      else if (at(MITokenKind::Identifier) && token().stringValue() == "Exception")
        Section = MachineBasicBlockSectionID::exception();
      else if (at(MITokenKind::Identifier) && token().stringValue() == "Cold")
        Section = MachineBasicBlockSectionID::cold();
      else
        return fail("unknown basic block section ID");
      if (Block != nullptr)
        Block->attributes().setSectionID(Section);
      advance();
    } else if (atKeyword("bb_id")) {
      if (!Seen.insert("bb_id").second)
        return fail("duplicate basic block profile ID");
      advance();
      std::uint32_t BaseID = 0;
      std::uint32_t CloneID = 0;
      if (at(MITokenKind::FloatingPointLiteral)) {
        const std::string_view Spelling = token().spelling();
        const std::size_t Dot = Spelling.find('.');
        std::uint64_t Base = 0;
        std::uint64_t Clone = 0;
        if (Dot == std::string_view::npos || !parseUnsignedDecimal(Spelling.substr(0, Dot), Base) ||
            !parseUnsignedDecimal(Spelling.substr(Dot + 1), Clone) ||
            Base > std::numeric_limits<std::uint32_t>::max() ||
            Clone > std::numeric_limits<std::uint32_t>::max()) {
          return fail("invalid basic block profile ID");
        }
        BaseID = static_cast<std::uint32_t>(Base);
        CloneID = static_cast<std::uint32_t>(Clone);
        advance();
      } else {
        if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), BaseID))
          return fail("invalid basic block base ID");
        advance();
        if (consume(MITokenKind::Dot) || consume(MITokenKind::Comma)) {
          if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), CloneID))
            return fail("invalid basic block clone ID");
          advance();
        }
      }
      if (Block != nullptr)
        Block->attributes().setProfileID({BaseID, CloneID});
    } else if (atKeyword("call-frame-size")) {
      if (!Seen.insert("call-frame-size").second)
        return fail("duplicate basic block call frame size");
      advance();
      std::uint32_t Size = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), Size))
        return fail("invalid basic block call frame size");
      if (Block != nullptr)
        Block->attributes().setCallFrameSize(Size);
      advance();
    } else {
      return fail("unknown basic block attribute '" + std::string(token().spelling()) + "'");
    }

    if (!consume(MITokenKind::Comma))
      break;
  }
  return expect(MITokenKind::RightParen, "')'");
}

bool MIParser::Impl::parseBlockDefinition(bool Create, std::uint32_t &ID) {
  const MIToken Label = token();
  if (!at(MITokenKind::MachineBasicBlockLabel) || parseUnsigned32(Label, ID))
    return fail("invalid machine basic block label");
  advance();

  MachineBasicBlock *Block = nullptr;
  if (Create) {
    if (Blocks.size() >= Options.MaximumBlocks)
      return fail(Label, "machine basic block count exceeds parser limit");
    if (Blocks.contains(ID))
      return fail(Label, "redefinition of machine basic block with id #" + std::to_string(ID));
    if (!Label.stringValue().empty() && Context.HasIRBlock &&
        !Context.HasIRBlock(Label.stringValue())) {
      return fail(Label, "basic block '" + std::string(Label.stringValue()) +
                             "' is not defined in the function '" + std::string(Function.name()) +
                             "'");
    }
    auto Created = Function.createBasicBlock();
    if (!Created)
      return importError(Label, Created.error());
    Block = *Created;
    auto Appended = Function.appendBlock(*Block);
    if (!Appended)
      return importError(Label, Appended.error());
    if (!Label.stringValue().empty()) {
      auto SetName = Block->attributes().setName(std::string(Label.stringValue()));
      if (!SetName)
        return importError(Label, SetName.error());
    }
    Blocks.emplace(ID, Block);
  } else {
    const auto Found = Blocks.find(ID);
    if (Found == Blocks.end())
      return fail(Label, "use of undefined machine basic block #" + std::to_string(ID));
    Block = Found->second;
  }

  if (consume(MITokenKind::LeftParen) && parseBlockAttributes(Create ? Block : nullptr))
    return true;
  return expect(MITokenKind::Colon, "':' after the basic block definition");
}

bool MIParser::Impl::parseFirstPass() {
  Position = 0;
  bool AtLineStart = true;
  std::size_t BraceDepth = 0;
  while (!at(MITokenKind::EndOfFile)) {
    if (consume(MITokenKind::Newline)) {
      AtLineStart = true;
      continue;
    }
    if (at(MITokenKind::MachineBasicBlockLabel)) {
      if (!AtLineStart || token().range().Begin.Column != 1)
        return fail("basic block definition must be at the start of a line");
      std::uint32_t ID = 0;
      if (parseBlockDefinition(true, ID))
        return true;
      AtLineStart = false;
      continue;
    }
    if (at(MITokenKind::LeftBrace))
      ++BraceDepth;
    else if (at(MITokenKind::RightBrace)) {
      if (BraceDepth == 0)
        return fail("extraneous closing instruction bundle brace");
      --BraceDepth;
    }
    AtLineStart = false;
    advance();
  }
  if (BraceDepth != 0)
    return fail("unterminated instruction bundle");
  return false;
}

Register MIParser::Impl::getOrCreateVirtualRegister(const MIToken &Token) {
  if (Token.kind() == MITokenKind::VirtualRegister) {
    std::uint32_t ID = 0;
    if (parseUnsigned32(Token, ID))
      return {};
    if (const auto Found = NumericVirtualRegisters.find(ID);
        Found != NumericVirtualRegisters.end()) {
      return Found->second;
    }
    auto Created = Function.registerInfo().createVirtualRegister();
    if (!Created) {
      static_cast<void>(importError(Token, Created.error()));
      return {};
    }
    NumericVirtualRegisters.emplace(ID, *Created);
    return *Created;
  }

  const std::string Name(Token.stringValue());
  if (const auto Found = NamedVirtualRegisters.find(Name); Found != NamedVirtualRegisters.end())
    return Found->second;
  auto Created = Function.registerInfo().createVirtualRegister();
  if (!Created) {
    static_cast<void>(importError(Token, Created.error()));
    return {};
  }
  auto Info = Function.registerInfo().virtualRegisterInfo(*Created);
  if (!Info) {
    static_cast<void>(importError(Token, Info.error()));
    return {};
  }
  Info->get().Name = Name;
  NamedVirtualRegisters.emplace(Name, *Created);
  return *Created;
}

bool MIParser::Impl::parseRegister(Register &Result) {
  const MIToken RegisterToken = token();
  if (consume(MITokenKind::Underscore)) {
    Result = {};
    return false;
  }
  if (at(MITokenKind::NamedRegister)) {
    if (RegisterToken.stringValue() == "noreg") {
      Result = {};
      advance();
      return false;
    }
    const PhysicalRegisterDesc *Description =
        Target.registers().physicalRegister(RegisterToken.stringValue());
    if (Description == nullptr)
      return fail("unknown physical register '$" + std::string(RegisterToken.stringValue()) + "'");
    Result = Register::fromPhysical(Description->Reg);
    advance();
    return false;
  }
  if (at(MITokenKind::VirtualRegister) || at(MITokenKind::NamedVirtualRegister)) {
    Result = getOrCreateVirtualRegister(RegisterToken);
    if (Result.isNoRegister())
      return true;
    advance();
    return false;
  }
  return fail("expected a machine register");
}

bool MIParser::Impl::parseRegisterOperand(bool IsDef, ParsedOperand &Result) {
  const MIToken Begin = token();
  RegisterOperandFlags Flags{IsDef ? RegisterOperandFlag::Define : RegisterOperandFlag::Use};
  std::unordered_set<std::string> SeenFlags;
  while (token().isRegisterFlag()) {
    const std::string Name(token().stringValue());
    if (!SeenFlags.insert(Name).second)
      return fail("duplicate '" + Name + "' register flag");
    if (Name == "implicit")
      Flags.set(RegisterOperandFlag::Implicit);
    else if (Name == "implicit-def") {
      Flags.clear(RegisterOperandFlag::Use);
      Flags.set(RegisterOperandFlag::Define);
      Flags.set(RegisterOperandFlag::Implicit);
      IsDef = true;
    } else if (Name == "def") {
      Flags.clear(RegisterOperandFlag::Use);
      Flags.set(RegisterOperandFlag::Define);
      IsDef = true;
    } else if (Name == "dead")
      Flags.set(RegisterOperandFlag::Dead);
    else if (Name == "killed")
      Flags.set(RegisterOperandFlag::Kill);
    else if (Name == "undef")
      Flags.set(RegisterOperandFlag::Undef);
    else if (Name == "internal")
      Flags.set(RegisterOperandFlag::InternalRead);
    else if (Name == "early-clobber")
      Flags.set(RegisterOperandFlag::EarlyClobber);
    else if (Name == "debug-use")
      Flags.set(RegisterOperandFlag::Debug);
    else if (Name == "renamable")
      Flags.set(RegisterOperandFlag::Renamable);
    advance();
  }

  Register Reg;
  if (parseRegister(Reg))
    return true;

  SubRegIndex SubReg;
  if (consume(MITokenKind::Dot)) {
    if (!Reg.isVirtual() || !at(MITokenKind::Identifier))
      return fail("subregister index requires a virtual register");
    const SubRegIndexDesc *Description = Target.registers().subRegIndex(token().stringValue());
    if (Description == nullptr)
      return fail("unknown subregister index '" + std::string(token().stringValue()) + "'");
    SubReg = Description->ID;
    advance();
  }

  if (consume(MITokenKind::Colon)) {
    if (!Reg.isVirtual())
      return fail("register class or bank requires a virtual register");
    auto InfoResult = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!InfoResult)
      return importError(Begin, InfoResult.error());
    VirtualRegisterInfo &Info = InfoResult->get();
    if (consume(MITokenKind::Underscore)) {
      if (Info.RegClass || Info.Bank)
        return fail("conflicting generic virtual register constraint");
    } else if (at(MITokenKind::Identifier)) {
      if (const RegisterClassDesc *Class =
              Target.registers().registerClass(token().stringValue())) {
        if ((Info.RegClass && *Info.RegClass != Class->ID) ||
            (Info.Bank && *Info.Bank != Class->DefaultBank))
          return fail("conflicting virtual register class");
        auto SetClass = Function.registerInfo().setRegisterClass(Reg, Class->ID);
        if (!SetClass)
          return importError(token(), SetClass.error());
        auto SetBank = Function.registerInfo().setRegisterBank(Reg, Class->DefaultBank);
        if (!SetBank)
          return importError(token(), SetBank.error());
      } else if (const RegisterBankDesc *Bank =
                     Target.registers().registerBank(token().stringValue())) {
        const RegisterClassDesc *ExistingClass =
            Info.RegClass ? Target.registers().registerClass(*Info.RegClass) : nullptr;
        if ((Info.Bank && *Info.Bank != Bank->ID) ||
            (Info.RegClass && (ExistingClass == nullptr || ExistingClass->DefaultBank != Bank->ID)))
          return fail("conflicting virtual register bank");
        auto SetBank = Function.registerInfo().setRegisterBank(Reg, Bank->ID);
        if (!SetBank)
          return importError(token(), SetBank.error());
      } else {
        return fail("unknown register class or bank '" + std::string(token().stringValue()) + "'");
      }
      advance();
    } else {
      return fail("expected '_', register class, or register bank name");
    }
  }

  std::optional<std::size_t> TiedDef;
  if (consume(MITokenKind::LeftParen)) {
    if (atKeyword("tied-def")) {
      if (IsDef)
        return fail("'tied-def' is only valid on register uses");
      advance();
      std::uint64_t Index = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Index) ||
          Index > std::numeric_limits<std::size_t>::max()) {
        return fail("invalid tied-def operand index");
      }
      TiedDef = static_cast<std::size_t>(Index);
      advance();
      if (expect(MITokenKind::RightParen, "')' after tied-def"))
        return true;
    } else {
      if (!Reg.isVirtual())
        return fail("low-level type is only valid on a virtual register");
      LowLevelType Type;
      if (parseLowLevelType(Type))
        return true;
      if (expect(MITokenKind::RightParen, "')' after the low-level type"))
        return true;
      auto InfoResult = Function.registerInfo().virtualRegisterInfo(Reg);
      if (!InfoResult)
        return importError(Begin, InfoResult.error());
      VirtualRegisterInfo &Info = InfoResult->get();
      if (Info.Type && *Info.Type != Type)
        return fail(Begin, "conflicting virtual register low-level type");
      auto SetType = Function.registerInfo().setType(Reg, Type);
      if (!SetType)
        return importError(Begin, SetType.error());
    }
  }

  if (IsDef && Reg.isVirtual()) {
    auto InfoResult = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!InfoResult)
      return importError(Begin, InfoResult.error());
    const VirtualRegisterInfo &Info = InfoResult->get();
    if (!Info.RegClass && !Info.Type)
      return fail(Begin, "generic virtual register definition requires a low-level type");
  }

  if (Flags.has(RegisterOperandFlag::Define) && Flags.has(RegisterOperandFlag::Kill))
    return fail(Begin, "defined register operand cannot be killed");
  if (!Flags.has(RegisterOperandFlag::Define) && Flags.has(RegisterOperandFlag::Dead))
    return fail(Begin, "register use cannot be dead");

  auto Operand = MachineOperand::registerOperand(Reg, Flags, SubReg);
  if (!Operand)
    return importError(Begin, Operand.error());
  Result =
      ParsedOperand{std::move(*Operand), TiedDef, {Begin.range().Begin, token().range().Begin}};
  return false;
}

MachineBasicBlock *MIParser::Impl::blockFor(const MIToken &Token) {
  std::uint32_t ID = 0;
  if (parseUnsigned32(Token, ID)) {
    static_cast<void>(fail(Token, "invalid machine basic block reference"));
    return nullptr;
  }
  const auto Found = Blocks.find(ID);
  if (Found == Blocks.end()) {
    static_cast<void>(fail(Token, "use of undefined machine basic block #" + std::to_string(ID)));
    return nullptr;
  }
  if (!Token.stringValue().empty()) {
    const std::optional<std::string_view> Name = Found->second->attributes().name();
    if (!Name || *Name != Token.stringValue()) {
      static_cast<void>(fail(Token, "machine basic block reference has a mismatched name"));
      return nullptr;
    }
  }
  return Found->second;
}

std::optional<FrameIndex> MIParser::Impl::frameIndexFor(const MIToken &Token, bool Fixed) {
  std::uint32_t ID = 0;
  if (parseUnsigned32(Token, ID)) {
    static_cast<void>(fail(Token, "invalid stack object reference"));
    return std::nullopt;
  }
  const auto &Slots = Fixed ? Context.FixedStackObjects : Context.StackObjects;
  for (const MIFrameObjectSlot &Slot : Slots) {
    if (Slot.TextID != ID)
      continue;
    if (!Token.stringValue().empty() && (!Slot.Name || *Slot.Name != Token.stringValue())) {
      static_cast<void>(fail(Token, "stack object reference has a mismatched name"));
      return std::nullopt;
    }
    if (Function.frameInfo().object(Slot.Index) == nullptr) {
      static_cast<void>(fail(Token, "stack object slot maps to a missing frame object"));
      return std::nullopt;
    }
    return Slot.Index;
  }
  static_cast<void>(fail(Token, std::string("use of undefined ") +
                                    (Fixed ? "fixed stack" : "stack") + " object #" +
                                    std::to_string(ID)));
  return std::nullopt;
}

std::optional<ConstantPoolIndex> MIParser::Impl::constantPoolIndexFor(const MIToken &Token) {
  std::uint32_t ID = 0;
  if (parseUnsigned32(Token, ID)) {
    static_cast<void>(fail(Token, "invalid constant pool reference"));
    return std::nullopt;
  }
  for (const MIConstantPoolSlot &Slot : Context.ConstantPoolItems) {
    if (Slot.TextID == ID && Function.constantPool().entry(Slot.Index) != nullptr)
      return Slot.Index;
  }
  static_cast<void>(fail(Token, "use of undefined constant pool item #" + std::to_string(ID)));
  return std::nullopt;
}

std::optional<JumpTableIndex> MIParser::Impl::jumpTableIndexFor(const MIToken &Token) {
  std::uint32_t ID = 0;
  if (parseUnsigned32(Token, ID)) {
    static_cast<void>(fail(Token, "invalid jump table reference"));
    return std::nullopt;
  }
  for (const MIJumpTableSlot &Slot : Context.JumpTables) {
    if (Slot.TextID == ID && (Function.jumpTableInfo().table(Slot.Index) != nullptr ||
                              (Slot.Index.value() < Function.jumpTableInfo().tables().size() &&
                               Function.jumpTableInfo().tables()[Slot.Index.value()].isPending())))
      return Slot.Index;
  }
  static_cast<void>(fail(Token, "use of undefined jump table #" + std::to_string(ID)));
  return std::nullopt;
}

std::string MIParser::Impl::symbolName(const MIToken &Token) const {
  if (Token.hasStringValue())
    return std::string(Token.stringValue());
  return std::string(Token.integerValue());
}

bool MIParser::Impl::parseTargetFlags(std::uint32_t &Flags) {
  Flags = 0;
  if (!atKeyword("target-flags"))
    return false;
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after target-flags"))
    return true;
  if (!at(MITokenKind::Identifier))
    return fail("expected a target flag name");

  const std::string First(token().stringValue());
  if (const auto Direct = Target.mirText().directOperandFlag(First))
    Flags = *Direct;
  else if (const auto Bit = Target.mirText().bitmaskOperandFlag(First))
    Flags = *Bit;
  else
    return fail("unknown target operand flag '" + First + "'");
  advance();

  std::unordered_set<std::string> Seen{First};
  while (consume(MITokenKind::Comma)) {
    if (!at(MITokenKind::Identifier))
      return fail("expected a target bit flag name");
    const std::string Name(token().stringValue());
    const auto Bit = Target.mirText().bitmaskOperandFlag(Name);
    if (!Bit)
      return fail("unknown target bit operand flag '" + Name + "'");
    if (!Seen.insert(Name).second || (Flags & *Bit) == *Bit)
      return fail("duplicate target operand flag '" + Name + "'");
    Flags |= *Bit;
    advance();
  }
  if (expect(MITokenKind::RightParen, "')' after target flags"))
    return true;
  const auto Valid = Target.mirText().validateOperandFlags(Flags);
  return !Valid && importError(token(), Valid.error());
}

bool MIParser::Impl::parseTypedIntegerOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  const std::string_view TypeSpelling = Begin.stringValue();
  if (!TypeSpelling.starts_with('i'))
    return fail(Begin, "typed integer operand must use an 'iN' type");
  std::uint64_t WideBitWidth = 0;
  if (!parseUnsignedDigits(TypeSpelling.substr(1), WideBitWidth) ||
      WideBitWidth > APInt::MaxBitWidth) {
    return fail(Begin, "typed integer operand has an invalid bit width");
  }
  const auto BitWidth = static_cast<std::uint32_t>(WideBitWidth);
  advance();

  std::string_view ValueSpelling;
  if (at(MITokenKind::IntegerLiteral)) {
    ValueSpelling = token().integerValue();
  } else if (at(MITokenKind::Identifier) && token().stringValue() == "true") {
    ValueSpelling = "1";
  } else if (at(MITokenKind::Identifier) && token().stringValue() == "false") {
    ValueSpelling = "0";
  } else {
    return fail("expected a decimal integer, true, or false after the typed integer type");
  }

  auto Value = parseIntegerImmediateValue(BitWidth, ValueSpelling);
  if (!Value)
    return importError(token(), Value.error());
  advance();
  Result = ParsedOperand{
      MachineOperand::integerImmediate(std::move(*Value)),
      std::nullopt,
      {Begin.range().Begin, token().range().Begin},
  };
  return false;
}

bool MIParser::Impl::parseFloatingImmediateOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  const auto Semantics = floatSemanticsForKeyword(Begin.stringValue());
  if (!Semantics)
    return fail(Begin, "unknown floating-point immediate type");
  advance();
  if (!at(MITokenKind::FloatingPointLiteral) && !at(MITokenKind::HexLiteral))
    return fail("expected a floating-point literal");

  const MIToken Literal = token();
  std::string ConstantText(Begin.stringValue());
  ConstantText.push_back(' ');
  ConstantText.append(Literal.spelling());
  auto Constant = parseLLVMConstant(ConstantText);
  if (!Constant)
    return fail(Literal, std::string(Constant.error().message()));
  if (!Constant->FloatingValue || Constant->FloatingValue->semantics() != *Semantics)
    return fail(Literal, "floating-point literal produced the wrong semantics");
  advance();
  Result = ParsedOperand{
      MachineOperand::floatingImmediate(*Constant->FloatingValue),
      std::nullopt,
      {Begin.range().Begin, token().range().Begin},
  };
  return false;
}

bool MIParser::Impl::parseMetadataReference(MachineMetadataReference &Result) {
  const MIToken Begin = token();
  if (expect(MITokenKind::Exclaim, "'!' before a metadata slot"))
    return true;
  std::uint32_t TextID = 0;
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), TextID))
    return fail("expected a 32-bit metadata slot number");

  for (const MIMetadataSlot &Slot : Context.Metadata) {
    if (Slot.TextID != TextID)
      continue;
    if (!Slot.Reference.isValid())
      return fail(Begin, "metadata slot maps to an invalid reference");
    Result = Slot.Reference;
    advance();
    return false;
  }
  if (Function.metadata().node(TextID) != nullptr) {
    Result = MachineMetadataReference::machineFunction(TextID);
    advance();
    return false;
  }
  return fail(Begin, "use of undefined metadata slot !" + std::to_string(TextID));
}

bool MIParser::Impl::parseMetadataOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  MachineMetadataValue Value;
  if (at(MITokenKind::Exclaim)) {
    MachineMetadataReference Reference;
    if (parseMetadataReference(Reference))
      return true;
    auto Created = MachineMetadataValue::reference(Reference);
    if (!Created)
      return importError(Begin, Created.error());
    Value = std::move(*Created);
  } else if (at(MITokenKind::MetadataKeyword) && token().isMetadataKeyword("!DIExpression")) {
    advance();
    if (expect(MITokenKind::LeftParen, "'(' after !DIExpression"))
      return true;
    std::vector<std::uint64_t> Elements;
    while (!at(MITokenKind::RightParen)) {
      if (at(MITokenKind::Identifier)) {
        const auto Opcode = parseDIExpressionOpcodeName(token().stringValue());
        const auto Encoding = parseDIExpressionAttributeEncodingName(token().stringValue());
        if (!Opcode && !Encoding)
          return fail(invalidDIExpressionElementMessage(token().stringValue()));
        Elements.push_back(Opcode ? *Opcode : *Encoding);
        advance();
      } else if (at(MITokenKind::IntegerLiteral)) {
        std::int64_t Signed = 0;
        if (parseSigned64(token(), Signed))
          return fail("debug expression element exceeds 64 bits");
        Elements.push_back(std::bit_cast<std::uint64_t>(Signed));
        advance();
      } else {
        return fail("expected a debug expression opcode or integer");
      }
      if (!consume(MITokenKind::Comma))
        break;
    }
    if (expect(MITokenKind::RightParen, "')' after !DIExpression"))
      return true;
    Value = MachineMetadataValue::expression(MachineDIExpression(std::move(Elements)));
  } else {
    return fail("expected a metadata reference or !DIExpression");
  }

  auto Operand = MachineOperand::metadata(std::move(Value));
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand),
      std::nullopt,
      {Begin.range().Begin, token().range().Begin},
  };
  return false;
}

bool MIParser::Impl::parseInlineDILocation(
    std::vector<MachineDILocationFrame> &InlineChain,
    std::optional<MachineMetadataReference> &TerminalInlinedAt) {
  if (!at(MITokenKind::MetadataKeyword) || !token().isMetadataKeyword("!DILocation")) {
    return fail("expected an inline !DILocation");
  }
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after !DILocation"))
    return true;

  MachineDILocationFrame Frame;
  bool HasLine = false;
  bool HasScope = false;
  std::vector<MachineDILocationFrame> NestedChain;
  std::optional<MachineMetadataReference> NestedTerminal;
  std::unordered_set<std::string> SeenFields;
  while (!at(MITokenKind::RightParen)) {
    if (!at(MITokenKind::Identifier))
      return fail("expected a DILocation field name");
    const MIToken FieldToken = token();
    const std::string Field(token().stringValue());
    if (!SeenFields.insert(Field).second)
      return fail("duplicate DILocation field '" + Field + "'");
    advance();
    if (expect(MITokenKind::Colon, "':' after a DILocation field name"))
      return true;

    if (Field == "line") {
      std::uint32_t Line = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), Line))
        return fail("expected an unsigned 32-bit DILocation line");
      Frame.Line = Line;
      HasLine = true;
      advance();
    } else if (Field == "column") {
      std::uint64_t Column = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Column) ||
          Column > std::numeric_limits<std::uint16_t>::max()) {
        return fail("expected an unsigned 16-bit DILocation column");
      }
      Frame.Column = static_cast<std::uint16_t>(Column);
      advance();
    } else if (Field == "scope") {
      MachineMetadataReference Scope;
      if (parseMetadataReference(Scope))
        return true;
      if (Scope.scope() != MachineMetadataScope::EmbeddedIR)
        return fail(FieldToken, "DILocation scope must reference embedded IR metadata");
      Frame.Scope = Scope;
      HasScope = true;
    } else if (Field == "inlinedAt") {
      if (at(MITokenKind::MetadataKeyword) && token().isMetadataKeyword("!DILocation")) {
        if (parseInlineDILocation(NestedChain, NestedTerminal))
          return true;
      } else {
        MachineMetadataReference Reference;
        if (parseMetadataReference(Reference))
          return true;
        if (Reference.scope() != MachineMetadataScope::EmbeddedIR) {
          return fail(FieldToken, "DILocation inlinedAt must reference embedded IR metadata");
        }
        NestedTerminal = Reference;
      }
    } else if (Field == "isImplicitCode") {
      if (!at(MITokenKind::Identifier) ||
          (token().stringValue() != "true" && token().stringValue() != "false")) {
        return fail("expected 'true' or 'false' for DILocation isImplicitCode");
      }
      Frame.IsImplicitCode = token().stringValue() == "true";
      advance();
    } else if (Field == "atomGroup") {
      std::uint64_t Group = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Group) ||
          Group > MachineDILocation::MaxAtomGroup) {
        return fail("DILocation atomGroup exceeds 61 bits");
      }
      Frame.AtomGroup = Group;
      advance();
    } else if (Field == "atomRank") {
      std::uint64_t Rank = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Rank) || Rank > 7) {
        return fail("DILocation atomRank exceeds three bits");
      }
      Frame.AtomRank = static_cast<std::uint8_t>(Rank);
      advance();
    } else {
      return fail(FieldToken, "unknown DILocation field '" + Field + "'");
    }

    if (!consume(MITokenKind::Comma))
      break;
  }
  if (expect(MITokenKind::RightParen, "')' after !DILocation"))
    return true;
  if (!HasLine)
    return fail("DILocation requires a line field");
  if (!HasScope)
    return fail("DILocation requires a scope field");

  InlineChain.push_back(Frame);
  InlineChain.insert(InlineChain.end(), NestedChain.begin(), NestedChain.end());
  TerminalInlinedAt = NestedTerminal;
  return false;
}

bool MIParser::Impl::parseCFIRegister(DwarfRegisterNumber &Result) {
  if (!at(MITokenKind::NamedRegister))
    return fail("expected a physical register in the CFI instruction");
  const MIToken RegisterToken = token();
  Register Reg;
  if (parseRegister(Reg) || !Reg.isPhysical())
    return true;
  const auto Physical = Reg.physicalRegister();
  if (!Physical)
    return fail(RegisterToken, "CFI register is not a physical register");
  const auto Dwarf = Target.mirText().dwarfRegister(*Physical);
  if (!Dwarf)
    return fail(RegisterToken, "physical register has no canonical DWARF register number");
  Result = DwarfRegisterNumber::fromValue(*Dwarf);
  return false;
}

bool MIParser::Impl::parseCFISigned(std::int64_t &Result) {
  if (!at(MITokenKind::IntegerLiteral) || parseSigned64(token(), Result))
    return fail("expected a signed 64-bit CFI integer");
  advance();
  return false;
}

bool MIParser::Impl::parseCFIUnsigned(std::uint32_t &Result) {
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), Result))
    return fail("expected an unsigned 32-bit CFI integer");
  advance();
  return false;
}

bool MIParser::Impl::parseCFIOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  const std::string Operation(token().stringValue());
  advance();

  const auto Comma = [this]() { return expect(MITokenKind::Comma, "',' in CFI instruction"); };
  DwarfRegisterNumber Register;
  DwarfRegisterNumber OtherRegister;
  std::int64_t Offset = 0;
  std::optional<MachineCFIInstruction> Instruction;
  if (Operation == "same_value") {
    if (parseCFIRegister(Register))
      return true;
    Instruction = MachineCFIInstruction::sameValue(Register);
  } else if (Operation == "remember_state") {
    Instruction = MachineCFIInstruction::rememberState();
  } else if (Operation == "restore_state") {
    Instruction = MachineCFIInstruction::restoreState();
  } else if (Operation == "offset" || Operation == "rel_offset") {
    if (parseCFIRegister(Register) || Comma() || parseCFISigned(Offset))
      return true;
    Instruction = Operation == "offset" ? MachineCFIInstruction::offset(Register, Offset)
                                        : MachineCFIInstruction::relativeOffset(Register, Offset);
  } else if (Operation == "def_cfa_register") {
    if (parseCFIRegister(Register))
      return true;
    Instruction = MachineCFIInstruction::defCFARegister(Register);
  } else if (Operation == "def_cfa_offset" || Operation == "adjust_cfa_offset") {
    if (parseCFISigned(Offset))
      return true;
    Instruction = Operation == "def_cfa_offset" ? MachineCFIInstruction::defCFAOffset(Offset)
                                                : MachineCFIInstruction::adjustCFAOffset(Offset);
  } else if (Operation == "def_cfa") {
    if (parseCFIRegister(Register) || Comma() || parseCFISigned(Offset))
      return true;
    Instruction = MachineCFIInstruction::defCFA(Register, Offset);
  } else if (Operation == "llvm_def_aspace_cfa") {
    std::uint32_t AddressSpace = 0;
    if (parseCFIRegister(Register) || Comma() || parseCFISigned(Offset) || Comma() ||
        parseCFIUnsigned(AddressSpace)) {
      return true;
    }
    Instruction = MachineCFIInstruction::llvmDefAddressSpaceCFA(Register, Offset, AddressSpace);
  } else if (Operation == "restore" || Operation == "undefined") {
    if (parseCFIRegister(Register))
      return true;
    Instruction = Operation == "restore" ? MachineCFIInstruction::restore(Register)
                                         : MachineCFIInstruction::undefined(Register);
  } else if (Operation == "register") {
    if (parseCFIRegister(Register) || Comma() || parseCFIRegister(OtherRegister))
      return true;
    Instruction = MachineCFIInstruction::registerRule(Register, OtherRegister);
  } else if (Operation == "window_save") {
    Instruction = MachineCFIInstruction::windowSave();
  } else if (Operation == "negate_ra_sign_state") {
    Instruction = MachineCFIInstruction::negateReturnAddressState();
  } else if (Operation == "negate_ra_sign_state_with_pc") {
    Instruction = MachineCFIInstruction::negateReturnAddressStateWithPC();
  } else if (Operation == "llvm_register_pair") {
    DwarfRegisterNumber First;
    DwarfRegisterNumber Second;
    std::uint32_t FirstSize = 0;
    std::uint32_t SecondSize = 0;
    if (parseCFIRegister(Register) || Comma() || parseCFIRegister(First) || Comma() ||
        parseCFIUnsigned(FirstSize) || Comma() || parseCFIRegister(Second) || Comma() ||
        parseCFIUnsigned(SecondSize)) {
      return true;
    }
    Instruction = MachineCFIInstruction::llvmRegisterPair({.Register = Register,
                                                           .FirstRegister = First,
                                                           .FirstSizeInBits = FirstSize,
                                                           .SecondRegister = Second,
                                                           .SecondSizeInBits = SecondSize});
  } else if (Operation == "llvm_vector_registers") {
    if (parseCFIRegister(Register) || Comma())
      return true;
    std::vector<MachineCFIVectorRegisterLane> Registers;
    do {
      DwarfRegisterNumber VectorRegister;
      std::uint32_t Lane = 0;
      std::uint32_t Size = 0;
      if (parseCFIRegister(VectorRegister) || Comma() || parseCFIUnsigned(Lane) || Comma() ||
          parseCFIUnsigned(Size)) {
        return true;
      }
      Registers.push_back({.Register = VectorRegister, .Lane = Lane, .SizeInBits = Size});
      if (!at(MITokenKind::Comma))
        break;
      advance();
      if (!at(MITokenKind::NamedRegister))
        return fail("expected another vector CFI register tuple");
    } while (true);
    auto Created = MachineCFIInstruction::llvmVectorRegisters(
        {.Register = Register, .VectorRegisters = std::move(Registers)});
    if (!Created)
      return importError(Begin, Created.error());
    Instruction = std::move(*Created);
  } else if (Operation == "llvm_vector_offset") {
    DwarfRegisterNumber MaskRegister;
    std::uint32_t RegisterSize = 0;
    std::uint32_t MaskSize = 0;
    if (parseCFIRegister(Register) || Comma() || parseCFIUnsigned(RegisterSize) || Comma() ||
        parseCFIRegister(MaskRegister) || Comma() || parseCFIUnsigned(MaskSize) || Comma() ||
        parseCFISigned(Offset)) {
      return true;
    }
    Instruction = MachineCFIInstruction::llvmVectorOffset({.Register = Register,
                                                           .RegisterSizeInBits = RegisterSize,
                                                           .MaskRegister = MaskRegister,
                                                           .MaskRegisterSizeInBits = MaskSize,
                                                           .Offset = Offset});
  } else if (Operation == "llvm_vector_register_mask") {
    DwarfRegisterNumber SpillRegister;
    DwarfRegisterNumber MaskRegister;
    std::uint32_t SpillLaneSize = 0;
    std::uint32_t MaskSize = 0;
    if (parseCFIRegister(Register) || Comma() || parseCFIRegister(SpillRegister) || Comma() ||
        parseCFIUnsigned(SpillLaneSize) || Comma() || parseCFIRegister(MaskRegister) || Comma() ||
        parseCFIUnsigned(MaskSize)) {
      return true;
    }
    Instruction =
        MachineCFIInstruction::llvmVectorRegisterMask({.Register = Register,
                                                       .SpillRegister = SpillRegister,
                                                       .SpillRegisterLaneSizeInBits = SpillLaneSize,
                                                       .MaskRegister = MaskRegister,
                                                       .MaskRegisterSizeInBits = MaskSize});
  } else if (Operation == "escape") {
    std::vector<std::uint8_t> Bytes;
    while (true) {
      std::uint64_t Byte = 0;
      if (!at(MITokenKind::HexLiteral) || parseUnsigned(token(), Byte) ||
          Byte > std::numeric_limits<std::uint8_t>::max()) {
        return fail("CFI escape element must be an 8-bit hexadecimal literal");
      }
      Bytes.push_back(static_cast<std::uint8_t>(Byte));
      advance();
      if (!at(MITokenKind::Comma) || !nextIs(MITokenKind::HexLiteral))
        break;
      advance();
    }
    auto Created = MachineCFIInstruction::escape(std::move(Bytes));
    if (!Created)
      return importError(Begin, Created.error());
    Instruction = std::move(*Created);
  } else {
    return fail(Begin, "unsupported CFI operation '" + Operation + "'");
  }

  auto Index = Function.cfiInfo().addInstruction(std::move(*Instruction));
  if (!Index)
    return importError(Begin, Index.error());
  Result = ParsedOperand{
      MachineOperand::cfiIndex(*Index),
      std::nullopt,
      {Begin.range().Begin, token().range().Begin},
  };
  return false;
}

bool MIParser::Impl::parseMemoryBase(MemoryBase &Result, std::int64_t &Offset) {
  const MIToken BaseToken = token();
  switch (BaseToken.kind()) {
  case MITokenKind::StackObject:
  case MITokenKind::FixedStackObject: {
    const auto Index = frameIndexFor(BaseToken, BaseToken.kind() == MITokenKind::FixedStackObject);
    if (!Index)
      return true;
    Result = MemoryBase::frameIndex(*Index);
    advance();
    break;
  }
  case MITokenKind::ConstantPoolItem: {
    const auto Index = constantPoolIndexFor(BaseToken);
    if (!Index)
      return true;
    Result = MemoryBase::constantPool(*Index);
    advance();
    break;
  }
  case MITokenKind::JumpTableIndex: {
    const auto Index = jumpTableIndexFor(BaseToken);
    if (!Index)
      return true;
    Result = MemoryBase::jumpTable(*Index);
    advance();
    break;
  }
  case MITokenKind::GlobalValue:
  case MITokenKind::NamedGlobalValue:
    Result = MemoryBase::globalSymbol(symbolName(BaseToken));
    advance();
    break;
  case MITokenKind::ExternalSymbol:
    Result = MemoryBase::externalSymbol(symbolName(BaseToken));
    advance();
    break;
  case MITokenKind::IRValue:
  case MITokenKind::NamedIRValue:
    Result = MemoryBase::irValue(symbolName(BaseToken));
    advance();
    break;
  case MITokenKind::QuotedIRValue: {
    auto Constant = parseLLVMConstant(BaseToken.stringValue());
    if (!Constant)
      return importError(BaseToken, Constant.error());
    auto Base = MemoryBase::irConstant(std::move(*Constant));
    if (!Base)
      return importError(BaseToken, Base.error());
    Result = std::move(*Base);
    advance();
    break;
  }
  case MITokenKind::Keyword:
    if (BaseToken.isKeyword("unknown-address")) {
      Result = MemoryBase();
      advance();
    } else if (BaseToken.isKeyword("stack")) {
      Result = MemoryBase::stack();
      advance();
    } else if (BaseToken.isKeyword("got")) {
      Result = MemoryBase::got();
      advance();
    } else if (BaseToken.isKeyword("constant-pool")) {
      Result = MemoryBase::constantPoolPseudo();
      advance();
    } else if (BaseToken.isKeyword("jump-table")) {
      Result = MemoryBase::jumpTablePseudo();
      advance();
    } else if (BaseToken.isKeyword("call-entry")) {
      advance();
      if (at(MITokenKind::GlobalValue) || at(MITokenKind::NamedGlobalValue)) {
        Result = MemoryBase::callEntryGlobal(symbolName(token()));
      } else if (at(MITokenKind::ExternalSymbol)) {
        Result = MemoryBase::callEntryExternal(symbolName(token()));
      } else {
        return fail("call-entry memory base requires a global or external symbol");
      }
      advance();
    } else if (BaseToken.isKeyword("custom")) {
      advance();
      if (!at(MITokenKind::Identifier) && !at(MITokenKind::StringConstant))
        return fail("custom memory base requires a target spelling");
      Result = MemoryBase::custom(std::string(token().stringValue()));
      advance();
    } else {
      return fail(BaseToken, "unsupported machine memory base");
    }
    break;
  default:
    return fail(BaseToken, "expected a machine memory base");
  }
  return parseOptionalOffset(Offset);
}

bool MIParser::Impl::parseMemoryOperand(const MachineMemOperand *&Result,
                                        Opcode InstructionOpcode) {
  const MIToken Begin = token();
  if (expect(MITokenKind::LeftParen, "'(' before a machine memory operand"))
    return true;

  MachineMemOperandDesc Description;
  std::unordered_set<std::string> SeenFlags;
  while (token().isMemoryOperandFlag()) {
    const std::string Name(token().stringValue());
    if (!SeenFlags.insert(Name).second)
      return fail("duplicate machine memory operand flag '" + Name + "'");
    if (atKeyword("volatile"))
      Description.Accesses.set(MemoryAccessFlag::Volatile);
    else if (atKeyword("non-temporal"))
      Description.Accesses.set(MemoryAccessFlag::NonTemporal);
    else if (atKeyword("dereferenceable"))
      Description.Accesses.set(MemoryAccessFlag::Dereferenceable);
    else if (atKeyword("invariant"))
      Description.Accesses.set(MemoryAccessFlag::Invariant);
    else {
      const auto Flag = Target.mirText().machineMemOperandFlag(token().stringValue());
      if (!Flag)
        return fail("unknown target machine memory operand flag '" + Name + "'");
      if ((Description.TargetFlags & *Flag) != 0)
        return fail("overlapping target machine memory operand flag '" + Name + "'");
      Description.TargetFlags |= *Flag;
    }
    advance();
  }

  if (!at(MITokenKind::Identifier) ||
      (token().stringValue() != "load" && token().stringValue() != "store")) {
    return fail("expected load or store in a machine memory operand");
  }
  if (token().stringValue() == "load")
    Description.Accesses.set(MemoryAccessFlag::Load);
  else
    Description.Accesses.set(MemoryAccessFlag::Store);
  advance();
  if (at(MITokenKind::Identifier) && token().stringValue() == "store") {
    if (Description.Accesses.has(MemoryAccessFlag::Store))
      return fail("duplicate store memory operation");
    Description.Accesses.set(MemoryAccessFlag::Store);
    advance();
  }

  if (at(MITokenKind::Identifier) && token().stringValue() == "syncscope") {
    advance();
    if (expect(MITokenKind::LeftParen, "'(' after syncscope"))
      return true;
    if (!at(MITokenKind::StringConstant))
      return fail("syncscope requires a quoted scope name");
    const std::string_view Name = token().stringValue();
    auto Scope = std::ranges::find_if(
        Context.SyncScopes, [Name](const MISyncScopeSlot &Slot) { return Slot.Name == Name; });
    if (Scope == Context.SyncScopes.end() && Context.InternSyncScope) {
      const MIToken NameToken = token();
      auto ID = Context.InternSyncScope(Name);
      if (!ID)
        return importError(NameToken, ID.error());
      Context.SyncScopes.push_back({.Name = std::string(Name), .ID = *ID});
      Scope = std::prev(Context.SyncScopes.end());
    }
    if (Scope == Context.SyncScopes.end())
      return fail("unknown synchronization scope '" + std::string(Name) + "'");
    Description.SyncScope = Scope->ID;
    advance();
    if (expect(MITokenKind::RightParen, "')' after syncscope"))
      return true;
  }

  std::size_t OrderingCount = 0;
  while (at(MITokenKind::Identifier)) {
    const auto Ordering = atomicOrdering(token().stringValue());
    if (!Ordering)
      break;
    if (OrderingCount == 0)
      Description.SuccessOrdering = *Ordering;
    else if (OrderingCount == 1)
      Description.FailureOrdering = *Ordering;
    else
      return fail("machine memory operand has more than two atomic orderings");
    ++OrderingCount;
    advance();
  }
  const bool IsLoad = Description.Accesses.has(MemoryAccessFlag::Load);
  const bool IsStore = Description.Accesses.has(MemoryAccessFlag::Store);
  if (OrderingCount == 1) {
    if (InstructionOpcode == opcode(GenericOpcode::G_ATOMIC_CMPXCHG) ||
        InstructionOpcode == opcode(GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS)) {
      Description.AtomicKind = AtomicOperation::CompareExchange;
    } else {
      Description.AtomicKind = IsLoad && IsStore
                                   ? AtomicOperation::ReadModifyWrite
                                   : (IsLoad ? AtomicOperation::Load : AtomicOperation::Store);
    }
  } else if (OrderingCount == 2)
    Description.AtomicKind = AtomicOperation::CompareExchange;

  std::uint64_t MinimumBytes = 0;
  if (at(MITokenKind::IntegerLiteral)) {
    if (parseUnsigned(token(), MinimumBytes) || MinimumBytes == 0)
      return fail("machine memory operand size must be a nonzero byte count");
    auto Size = MemorySize::fixed(MinimumBytes);
    if (!Size)
      return importError(token(), Size.error());
    Description.Size = *Size;
    advance();
  } else if (atKeyword("unknown-size")) {
    advance();
  } else if (consume(MITokenKind::LeftParen)) {
    LowLevelType Type;
    if (parseLowLevelType(Type))
      return true;
    if (expect(MITokenKind::RightParen, "')' after the memory low-level type"))
      return true;
    const auto MinimumBits = minimumTypeSizeInBits(Type);
    if (!MinimumBits || *MinimumBits == 0)
      return fail(Begin, "memory low-level type has no representable size");
    MinimumBytes = (*MinimumBits + 7U) / 8U;
    auto Size = Type.isScalableVector() ? MemorySize::scalable(MinimumBytes)
                                        : MemorySize::fixed(MinimumBytes);
    if (!Size)
      return importError(Begin, Size.error());
    Description.Size = *Size;
    Description.MemoryType = Type;
  } else {
    return fail("expected a memory size, unknown-size, or memory low-level type");
  }

  if (MinimumBytes != 0) {
    if (MinimumBytes > (std::uint64_t{1} << 63U))
      return fail(Begin, "machine memory operand size has no representable default alignment");
    auto DefaultAlignment = Alignment::create(std::bit_ceil(MinimumBytes));
    if (!DefaultAlignment)
      return importError(Begin, DefaultAlignment.error());
    Description.Align = *DefaultAlignment;
  }

  if (at(MITokenKind::Identifier)) {
    const std::string_view ExpectedPreposition =
        IsLoad && IsStore ? "on" : (IsLoad ? "from" : "into");
    if (token().stringValue() != ExpectedPreposition) {
      return fail("expected '" + std::string(ExpectedPreposition) +
                  "' before the machine memory base");
    }
    advance();
    if (parseMemoryBase(Description.Base, Description.Offset))
      return true;
    if (const GenericConstantData *Constant = Description.Base.constantValue())
      Description.AddressSpace = Constant->Type.AddressSpace;
  }

  Alignment BaseAlignment = Description.Align;
  std::unordered_set<std::string> SeenOptions;
  while (consume(MITokenKind::Comma)) {
    const std::string Name(token().spelling());
    if (!SeenOptions.insert(Name).second)
      return fail("duplicate machine memory operand option '" + Name + "'");
    if (atKeyword("align")) {
      Alignment ParsedAlignment;
      if (parseAlignment(ParsedAlignment))
        return true;
      if ((static_cast<std::uint64_t>(Description.Offset) & (ParsedAlignment.bytes() - 1U)) != 0) {
        return fail("specified alignment is more aligned than offset");
      }
      BaseAlignment = ParsedAlignment;
    } else if (atKeyword("basealign")) {
      if (parseAlignment(BaseAlignment))
        return true;
    } else if (atKeyword("addrspace")) {
      advance();
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), Description.AddressSpace) ||
          Description.AddressSpace > LowLevelType::MaxAddressSpace) {
        return fail("invalid machine memory operand address space");
      }
      advance();
    } else if (at(MITokenKind::MetadataKeyword)) {
      const std::string MetadataKind(token().stringValue());
      advance();
      MachineMetadataReference Reference;
      if (parseMetadataReference(Reference))
        return true;
      if (MetadataKind == "!tbaa")
        Description.TBAA = Reference;
      else if (MetadataKind == "!alias.scope")
        Description.AliasScope = Reference;
      else if (MetadataKind == "!noalias")
        Description.NoAlias = Reference;
      else if (MetadataKind == "!noalias.addrspace")
        Description.NoAliasAddressSpace = Reference;
      else if (MetadataKind == "!range")
        Description.RangeMetadata = Reference;
      else
        return fail("unsupported machine memory operand metadata attachment");
    } else {
      return fail("unknown machine memory operand option '" + Name + "'");
    }
  }

  std::uint64_t EffectiveAlignment = BaseAlignment.bytes();
  if (Description.Offset != 0) {
    const auto OffsetBits = static_cast<std::uint64_t>(Description.Offset);
    const std::uint64_t OffsetAlignment = std::uint64_t{1} << std::countr_zero(OffsetBits);
    EffectiveAlignment = std::min(EffectiveAlignment, OffsetAlignment);
  }
  Description.Align = *Alignment::create(EffectiveAlignment);
  if (BaseAlignment != Description.Align)
    Description.BaseAlign = BaseAlignment;
  else
    Description.BaseAlign.reset();

  if (expect(MITokenKind::RightParen, "')' after the machine memory operand"))
    return true;

  auto Created = Function.createMemOperand(std::move(Description));
  if (!Created)
    return importError(Begin, Created.error());
  Result = *Created;
  return false;
}

bool MIParser::Impl::parsePredicateOperand(ParsedOperand &Result, bool IsFloating) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after predicate kind"))
    return true;
  if (!at(MITokenKind::Identifier))
    return fail("expected a predicate spelling");

  Expected<MachineOperand> Operand = IsFloating ? [&]() -> Expected<MachineOperand> {
    const auto Predicate = floatingPredicate(token().stringValue());
    if (!Predicate) {
      return makeUnexpected(ErrorCode::ParseError, "invalid floating-point predicate");
    }
    return MachineOperand::floatingPredicate(*Predicate);
  }()
      : [&]() -> Expected<MachineOperand> {
          const auto Predicate = integerPredicate(token().stringValue());
          if (!Predicate) {
            return makeUnexpected(ErrorCode::ParseError, "invalid integer predicate");
          }
          return MachineOperand::integerPredicate(*Predicate);
        }();
  if (!Operand)
    return importError(token(), Operand.error());
  advance();
  if (expect(MITokenKind::RightParen, "')' after predicate"))
    return true;
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseShuffleMaskOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after shufflemask"))
    return true;
  std::vector<std::int32_t> Elements;
  while (!at(MITokenKind::RightParen)) {
    if (atKeyword("undef")) {
      Elements.push_back(ShuffleMaskOperand::Undef);
      advance();
    } else {
      std::int64_t Value = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseSigned64(token(), Value) ||
          Value < std::numeric_limits<std::int32_t>::min() ||
          Value > std::numeric_limits<std::int32_t>::max()) {
        return fail("shuffle mask element must be a 32-bit integer or undef");
      }
      Elements.push_back(static_cast<std::int32_t>(Value));
      advance();
    }
    if (!consume(MITokenKind::Comma))
      break;
  }
  if (expect(MITokenKind::RightParen, "')' after shufflemask"))
    return true;
  auto Operand = MachineOperand::shuffleMask(std::move(Elements));
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseDebugInstrRefOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after dbg-instr-ref"))
    return true;
  std::uint64_t Instruction = 0;
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), Instruction))
    return fail("invalid debug instruction number");
  advance();
  if (expect(MITokenKind::Comma, "',' in dbg-instr-ref"))
    return true;
  std::uint64_t OperandNumber = 0;
  if (!at(MITokenKind::IntegerLiteral) || parseUnsigned(token(), OperandNumber))
    return fail("invalid debug operand number");
  advance();
  if (expect(MITokenKind::RightParen, "')' after dbg-instr-ref"))
    return true;
  auto Operand = MachineOperand::debugInstrRef(Instruction, OperandNumber);
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseLaneMaskOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after lanemask"))
    return true;
  std::uint64_t Value = 0;
  if (parseUnsigned(token(), Value))
    return fail("invalid 64-bit lane mask");
  advance();
  if (expect(MITokenKind::RightParen, "')' after lanemask"))
    return true;
  Result = ParsedOperand{MachineOperand::laneMask(LaneMask::fromRaw(Value)),
                         std::nullopt,
                         {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseIntrinsicOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after intrinsic"))
    return true;
  if (!at(MITokenKind::NamedGlobalValue))
    return fail("expected a named intrinsic");
  const auto ID = Target.mirText().intrinsic(token().stringValue());
  if (!ID)
    return fail("unknown intrinsic '" + std::string(token().stringValue()) + "'");
  advance();
  if (expect(MITokenKind::RightParen, "')' after intrinsic"))
    return true;
  auto Operand = MachineOperand::intrinsic(*ID);
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseTargetIndexOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after target-index"))
    return true;
  if (!at(MITokenKind::Identifier))
    return fail("expected a target index name");
  const auto Index = Target.mirText().targetIndex(token().stringValue());
  if (!Index)
    return fail("unknown target index '" + std::string(token().stringValue()) + "'");
  advance();
  if (expect(MITokenKind::RightParen, "')' after target index"))
    return true;
  std::int64_t Offset = 0;
  if (parseOptionalOffset(Offset))
    return true;
  Result = ParsedOperand{MachineOperand::targetIndex(*Index, Offset),
                         std::nullopt,
                         {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseCustomMaskOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after CustomRegMask"))
    return true;
  std::uint32_t MaximumRegister = 0;
  for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters())
    MaximumRegister = std::max(MaximumRegister, Register.Reg.number());
  std::vector<std::uint64_t> Words(MaximumRegister / 64U + 1U, 0);
  if (!at(MITokenKind::RightParen)) {
    do {
      Register Reg;
      if (!at(MITokenKind::NamedRegister) || parseRegister(Reg) || !Reg.isPhysical())
        return fail("CustomRegMask requires physical registers");
      const auto Physical = Reg.physicalRegister();
      if (!Physical)
        return fail("CustomRegMask requires physical registers");
      const std::uint32_t Number = Physical->number();
      Words[Number / 64U] |= std::uint64_t{1} << (Number % 64U);
    } while (consume(MITokenKind::Comma));
  }
  if (expect(MITokenKind::RightParen, "')' after CustomRegMask"))
    return true;
  auto Operand =
      MachineOperand::registerMask(std::make_shared<const RegisterMask>(std::move(Words)));
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseLiveOutMaskOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after liveout"))
    return true;
  std::uint32_t MaximumRegister = 0;
  for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters())
    MaximumRegister = std::max(MaximumRegister, Register.Reg.number());
  std::vector<std::uint64_t> Words(MaximumRegister / 64U + 1U, 0);
  do {
    Register Reg;
    if (!at(MITokenKind::NamedRegister) || parseRegister(Reg) || !Reg.isPhysical())
      return fail("liveout requires physical registers");
    const auto Physical = Reg.physicalRegister();
    if (!Physical)
      return fail("liveout requires physical registers");
    const std::uint32_t Number = Physical->number();
    Words[Number / 64U] |= std::uint64_t{1} << (Number % 64U);
  } while (consume(MITokenKind::Comma));
  if (expect(MITokenKind::RightParen, "')' after liveout"))
    return true;
  auto Operand =
      MachineOperand::liveOutRegisterMask(std::make_shared<const RegisterMask>(std::move(Words)));
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseBlockAddressOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  advance();
  if (expect(MITokenKind::LeftParen, "'(' after blockaddress"))
    return true;
  if (!at(MITokenKind::GlobalValue) && !at(MITokenKind::NamedGlobalValue))
    return fail("blockaddress requires a function reference");
  const std::string FunctionName = symbolName(token());
  advance();
  if (expect(MITokenKind::Comma, "',' in blockaddress"))
    return true;
  if (!at(MITokenKind::IRBlock) && !at(MITokenKind::NamedIRBlock))
    return fail("blockaddress requires an IR block reference");
  const std::string BlockName = symbolName(token());
  advance();
  if (expect(MITokenKind::RightParen, "')' after blockaddress"))
    return true;
  std::int64_t Offset = 0;
  if (parseOptionalOffset(Offset))
    return true;
  auto Operand = MachineOperand::blockAddress(FunctionName, BlockName, Offset);
  if (!Operand)
    return importError(Begin, Operand.error());
  Result = ParsedOperand{
      std::move(*Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseOperand(bool IsDef, ParsedOperand &Result) {
  const MIToken Begin = token();
  if (token().isRegister() || token().isRegisterFlag())
    return parseRegisterOperand(IsDef, Result);

  std::uint32_t TargetFlags = 0;
  if (parseTargetFlags(TargetFlags))
    return true;
  const MIToken OperandToken = token();
  if (isFloatingSemanticsKeyword(OperandToken)) {
    if (parseFloatingImmediateOperand(Result))
      return true;
    Result.Operand.setTargetFlags(TargetFlags);
    return false;
  }
  if (OperandToken.kind() == MITokenKind::Exclaim ||
      (OperandToken.kind() == MITokenKind::MetadataKeyword &&
       OperandToken.isMetadataKeyword("!DIExpression"))) {
    if (parseMetadataOperand(Result))
      return true;
    Result.Operand.setTargetFlags(TargetFlags);
    return false;
  }
  if (isCFIOperationToken(OperandToken)) {
    if (parseCFIOperand(Result))
      return true;
    Result.Operand.setTargetFlags(TargetFlags);
    return false;
  }
  MachineOperand Operand = MachineOperand::immediate(0);
  bool HasOperand = true;

  switch (OperandToken.kind()) {
  case MITokenKind::IntegerLiteral: {
    std::int64_t Value = 0;
    if (parseSigned64(OperandToken, Value))
      return fail("integer literal is too large for a machine immediate");
    Operand = MachineOperand::immediate(Value, TargetFlags);
    advance();
    break;
  }
  case MITokenKind::MachineBasicBlock: {
    MachineBasicBlock *Block = blockFor(OperandToken);
    if (Block == nullptr)
      return true;
    Operand = MachineOperand::basicBlock(*Block, TargetFlags);
    advance();
    break;
  }
  case MITokenKind::StackObject:
  case MITokenKind::FixedStackObject: {
    const bool Fixed = OperandToken.kind() == MITokenKind::FixedStackObject;
    const auto Index = frameIndexFor(OperandToken, Fixed);
    if (!Index)
      return true;
    advance();
    std::int64_t Offset = 0;
    if (parseOptionalOffset(Offset))
      return true;
    Operand = MachineOperand::frameIndex(*Index, Offset, TargetFlags);
    break;
  }
  case MITokenKind::ConstantPoolItem: {
    const auto Index = constantPoolIndexFor(OperandToken);
    if (!Index)
      return true;
    advance();
    std::int64_t Offset = 0;
    if (parseOptionalOffset(Offset))
      return true;
    Operand = MachineOperand::constantPool(*Index, Offset, TargetFlags);
    break;
  }
  case MITokenKind::JumpTableIndex: {
    const auto Index = jumpTableIndexFor(OperandToken);
    if (!Index)
      return true;
    Operand = MachineOperand::jumpTable(*Index, TargetFlags);
    advance();
    break;
  }
  case MITokenKind::NamedGlobalValue:
  case MITokenKind::GlobalValue: {
    const std::string Name = symbolName(OperandToken);
    advance();
    std::int64_t Offset = 0;
    if (parseOptionalOffset(Offset))
      return true;
    const GlobalValueProperties Properties = Context.GlobalValuePropertiesForName
                                                 ? Context.GlobalValuePropertiesForName(Name)
                                                 : GlobalValueProperties{};
    auto Created = MachineOperand::globalSymbol(Name, Offset, TargetFlags, Properties);
    if (!Created)
      return importError(OperandToken, Created.error());
    Operand = std::move(*Created);
    break;
  }
  case MITokenKind::ExternalSymbol: {
    const std::string Name = symbolName(OperandToken);
    advance();
    std::int64_t Offset = 0;
    if (parseOptionalOffset(Offset))
      return true;
    auto Created = MachineOperand::externalSymbol(Name, Offset, TargetFlags);
    if (!Created)
      return importError(OperandToken, Created.error());
    Operand = std::move(*Created);
    break;
  }
  case MITokenKind::MCSymbol: {
    const std::string Name = symbolName(OperandToken);
    advance();
    std::int64_t Offset = 0;
    if (parseOptionalOffset(Offset))
      return true;
    auto Created = MachineOperand::mcSymbol(Name, Offset, TargetFlags);
    if (!Created)
      return importError(OperandToken, Created.error());
    Operand = std::move(*Created);
    break;
  }
  case MITokenKind::SubRegisterIndex: {
    const SubRegIndexDesc *Description = Target.registers().subRegIndex(OperandToken.stringValue());
    if (Description == nullptr)
      return fail("unknown subregister index '" + std::string(OperandToken.stringValue()) + "'");
    Operand =
        MachineOperand::immediate(static_cast<std::int64_t>(Description->ID.value()), TargetFlags);
    advance();
    break;
  }
  case MITokenKind::Identifier: {
    if (const auto Mask = Target.mirText().registerMask(OperandToken.stringValue())) {
      auto Created = MachineOperand::registerMask(Mask, TargetFlags);
      if (!Created)
        return importError(OperandToken, Created.error());
      Operand = std::move(*Created);
      advance();
    } else if (OperandToken.stringValue() == "CustomRegMask") {
      if (TargetFlags != 0)
        return fail("custom register masks cannot have target flags");
      return parseCustomMaskOperand(Result);
    } else if (OperandToken.stringValue().starts_with('i')) {
      if (parseTypedIntegerOperand(Result))
        return true;
      Result.Operand.setTargetFlags(TargetFlags);
      return false;
    } else {
      return fail("unknown named machine operand '" + std::string(OperandToken.stringValue()) +
                  "'");
    }
    break;
  }
  case MITokenKind::Keyword:
    HasOperand = false;
    break;
  default:
    return fail("expected a machine operand");
  }

  if (!HasOperand) {
    bool Failed = false;
    if (OperandToken.isKeyword("intpred"))
      Failed = parsePredicateOperand(Result, false);
    else if (OperandToken.isKeyword("floatpred"))
      Failed = parsePredicateOperand(Result, true);
    else if (OperandToken.isKeyword("shufflemask"))
      Failed = parseShuffleMaskOperand(Result);
    else if (OperandToken.isKeyword("dbg-instr-ref"))
      Failed = parseDebugInstrRefOperand(Result);
    else if (OperandToken.isKeyword("lanemask"))
      Failed = parseLaneMaskOperand(Result);
    else if (OperandToken.isKeyword("intrinsic"))
      Failed = parseIntrinsicOperand(Result);
    else if (OperandToken.isKeyword("target-index"))
      Failed = parseTargetIndexOperand(Result);
    else if (OperandToken.isKeyword("liveout"))
      Failed = parseLiveOutMaskOperand(Result);
    else if (OperandToken.isKeyword("blockaddress"))
      Failed = parseBlockAddressOperand(Result);
    else
      return fail("unsupported machine operand spelling '" + std::string(OperandToken.spelling()) +
                  "'");
    if (!Failed)
      Result.Operand.setTargetFlags(TargetFlags);
    return Failed;
  }

  Result =
      ParsedOperand{std::move(Operand), std::nullopt, {Begin.range().Begin, token().range().Begin}};
  return false;
}

bool MIParser::Impl::parseInlineAsmExtraInfoOperand(ParsedOperand &Result) {
  const MIToken Begin = token();
  InlineAsmExtraInfo ExtraInfo;
  while (at(MITokenKind::Identifier)) {
    const auto Bits = InlineAsmExtraInfo::namedFlagBits(token().stringValue());
    if (!Bits) {
      return fail("unknown inline asm extra info flag '" + std::string(token().stringValue()) +
                  "'");
    }
    ExtraInfo = InlineAsmExtraInfo(ExtraInfo.rawValue() | *Bits);
    advance();
  }
  Result = ParsedOperand{
      MachineOperand::immediate(static_cast<std::int64_t>(ExtraInfo.rawValue())),
      std::nullopt,
      {Begin.range().Begin, token().range().Begin},
  };
  return false;
}

bool MIParser::Impl::parseInlineAsmOperandFlag(ParsedOperand &Result) {
  const MIToken Begin = token();
  const std::string KindName(token().stringValue());
  const auto Kind = InlineAsmOperandFlag::kindForName(KindName);
  if (!Kind)
    return fail("unknown inline asm operand kind '" + KindName + "'");
  advance();

  InlineAsmOperandFlag Flag(*Kind);
  if (at(MITokenKind::Identifier) && token().stringValue() == "tiedto") {
    advance();
    if (!consume(MITokenKind::Colon))
      return fail("expected ':' after 'tiedto'");
    if (!at(MITokenKind::NamedRegister))
      return fail("expected '$N' operand number after 'tiedto:'");
    std::uint64_t OperandNumber = 0;
    if (!parseUnsignedDecimal(token().stringValue(), OperandNumber) ||
        OperandNumber > std::numeric_limits<std::uint32_t>::max() ||
        !Flag.setMatchingOperand(static_cast<std::uint32_t>(OperandNumber))) {
      return fail("invalid operand number in tiedto constraint");
    }
    advance();
  } else if (consume(MITokenKind::Colon)) {
    if (!at(MITokenKind::Identifier))
      return fail("expected register class or memory constraint name after ':'");
    const std::string Constraint(token().stringValue());
    if (*Kind == InlineAsmOperandKind::Memory) {
      const auto MemoryConstraint = InlineAsmOperandFlag::memoryConstraintForName(Constraint);
      if (!MemoryConstraint)
        return fail("unknown memory constraint '" + Constraint + "'");
      if (!Flag.setMemoryConstraint(*MemoryConstraint))
        return fail("invalid memory constraint encoding");
    } else if (*Kind == InlineAsmOperandKind::RegisterDef ||
               *Kind == InlineAsmOperandKind::RegisterUse ||
               *Kind == InlineAsmOperandKind::RegisterDefEarlyClobber) {
      const RegisterClassDesc *RegisterClass = Target.registers().registerClass(Constraint);
      if (RegisterClass == nullptr)
        return fail("unknown register class '" + Constraint + "'");
      if (!Flag.setRegisterClass(RegisterClass->ID.value()))
        return fail("register class cannot be encoded in an inline asm operand flag");
    }
    advance();
  }

  Result = ParsedOperand{
      MachineOperand::immediate(static_cast<std::int64_t>(Flag.rawValue())),
      std::nullopt,
      {Begin.range().Begin, token().range().Begin},
  };
  return false;
}

bool MIParser::Impl::parseBlockLiveIns(MachineBasicBlock &Block) {
  advance();
  if (expect(MITokenKind::Colon, "':' after liveins"))
    return true;
  if (token().isNewlineOrEnd())
    return false;
  do {
    if (!at(MITokenKind::NamedRegister))
      return fail("expected a physical register in liveins");
    Register Reg;
    if (parseRegister(Reg) || !Reg.isPhysical())
      return true;
    const auto Physical = Reg.physicalRegister();
    if (!Physical)
      return fail("expected a physical register in liveins");
    LaneMask Lanes = LaneMask::all();
    if (consume(MITokenKind::Colon)) {
      std::uint64_t Raw = 0;
      if (parseUnsigned(token(), Raw))
        return fail("invalid live-in lane mask");
      Lanes = LaneMask::fromRaw(Raw);
      advance();
    }
    auto Added = Block.addLiveIn(*Physical, Lanes);
    if (!Added)
      return importError(token(), Added.error());
  } while (consume(MITokenKind::Comma));
  return false;
}

bool MIParser::Impl::parseBlockSuccessors(MachineBasicBlock &Block) {
  advance();
  if (expect(MITokenKind::Colon, "':' after successors"))
    return true;
  ExplicitSuccessorBlocks.insert(&Block);
  if (token().isNewlineOrEnd())
    return false;

  std::vector<MachineBasicBlock *> Successors;
  std::vector<BranchProbability> Probabilities;
  do {
    if (!at(MITokenKind::MachineBasicBlock))
      return fail("expected a machine basic block reference");
    MachineBasicBlock *Successor = blockFor(token());
    if (Successor == nullptr)
      return true;
    advance();
    BranchProbability Probability = BranchProbability::zero();
    if (consume(MITokenKind::LeftParen)) {
      std::uint32_t Weight = 0;
      if (parseUnsigned32(token(), Weight))
        return fail("expected unsigned integer");
      Probability = BranchProbability::fromRaw(Weight);
      advance();
      if (expect(MITokenKind::RightParen, "')' after successor weight"))
        return true;
    }
    Successors.push_back(Successor);
    Probabilities.push_back(Probability);
  } while (consume(MITokenKind::Comma));

  BranchProbability::normalizeProbabilities(Probabilities);
  for (std::size_t Index = 0; Index < Successors.size(); ++Index) {
    auto Added =
        Function.cfgUpdater().addSuccessor(Block, *Successors[Index], Probabilities[Index]);
    if (!Added)
      return importError(token(), Added.error());
  }
  return false;
}

bool MIParser::Impl::parseInstruction(MachineBasicBlock &Block, bool InBundle,
                                      MachineInstr *PreviousInstruction, MachineInstr *&Result) {
  if (ParsedInstructionCount >= Options.MaximumInstructions)
    return fail("machine instruction count exceeds parser limit");

  std::vector<ParsedOperand> Operands;
  while (token().isRegister() || token().isRegisterFlag()) {
    ParsedOperand Operand{MachineOperand::immediate(0), std::nullopt, {}};
    if (parseRegisterOperand(true, Operand))
      return true;
    Operands.push_back(std::move(Operand));
    if (!consume(MITokenKind::Comma))
      break;
  }
  if (!Operands.empty() && expect(MITokenKind::Equal, "'=' after register definitions"))
    return true;

  MachineInstrFlags Flags;
  while (isInstructionFlag(token())) {
    const auto Flag = instructionFlag(token().stringValue());
    if (!Flag)
      return fail("unknown instruction flag");
    if (Flags.has(*Flag))
      return fail("duplicate instruction flag '" + std::string(token().stringValue()) + "'");
    Flags.set(*Flag);
    advance();
  }
  if (!at(MITokenKind::Identifier))
    return fail("expected a machine instruction name");
  const MIToken OpcodeToken = token();
  const InstructionDescriptor *Descriptor =
      Target.instructions().descriptor(OpcodeToken.stringValue());
  if (Descriptor == nullptr)
    return fail("unknown machine instruction '" + std::string(OpcodeToken.stringValue()) + "'");
  advance();
  const bool IsInlineAsm = Descriptor->Name == "INLINEASM" || Descriptor->Name == "INLINEASM_BR";

  while (!token().isNewlineOrEnd() && !at(MITokenKind::DoubleColon) &&
         !at(MITokenKind::LeftBrace) && !atKeyword("debug-instr-number") &&
         !atKeyword("debug-location") && !atKeyword("pre-instr-symbol") &&
         !atKeyword("post-instr-symbol") && !atKeyword("heap-alloc-marker") &&
         !atKeyword("pcsections") && !atKeyword("mmra") && !atKeyword("cfi-type") &&
         !atKeyword("deactivation-symbol")) {
    if (Operands.size() >= Options.MaximumOperandsPerInstruction)
      return fail("instruction operand count exceeds parser limit");
    ParsedOperand Operand{MachineOperand::immediate(0), std::nullopt, {}};
    const bool IsInlineAsmExtraInfo = IsInlineAsm &&
                                      Operands.size() == InlineAsmExtraInfoOperandIndex &&
                                      at(MITokenKind::Identifier);
    const bool IsInlineAsmOperandFlag = IsInlineAsm &&
                                        Operands.size() >= InlineAsmFirstConstraintOperandIndex &&
                                        at(MITokenKind::Identifier);
    if (IsInlineAsmExtraInfo ? parseInlineAsmExtraInfoOperand(Operand)
                             : (IsInlineAsmOperandFlag ? parseInlineAsmOperandFlag(Operand)
                                                       : parseOperand(false, Operand)))
      return true;
    Operands.push_back(std::move(Operand));
    if (token().isNewlineOrEnd() || at(MITokenKind::DoubleColon) || at(MITokenKind::LeftBrace) ||
        atKeyword("debug-instr-number") || atKeyword("debug-location"))
      break;
    if (expect(MITokenKind::Comma, "',' before the next machine operand"))
      return true;
  }

  std::uint32_t DebugNumber = 0;
  std::optional<MachineDebugLocation> DebugLocation;
  std::optional<std::string> PreInstrSymbol;
  std::optional<std::string> PostInstrSymbol;
  std::optional<MachineMetadataReference> HeapAllocMarker;
  std::optional<MachineMetadataReference> PCSections;
  std::optional<MachineMetadataReference> MMRA;
  std::optional<std::uint32_t> CFIType;
  std::optional<std::string> DeactivationSymbol;
  std::unordered_set<std::string> SeenAttachments;
  while (atKeyword("debug-instr-number") || atKeyword("debug-location") ||
         atKeyword("pre-instr-symbol") || atKeyword("post-instr-symbol") ||
         atKeyword("heap-alloc-marker") || atKeyword("pcsections") || atKeyword("mmra") ||
         atKeyword("cfi-type") || atKeyword("deactivation-symbol")) {
    const std::string Name(token().stringValue());
    if (!SeenAttachments.insert(Name).second)
      return fail("duplicate instruction attachment '" + Name + "'");
    advance();
    if (Name == "debug-instr-number") {
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), DebugNumber) ||
          DebugNumber == 0) {
        return fail("invalid debug instruction number");
      }
      advance();
    } else if (Name == "debug-location") {
      if (at(MITokenKind::MetadataKeyword) && token().isMetadataKeyword("!DILocation")) {
        std::vector<MachineDILocationFrame> InlineChain;
        std::optional<MachineMetadataReference> TerminalInlinedAt;
        if (parseInlineDILocation(InlineChain, TerminalInlinedAt))
          return true;
        auto Location = MachineDILocation::create(std::move(InlineChain), TerminalInlinedAt);
        if (!Location)
          return importError(OpcodeToken, Location.error());
        DebugLocation = MachineDebugLocation::inlineLocation(std::move(*Location));
      } else {
        MachineMetadataReference Reference;
        if (parseMetadataReference(Reference))
          return true;
        auto Created = MachineDebugLocation::reference(Reference);
        if (!Created)
          return importError(OpcodeToken, Created.error());
        DebugLocation = std::move(*Created);
      }
    } else if (Name == "pre-instr-symbol" || Name == "post-instr-symbol") {
      if (!at(MITokenKind::MCSymbol))
        return fail("expected an MC symbol after '" + Name + "'");
      if (Name == "pre-instr-symbol")
        PreInstrSymbol = symbolName(token());
      else
        PostInstrSymbol = symbolName(token());
      advance();
    } else if (Name == "heap-alloc-marker" || Name == "pcsections" || Name == "mmra") {
      MachineMetadataReference Reference;
      if (parseMetadataReference(Reference))
        return true;
      if (Name == "heap-alloc-marker")
        HeapAllocMarker = Reference;
      else if (Name == "pcsections")
        PCSections = Reference;
      else
        MMRA = Reference;
    } else if (Name == "cfi-type") {
      std::uint32_t Type = 0;
      if (!at(MITokenKind::IntegerLiteral) || parseUnsigned32(token(), Type) || Type == 0)
        return fail("invalid nonzero CFI type");
      CFIType = Type;
      advance();
    } else {
      if (!at(MITokenKind::GlobalValue) && !at(MITokenKind::NamedGlobalValue))
        return fail("expected a global value after 'deactivation-symbol'");
      DeactivationSymbol = symbolName(token());
      advance();
    }
    static_cast<void>(consume(MITokenKind::Comma));
  }
  std::vector<const MachineMemOperand *> MemOperands;
  if (consume(MITokenKind::DoubleColon)) {
    while (!token().isNewlineOrEnd() && !at(MITokenKind::LeftBrace)) {
      if (MemOperands.size() >= Options.MaximumMemoryOperandsPerInstruction)
        return fail("machine memory operand count exceeds parser limit");
      const MachineMemOperand *MemOperand = nullptr;
      if (parseMemoryOperand(MemOperand, Descriptor->InstructionOpcode))
        return true;
      MemOperands.push_back(MemOperand);
      if (token().isNewlineOrEnd() || at(MITokenKind::LeftBrace))
        break;
      if (expect(MITokenKind::Comma, "',' before the next machine memory operand"))
        return true;
    }
  }

  auto Created = Function.createInstruction(Descriptor->InstructionOpcode);
  if (!Created)
    return importError(OpcodeToken, Created.error());
  MachineInstr *Instruction = *Created;
  Instruction->setFlags(Flags);
  for (ParsedOperand &Parsed : Operands) {
    auto Added = Instruction->addOperand(std::move(Parsed.Operand));
    if (!Added)
      return importError(OpcodeToken, Added.error());
  }
  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    if (!Operands[Index].TiedDef)
      continue;
    if (*Operands[Index].TiedDef >= Operands.size())
      return fail(OpcodeToken, "tied-def operand index is out of range");
    auto Tied = Instruction->tieOperands(*Operands[Index].TiedDef, Index);
    if (!Tied)
      return importError(OpcodeToken, Tied.error());
  }
  for (const MachineMemOperand *MemOperand : MemOperands) {
    auto Added = Instruction->addMemOperand(*MemOperand);
    if (!Added)
      return importError(OpcodeToken, Added.error());
  }
  if (DebugNumber != 0) {
    auto SetNumber = Instruction->setDebugInstrNumber(DebugNumber);
    if (!SetNumber)
      return importError(OpcodeToken, SetNumber.error());
  }
  if (DebugLocation) {
    auto SetLocation = Instruction->setDebugLocation(std::move(*DebugLocation));
    if (!SetLocation)
      return importError(OpcodeToken, SetLocation.error());
  }
  if (PreInstrSymbol) {
    auto SetSymbol = Instruction->setPreInstrSymbol(std::move(*PreInstrSymbol));
    if (!SetSymbol)
      return importError(OpcodeToken, SetSymbol.error());
  }
  if (PostInstrSymbol) {
    auto SetSymbol = Instruction->setPostInstrSymbol(std::move(*PostInstrSymbol));
    if (!SetSymbol)
      return importError(OpcodeToken, SetSymbol.error());
  }
  if (HeapAllocMarker) {
    auto SetMarker = Instruction->setHeapAllocMarker(*HeapAllocMarker);
    if (!SetMarker)
      return importError(OpcodeToken, SetMarker.error());
  }
  if (PCSections) {
    auto SetSections = Instruction->setPCSections(*PCSections);
    if (!SetSections)
      return importError(OpcodeToken, SetSections.error());
  }
  if (MMRA) {
    auto SetMMRA = Instruction->setMMRA(*MMRA);
    if (!SetMMRA)
      return importError(OpcodeToken, SetMMRA.error());
  }
  if (CFIType) {
    auto SetType = Instruction->setCFIType(*CFIType);
    if (!SetType)
      return importError(OpcodeToken, SetType.error());
  }
  if (DeactivationSymbol) {
    auto SetSymbol = Instruction->setDeactivationSymbol(std::move(*DeactivationSymbol));
    if (!SetSymbol)
      return importError(OpcodeToken, SetSymbol.error());
  }
  if (InBundle) {
    Flags.set(MachineInstrFlag::BundledWithPred);
    Instruction->setFlags(Flags);
    if (PreviousInstruction != nullptr) {
      MachineInstrFlags PreviousFlags = PreviousInstruction->flags();
      PreviousFlags.set(MachineInstrFlag::BundledWithSucc);
      PreviousInstruction->setFlags(PreviousFlags);
    }
  }
  auto Appended = Block.appendInstruction(*Instruction);
  if (!Appended)
    return importError(OpcodeToken, Appended.error());

  ++ParsedInstructionCount;
  Result = Instruction;
  return false;
}

bool MIParser::Impl::parseSecondPass() {
  Position = 0;
  while (consume(MITokenKind::Newline)) {
  }
  while (!at(MITokenKind::EndOfFile)) {
    if (!at(MITokenKind::MachineBasicBlockLabel))
      return fail("expected a machine basic block definition");
    std::uint32_t ID = 0;
    if (parseBlockDefinition(false, ID))
      return true;
    MachineBasicBlock *Block = Blocks.at(ID);

    while (consume(MITokenKind::Newline)) {
    }
    while (atKeyword("liveins") || atKeyword("successors")) {
      if ((atKeyword("liveins") && parseBlockLiveIns(*Block)) ||
          (atKeyword("successors") && parseBlockSuccessors(*Block))) {
        return true;
      }
      if (!token().isNewlineOrEnd())
        return fail("expected a line break after the block list");
      while (consume(MITokenKind::Newline)) {
      }
    }

    bool InBundle = false;
    MachineInstr *PreviousInstruction = nullptr;
    while (!at(MITokenKind::MachineBasicBlockLabel) && !at(MITokenKind::EndOfFile)) {
      if (consume(MITokenKind::Newline))
        continue;
      if (consume(MITokenKind::RightBrace)) {
        if (!InBundle)
          return fail("closing brace without an active instruction bundle");
        InBundle = false;
        continue;
      }

      MachineInstr *Instruction = nullptr;
      if (parseInstruction(*Block, InBundle, PreviousInstruction, Instruction))
        return true;
      PreviousInstruction = Instruction;
      if (consume(MITokenKind::LeftBrace)) {
        if (InBundle)
          return fail("nested instruction bundles are not allowed");
        MachineInstrFlags BundleFlags = Instruction->flags();
        BundleFlags.set(MachineInstrFlag::BundledWithSucc);
        Instruction->setFlags(BundleFlags);
        InBundle = true;
      }
      if (!token().isNewlineOrEnd() && !at(MITokenKind::RightBrace))
        return fail("expected a line break after the machine instruction");
    }
    if (InBundle)
      return fail("unterminated instruction bundle");
  }
  return false;
}

bool MIParser::Impl::inferSuccessors() {
  for (MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr; Block = Block->next()) {
    if (ExplicitSuccessorBlocks.contains(Block))
      continue;

    std::vector<MachineBasicBlock *> Successors;
    bool BlocksFallthrough = false;
    auto Guessed = guessSuccessors(*Block, Target, Successors, BlocksFallthrough);
    if (!Guessed)
      return importError(token(), Guessed.error());
    if (BlocksFallthrough && Block->next() != nullptr &&
        std::ranges::find(Successors, Block->next()) == Successors.end())
      Successors.push_back(Block->next());
    if (Successors.empty())
      continue;
    const auto Probability =
        BranchProbability::fromFraction(1U, static_cast<std::uint32_t>(Successors.size()));
    if (!Probability)
      return importError(token(), Probability.error());
    for (MachineBasicBlock *Successor : Successors) {
      auto Added = Function.cfgUpdater().addSuccessor(*Block, *Successor, *Probability);
      if (!Added)
        return importError(token(), Added.error());
    }
  }
  return false;
}

Expected<void> MIParser::Impl::parse() {
  if (ParseAttempted)
    return makeUnexpected(ErrorCode::InvalidArgument, "MI parser can only be run once");
  ParseAttempted = true;
  if (Function.blockCount() != 0 || !Function.allInstructions().empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "MI parser requires a staging machine function with an empty body");
  }
  for (const MIVirtualRegisterSlot &Slot : Context.VirtualRegisters) {
    if (!Slot.Reg.isVirtual() || !Function.registerInfo().isKnownVirtualRegister(Slot.Reg)) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "MI parser virtual-register slot maps to an unknown register");
    }
    if (!NumericVirtualRegisters.emplace(Slot.TextID, Slot.Reg).second) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "MI parser virtual-register text IDs must be unique");
    }
  }
  if (tokenize())
    return failureResult();
  if (parseFirstPass() || parseSecondPass() || inferSuccessors())
    return failureResult();

  Function.renumberBlocks();
  if (Options.VerifyAfterParse) {
    auto Verified =
        MachineVerifier::verify(Function, Target,
                                MachineVerifierOptions{.Mode = MachineVerifierMode::Generic,
                                                       .Stage = std::nullopt,
                                                       .VerifyProperties = false});
    if (!Verified) {
      static_cast<void>(fail(token(), "parsed machine function failed verification: " +
                                          std::string(Verified.error().message())));
      return failureResult();
    }
  }
  return {};
}

MIParser::MIParser(MachineFunction &Function, const TargetDescription &Target,
                   std::string SourceName, std::string SourceText, MIParserContext Context,
                   MIParserOptions Options)
    : Implementation(std::make_unique<Impl>(Function, Target, std::move(SourceName),
                                            std::move(SourceText), std::move(Context), Options)) {}

MIParser::MIParser(MIParser &&) noexcept = default;

MIParser &MIParser::operator=(MIParser &&) noexcept = default;

MIParser::~MIParser() = default;

Expected<void> MIParser::parse() {
  return Implementation->parse();
}

const TextSource &MIParser::source() const noexcept {
  return Implementation->source();
}

std::span<const TextDiagnostic> MIParser::diagnostics() const noexcept {
  return Implementation->diagnostics();
}

bool MIParser::failed() const noexcept {
  return Implementation->failed();
}

} // namespace rcc::mir
