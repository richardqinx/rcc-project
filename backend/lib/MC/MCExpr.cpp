// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCExpr.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace rcc::mc {
namespace {

[[nodiscard]] std::optional<std::int64_t> checkedUnary(MCUnaryOperator Operator,
                                                       std::int64_t Value) noexcept {
  switch (Operator) {
  case MCUnaryOperator::Plus:
    return Value;
  case MCUnaryOperator::Minus:
    if (Value == std::numeric_limits<std::int64_t>::min())
      return std::nullopt;
    return -Value;
  case MCUnaryOperator::BitwiseNot:
    return ~Value;
  case MCUnaryOperator::LogicalNot:
    return Value == 0 ? 1 : 0;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t>
checkedBinary(MCBinaryOperator Operator, std::int64_t Left, std::int64_t Right) noexcept {
  std::int64_t Result = 0;
  switch (Operator) {
  case MCBinaryOperator::Add:
    return __builtin_add_overflow(Left, Right, &Result) ? std::nullopt : std::optional(Result);
  case MCBinaryOperator::Subtract:
    return __builtin_sub_overflow(Left, Right, &Result) ? std::nullopt : std::optional(Result);
  case MCBinaryOperator::Multiply:
    return __builtin_mul_overflow(Left, Right, &Result) ? std::nullopt : std::optional(Result);
  case MCBinaryOperator::Divide:
    if (Right == 0 || (Left == std::numeric_limits<std::int64_t>::min() && Right == -1))
      return std::nullopt;
    return Left / Right;
  case MCBinaryOperator::Modulo:
    if (Right == 0 || (Left == std::numeric_limits<std::int64_t>::min() && Right == -1))
      return std::nullopt;
    return Left % Right;
  case MCBinaryOperator::ShiftLeft:
    if (Right < 0 || Right >= 64)
      return std::nullopt;
    if (Right == 63) {
      if (Left == 0)
        return 0;
      return Left == -1 ? std::optional(std::numeric_limits<std::int64_t>::min()) : std::nullopt;
    }
    if (__builtin_mul_overflow(Left, std::int64_t{1} << Right, &Result))
      return std::nullopt;
    return Result;
  case MCBinaryOperator::ShiftRight:
    if (Right < 0 || Right >= 64)
      return std::nullopt;
    return Left >> Right;
  case MCBinaryOperator::BitwiseAnd:
    return Left & Right;
  case MCBinaryOperator::BitwiseOr:
    return Left | Right;
  case MCBinaryOperator::BitwiseXor:
    return Left ^ Right;
  case MCBinaryOperator::LogicalAnd:
    return Left != 0 && Right != 0 ? 1 : 0;
  case MCBinaryOperator::LogicalOr:
    return Left != 0 || Right != 0 ? 1 : 0;
  case MCBinaryOperator::Equal:
    return Left == Right ? 1 : 0;
  case MCBinaryOperator::NotEqual:
    return Left != Right ? 1 : 0;
  case MCBinaryOperator::Less:
    return Left < Right ? 1 : 0;
  case MCBinaryOperator::LessEqual:
    return Left <= Right ? 1 : 0;
  case MCBinaryOperator::Greater:
    return Left > Right ? 1 : 0;
  case MCBinaryOperator::GreaterEqual:
    return Left >= Right ? 1 : 0;
  }
  return std::nullopt;
}

[[nodiscard]] Expected<void> verifyExpression(const MCExpr &Expression, std::size_t Depth,
                                              std::size_t MaximumDepth) {
  if (Depth > MaximumDepth)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC expression exceeds its depth limit");

  switch (Expression.kind()) {
  case MCExpr::Kind::Constant:
    if (Expression.getIf<std::int64_t>() == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "MC constant expression storage is invalid");
    return {};
  case MCExpr::Kind::SymbolRef: {
    const auto *Reference = Expression.getIf<MCSymbolRefExpr>();
    if (Reference == nullptr || !Reference->Symbol || Reference->Symbol->name().empty()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "MC symbol expression requires a valid symbol");
    }
    return {};
  }
  case MCExpr::Kind::Unary: {
    const auto *Unary = Expression.getIf<MCUnaryExpr>();
    if (Unary == nullptr || !Unary->Operand)
      return makeUnexpected(ErrorCode::InvalidArgument, "MC unary expression has no operand");
    return verifyExpression(*Unary->Operand, Depth + 1, MaximumDepth);
  }
  case MCExpr::Kind::Binary: {
    const auto *Binary = Expression.getIf<MCBinaryExpr>();
    if (Binary == nullptr || !Binary->Left || !Binary->Right)
      return makeUnexpected(ErrorCode::InvalidArgument, "MC binary expression has a null operand");
    auto Left = verifyExpression(*Binary->Left, Depth + 1, MaximumDepth);
    if (!Left)
      return std::unexpected(Left.error());
    return verifyExpression(*Binary->Right, Depth + 1, MaximumDepth);
  }
  case MCExpr::Kind::Target: {
    const auto *Target = Expression.getIf<MCTargetExpr>();
    if (Target == nullptr || Target->Kind == 0 || !Target->Operand) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "target MC expression requires a nonzero kind and an operand");
    }
    return verifyExpression(*Target->Operand, Depth + 1, MaximumDepth);
  }
  }
  return makeUnexpected(ErrorCode::InternalError, "MC expression kind is invalid");
}

} // namespace

