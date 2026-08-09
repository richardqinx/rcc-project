// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCInstPrinter.h"

#include "rcc/MC/MCExpr.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rcc::mc {
namespace {

[[nodiscard]] bool isBareSymbolName(std::string_view Name) noexcept {
  if (Name.empty())
    return false;
  const auto Initial = [](unsigned char Character) {
    return std::isalpha(Character) != 0 || Character == '_' || Character == '.' || Character == '$';
  };
  const auto Following = [](unsigned char Character) {
    return std::isalnum(Character) != 0 || Character == '_' || Character == '.' || Character == '$';
  };
  if (!Initial(static_cast<unsigned char>(Name.front())))
    return false;
  for (const char Character : Name.substr(1)) {
    if (!Following(static_cast<unsigned char>(Character)))
      return false;
  }
  return true;
}

[[nodiscard]] std::string_view unarySpelling(MCUnaryOperator Operator) noexcept {
  switch (Operator) {
  case MCUnaryOperator::Plus:
    return "+";
  case MCUnaryOperator::Minus:
    return "-";
  case MCUnaryOperator::BitwiseNot:
    return "~";
  case MCUnaryOperator::LogicalNot:
    return "!";
  }
  return {};
}

[[nodiscard]] std::string_view binarySpelling(MCBinaryOperator Operator) noexcept {
  switch (Operator) {
  case MCBinaryOperator::Add:
    return "+";
  case MCBinaryOperator::Subtract:
    return "-";
  case MCBinaryOperator::Multiply:
    return "*";
  case MCBinaryOperator::Divide:
    return "/";
  case MCBinaryOperator::Modulo:
    return "%";
  case MCBinaryOperator::ShiftLeft:
    return "<<";
  case MCBinaryOperator::ShiftRight:
    return ">>";
  case MCBinaryOperator::BitwiseAnd:
    return "&";
  case MCBinaryOperator::BitwiseOr:
    return "|";
  case MCBinaryOperator::BitwiseXor:
    return "^";
  case MCBinaryOperator::LogicalAnd:
    return "&&";
  case MCBinaryOperator::LogicalOr:
    return "||";
  case MCBinaryOperator::Equal:
    return "==";
  case MCBinaryOperator::NotEqual:
    return "!=";
  case MCBinaryOperator::Less:
    return "<";
  case MCBinaryOperator::LessEqual:
    return "<=";
  case MCBinaryOperator::Greater:
    return ">";
  case MCBinaryOperator::GreaterEqual:
    return ">=";
  }
  return {};
}

[[nodiscard]] Expected<std::string>
printExpressionImpl(const MCExpr &Expression, const MCInstPrinter &Printer, std::size_t Depth) {
  if (Depth > 256)
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "MC expression exceeds the assembly printing depth limit");

  switch (Expression.kind()) {
  case MCExpr::Kind::Constant:
    return std::to_string(*Expression.getIf<std::int64_t>());
  case MCExpr::Kind::SymbolRef: {
    const auto *Reference = Expression.getIf<MCSymbolRefExpr>();
    if (Reference == nullptr || !Reference->Symbol)
      return makeUnexpected(ErrorCode::InternalError, "MC symbol expression storage is invalid");
    return printMCSymbolName(Reference->Symbol->name());
  }
  case MCExpr::Kind::Unary: {
    const auto *Unary = Expression.getIf<MCUnaryExpr>();
    if (Unary == nullptr || !Unary->Operand)
      return makeUnexpected(ErrorCode::InternalError, "MC unary expression storage is invalid");
    auto Operand = printExpressionImpl(*Unary->Operand, Printer, Depth + 1);
    if (!Operand)
      return std::unexpected(Operand.error());
    return std::string(unarySpelling(Unary->Operator)) + "(" + *Operand + ")";
  }
  case MCExpr::Kind::Binary: {
    const auto *Binary = Expression.getIf<MCBinaryExpr>();
    if (Binary == nullptr || !Binary->Left || !Binary->Right)
      return makeUnexpected(ErrorCode::InternalError, "MC binary expression storage is invalid");
    auto Left = printExpressionImpl(*Binary->Left, Printer, Depth + 1);
    if (!Left)
      return std::unexpected(Left.error());
    auto Right = printExpressionImpl(*Binary->Right, Printer, Depth + 1);
    if (!Right)
      return std::unexpected(Right.error());
    return "(" + *Left + std::string(binarySpelling(Binary->Operator)) + *Right + ")";
  }
  case MCExpr::Kind::Target: {
    const auto *Target = Expression.getIf<MCTargetExpr>();
    if (Target == nullptr || !Target->Operand)
      return makeUnexpected(ErrorCode::InternalError, "target MC expression storage is invalid");
    auto Operand = printExpressionImpl(*Target->Operand, Printer, Depth + 1);
    if (!Operand)
      return std::unexpected(Operand.error());
    return Printer.printTargetExpression(Target->Kind, *Operand);
  }
  }
  return makeUnexpected(ErrorCode::InternalError, "MC expression kind is invalid");
}

} // namespace

Expected<std::string> printMCSymbolName(std::string_view Name) {
  if (Name.empty())
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "MC symbol name cannot be empty");
  if (isBareSymbolName(Name))
    return std::string(Name);

  std::string Result;
  Result.push_back('"');
  for (const char RawCharacter : Name) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (Character == 0 || Character == '\n' || Character == '\r' || Character == 0x7fU) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "MC symbol name contains a non-printable character");
    }
    if (Character == '"' || Character == '\\')
      Result.push_back('\\');
    Result.push_back(static_cast<char>(Character));
  }
  Result.push_back('"');
  return Result;
}

Expected<std::string> printMCExpression(const MCExpr &Expression, const MCInstPrinter &Printer) {
  auto Verification = Expression.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return printExpressionImpl(Expression, Printer, 1);
}

} // namespace rcc::mc
