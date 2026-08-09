// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_MCINST_H
#define RCC_MC_MCINST_H

#include "rcc/MC/MCExpr.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace rcc::mc {

class MCRegister final {
public:
  constexpr MCRegister() noexcept = default;

  [[nodiscard]] static constexpr MCRegister fromValue(std::uint32_t Value) noexcept {
    return MCRegister(Value);
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  friend constexpr bool operator==(MCRegister Left, MCRegister Right) noexcept = default;

private:
  explicit constexpr MCRegister(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = 0;
};

class MCOpcode final {
public:
  constexpr MCOpcode() noexcept = default;

  [[nodiscard]] static constexpr MCOpcode fromValue(std::uint32_t Value) noexcept {
    return MCOpcode(Value);
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Value != 0;
  }

  friend constexpr bool operator==(MCOpcode Left, MCOpcode Right) noexcept = default;

private:
  explicit constexpr MCOpcode(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = 0;
};

class MCOperand final {
public:
  enum class Kind : std::uint8_t {
    Register,
    Immediate,
    Float32Bits,
    Float64Bits,
    Expression,
  };

  [[nodiscard]] static MCOperand registerOperand(MCRegister Register) noexcept {
    return MCOperand(Kind::Register, Register);
  }

  [[nodiscard]] static MCOperand immediate(std::int64_t Value) noexcept {
    return MCOperand(Kind::Immediate, Value);
  }

  [[nodiscard]] static MCOperand float32Bits(std::uint32_t Value) noexcept {
    return MCOperand(Kind::Float32Bits, Value);
  }

  [[nodiscard]] static MCOperand float64Bits(std::uint64_t Value) noexcept {
    return MCOperand(Kind::Float64Bits, Value);
  }

  [[nodiscard]] static MCOperand expression(MCExprRef Value) {
    return MCOperand(Kind::Expression, std::move(Value));
  }

  [[nodiscard]] Kind kind() const noexcept {
    return OperandKind;
  }

  [[nodiscard]] const MCRegister *registerValue() const noexcept {
    return std::get_if<MCRegister>(&Storage);
  }

  [[nodiscard]] const std::int64_t *immediateValue() const noexcept {
    return std::get_if<std::int64_t>(&Storage);
  }

  [[nodiscard]] const std::uint32_t *float32BitsValue() const noexcept {
    return std::get_if<std::uint32_t>(&Storage);
  }

  [[nodiscard]] const std::uint64_t *float64BitsValue() const noexcept {
    return std::get_if<std::uint64_t>(&Storage);
  }

  [[nodiscard]] const MCExprRef *expressionValue() const noexcept {
    return std::get_if<MCExprRef>(&Storage);
  }

private:
  using StorageT = std::variant<MCRegister, std::int64_t, std::uint32_t, std::uint64_t, MCExprRef>;

  MCOperand(Kind OperandKind, StorageT Storage)
      : OperandKind(OperandKind), Storage(std::move(Storage)) {}

  Kind OperandKind;
  StorageT Storage;
};

class MCInst final {
public:
  explicit MCInst(MCOpcode InstructionOpcode) : InstructionOpcode(InstructionOpcode) {}

  [[nodiscard]] MCOpcode opcode() const noexcept {
    return InstructionOpcode;
  }

  void setFlags(std::uint32_t NewFlags) noexcept {
    Flags = NewFlags;
  }

  [[nodiscard]] std::uint32_t flags() const noexcept {
    return Flags;
  }

  void setSourceIdentity(std::uint64_t Identity) noexcept {
    SourceIdentity = Identity;
  }

  [[nodiscard]] std::uint64_t sourceIdentity() const noexcept {
    return SourceIdentity;
  }

  void reserveOperands(std::size_t Count) {
    Operands.reserve(Count);
  }

  void addOperand(MCOperand Operand) {
    Operands.push_back(std::move(Operand));
  }

  [[nodiscard]] std::span<const MCOperand> operands() const noexcept {
    return Operands;
  }

private:
  MCOpcode InstructionOpcode;
  std::uint32_t Flags = 0;
  std::uint64_t SourceIdentity = 0;
  std::vector<MCOperand> Operands;
};

} // namespace rcc::mc

#endif // RCC_MC_MCINST_H