MCExprRef MCExpr::constant(std::int64_t Value) {
  return std::shared_ptr<const MCExpr>(new MCExpr(Kind::Constant, Value));
}

Expected<MCExprRef> MCExpr::symbol(MCSymbolRef Symbol) {
  if (!Symbol || Symbol->name().empty())
    return makeUnexpected(ErrorCode::InvalidArgument, "MC symbol expression requires a symbol");
  return std::shared_ptr<const MCExpr>(
      new MCExpr(Kind::SymbolRef, MCSymbolRefExpr{.Symbol = std::move(Symbol)}));
}

Expected<MCExprRef> MCExpr::unary(MCUnaryOperator Operator, MCExprRef Operand) {
  if (!Operand)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC unary expression requires an operand");
  return std::shared_ptr<const MCExpr>(
      new MCExpr(Kind::Unary, MCUnaryExpr{.Operator = Operator, .Operand = std::move(Operand)}));
}

Expected<MCExprRef> MCExpr::binary(MCBinaryOperator Operator, MCExprRef Left, MCExprRef Right) {
  if (!Left || !Right)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC binary expression requires two operands");
  return std::shared_ptr<const MCExpr>(new MCExpr(
      Kind::Binary,
      MCBinaryExpr{.Operator = Operator, .Left = std::move(Left), .Right = std::move(Right)}));
}

Expected<MCExprRef> MCExpr::target(std::uint32_t Kind, MCExprRef Operand) {
  if (Kind == 0 || !Operand) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target MC expression requires a nonzero kind and an operand");
  }
  return std::shared_ptr<const MCExpr>(
      new MCExpr(MCExpr::Kind::Target, MCTargetExpr{.Kind = Kind, .Operand = std::move(Operand)}));
}

Expected<void> MCExpr::verify(std::size_t MaximumDepth) const {
  if (MaximumDepth == 0)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC expression depth limit cannot be zero");
  return verifyExpression(*this, 1, MaximumDepth);
}

std::optional<std::int64_t> MCExpr::evaluateAbsolute() const noexcept {
  switch (ExpressionKind) {
  case Kind::Constant:
    return *getIf<std::int64_t>();
  case Kind::SymbolRef:
  case Kind::Target:
    return std::nullopt;
  case Kind::Unary: {
    const auto *Unary = getIf<MCUnaryExpr>();
    if (Unary == nullptr || !Unary->Operand)
      return std::nullopt;
    const auto Value = Unary->Operand->evaluateAbsolute();
    return Value ? checkedUnary(Unary->Operator, *Value) : std::nullopt;
  }
  case Kind::Binary: {
    const auto *Binary = getIf<MCBinaryExpr>();
    if (Binary == nullptr || !Binary->Left || !Binary->Right)
      return std::nullopt;
    const auto Left = Binary->Left->evaluateAbsolute();
    const auto Right = Binary->Right->evaluateAbsolute();
    return Left && Right ? checkedBinary(Binary->Operator, *Left, *Right) : std::nullopt;
  }
  }
  return std::nullopt;
}

} // namespace rcc::mc
