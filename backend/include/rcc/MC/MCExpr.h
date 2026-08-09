// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCEXPR_H
#define RCC_MC_MCEXPR_H

#include "rcc/MC/MCSymbol.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace rcc::mc {

class MCExpr;
using MCExprRef = std::shared_ptr<const MCExpr>;

enum class MCUnaryOperator : std::uint8_t {
  Plus,
  Minus,
  BitwiseNot,
  LogicalNot,
};

enum class MCBinaryOperator : std::uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  Modulo,
  ShiftLeft,
  ShiftRight,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  LogicalAnd,
  LogicalOr,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
};

struct MCSymbolRefExpr final {
  MCSymbolRef Symbol;
};

struct MCUnaryExpr final {
  MCUnaryOperator Operator = MCUnaryOperator::Plus;
  MCExprRef Operand;
};

struct MCBinaryExpr final {
  MCBinaryOperator Operator = MCBinaryOperator::Add;
  MCExprRef Left;
  MCExprRef Right;
};

struct MCTargetExpr final {
  std::uint32_t Kind = 0;
  MCExprRef Operand;
};

class MCExpr final {
public:
  enum class Kind : std::uint8_t {
    Constant,
    SymbolRef,
    Unary,
    Binary,
    Target,
  };

  [[nodiscard]] static MCExprRef constant(std::int64_t Value);
  [[nodiscard]] static Expected<MCExprRef> symbol(MCSymbolRef Symbol);
  [[nodiscard]] static Expected<MCExprRef> unary(MCUnaryOperator Operator, MCExprRef Operand);
  [[nodiscard]] static Expected<MCExprRef> binary(MCBinaryOperator Operator, MCExprRef Left,
                                                  MCExprRef Right);
  [[nodiscard]] static Expected<MCExprRef> target(std::uint32_t Kind, MCExprRef Operand);

  [[nodiscard]] Kind kind() const noexcept {
    return ExpressionKind;
  }

  template <typename ValueT> [[nodiscard]] const ValueT *getIf() const noexcept {
    return std::get_if<ValueT>(&Storage);
  }

  [[nodiscard]] Expected<void> verify(std::size_t MaximumDepth = 256) const;
  [[nodiscard]] std::optional<std::int64_t> evaluateAbsolute() const noexcept;

private:
  using StorageT =
      std::variant<std::int64_t, MCSymbolRefExpr, MCUnaryExpr, MCBinaryExpr, MCTargetExpr>;

  MCExpr(Kind ExpressionKind, StorageT Storage)
      : ExpressionKind(ExpressionKind), Storage(std::move(Storage)) {}

  Kind ExpressionKind;
  StorageT Storage;
};

} // namespace rcc::mc

#endif // RCC_MC_MCEXPR_H
