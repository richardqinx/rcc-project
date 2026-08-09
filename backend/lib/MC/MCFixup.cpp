// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MC/MCFixup.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace rcc::mc {
namespace {

[[nodiscard]] Expected<std::int64_t> checkedAdd(std::int64_t Left, std::int64_t Right) {
  std::int64_t Result = 0;
  if (__builtin_add_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::RelocationFailed, "MC expression addition overflows");
  return Result;
}

[[nodiscard]] Expected<std::int64_t> checkedSubtract(std::int64_t Left, std::int64_t Right) {
  std::int64_t Result = 0;
  if (__builtin_sub_overflow(Left, Right, &Result))
    return makeUnexpected(ErrorCode::RelocationFailed, "MC expression subtraction overflows");
  return Result;
}

[[nodiscard]] Expected<MCValue> combineLinearValues(const MCValue &Left, const MCValue &Right,
                                                    bool SubtractRight) {
  struct SymbolTerm final {
    MCSymbolRef Symbol;
    int Coefficient = 0;
  };
  std::vector<SymbolTerm> Terms;
  const auto AddTerm = [&Terms](const MCSymbolRef &Symbol, int Coefficient) -> Expected<void> {
    if (!Symbol)
      return {};
    const auto Position = std::ranges::find(Terms, Symbol, &SymbolTerm::Symbol);
    if (Position == Terms.end()) {
      Terms.push_back(SymbolTerm{.Symbol = Symbol, .Coefficient = Coefficient});
      return {};
    }
    Position->Coefficient += Coefficient;
    if (Position->Coefficient < -1 || Position->Coefficient > 1) {
      return makeUnexpected(ErrorCode::RelocationFailed,
                            "MC expression has a symbol coefficient outside -1 to 1");
    }
    if (Position->Coefficient == 0)
      Terms.erase(Position);
    return {};
  };

  for (const auto &[Symbol, Coefficient] : std::array<std::pair<MCSymbolRef, int>, 4>{
           std::pair{Left.AddSymbol, 1},
           std::pair{Left.SubtractSymbol, -1},
           std::pair{Right.AddSymbol, SubtractRight ? -1 : 1},
           std::pair{Right.SubtractSymbol, SubtractRight ? 1 : -1},
       }) {
    auto Added = AddTerm(Symbol, Coefficient);
    if (!Added)
      return std::unexpected(Added.error());
  }

  MCValue Result;
  for (const SymbolTerm &Term : Terms) {
    MCSymbolRef &Destination = Term.Coefficient > 0 ? Result.AddSymbol : Result.SubtractSymbol;
    if (Destination) {
      return makeUnexpected(ErrorCode::RelocationFailed,
                            "MC expression contains more than one symbol with the same sign");
    }
    Destination = Term.Symbol;
  }
  auto Addend = SubtractRight ? checkedSubtract(Left.Addend, Right.Addend)
                              : checkedAdd(Left.Addend, Right.Addend);
  if (!Addend)
    return std::unexpected(Addend.error());
  Result.Addend = *Addend;
  return Result;
}

[[nodiscard]] Expected<MCValue> evaluate(const MCExpr &Expression, std::size_t Depth) {
  if (Depth > 256)
    return makeUnexpected(ErrorCode::RelocationFailed, "MC expression exceeds its depth limit");

  switch (Expression.kind()) {
  case MCExpr::Kind::Constant:
    return MCValue{.AddSymbol = {},
                   .SubtractSymbol = {},
                   .Addend = *Expression.getIf<std::int64_t>(),
                   .TargetKind = 0};
  case MCExpr::Kind::SymbolRef:
    return MCValue{.AddSymbol = Expression.getIf<MCSymbolRefExpr>()->Symbol,
                   .SubtractSymbol = {},
                   .Addend = 0,
                   .TargetKind = 0};
  case MCExpr::Kind::Target: {
    const auto &Target = *Expression.getIf<MCTargetExpr>();
    auto Result = evaluate(*Target.Operand, Depth + 1);
    if (!Result)
      return std::unexpected(Result.error());
    if (Result->TargetKind != 0) {
      return makeUnexpected(ErrorCode::RelocationFailed,
                            "nested target MC expressions are not relocatable");
    }
    Result->TargetKind = Target.Kind;
    return Result;
  }
  case MCExpr::Kind::Unary: {
    const auto &Unary = *Expression.getIf<MCUnaryExpr>();
    auto Operand = evaluate(*Unary.Operand, Depth + 1);
    if (!Operand)
      return std::unexpected(Operand.error());
    if (Unary.Operator == MCUnaryOperator::Plus)
      return Operand;
    if (Unary.Operator == MCUnaryOperator::Minus && Operand->isAbsolute()) {
      if (Operand->Addend == std::numeric_limits<std::int64_t>::min()) {
        return makeUnexpected(ErrorCode::RelocationFailed, "MC expression negation overflows");
      }
      Operand->Addend = -Operand->Addend;
      return Operand;
    }
    const auto Absolute = Expression.evaluateAbsolute();
    if (!Absolute) {
      return makeUnexpected(ErrorCode::RelocationFailed,
                            "symbolic unary MC expression is not relocatable");
    }
    return MCValue{.AddSymbol = {}, .SubtractSymbol = {}, .Addend = *Absolute, .TargetKind = 0};
  }
  case MCExpr::Kind::Binary: {
    const auto &Binary = *Expression.getIf<MCBinaryExpr>();
    if (Binary.Operator != MCBinaryOperator::Add && Binary.Operator != MCBinaryOperator::Subtract) {
      const auto Absolute = Expression.evaluateAbsolute();
      if (!Absolute) {
        return makeUnexpected(ErrorCode::RelocationFailed,
                              "symbolic binary MC expression is not relocatable");
      }
      return MCValue{.AddSymbol = {}, .SubtractSymbol = {}, .Addend = *Absolute, .TargetKind = 0};
    }
    auto Left = evaluate(*Binary.Left, Depth + 1);
    auto Right = evaluate(*Binary.Right, Depth + 1);
    if (!Left)
      return std::unexpected(Left.error());
    if (!Right)
      return std::unexpected(Right.error());
    if (Left->TargetKind != 0 || Right->TargetKind != 0) {
      return makeUnexpected(ErrorCode::RelocationFailed,
                            "target MC expression cannot participate in outer arithmetic");
    }

    return combineLinearValues(*Left, *Right, Binary.Operator == MCBinaryOperator::Subtract);
  }
  }
  return makeUnexpected(ErrorCode::InternalError, "MC expression has an invalid kind");
}

} // namespace

Expected<void> MCFixup::verify() const {
  if (Kind == fixup_kind::None)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC fixup has no kind");
  if (!Value)
    return makeUnexpected(ErrorCode::InvalidArgument, "MC fixup has no value expression");
  auto Verification = Value->verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return {};
}

Expected<MCValue> evaluateAsRelocatable(const MCExpr &Expression) {
  auto Verification = Expression.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return evaluate(Expression, 0);
}

} // namespace rcc::mc
