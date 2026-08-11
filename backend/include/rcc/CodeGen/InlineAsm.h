// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_INLINEASM_H
#define RCC_CODEGEN_INLINEASM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace rcc {

inline constexpr std::size_t InlineAsmStringOperandIndex = 0;
inline constexpr std::size_t InlineAsmExtraInfoOperandIndex = 1;
inline constexpr std::size_t InlineAsmFirstConstraintOperandIndex = 2;

enum class InlineAsmExtraInfoFlag : std::uint32_t {
  HasSideEffects = 1U,
  AlignStack = 2U,
  IntelDialect = 4U,
  MayLoad = 8U,
  MayStore = 16U,
  Convergent = 32U,
  MayUnwind = 64U,
};

class InlineAsmExtraInfo final {
public:
  explicit constexpr InlineAsmExtraInfo(std::uint32_t RawValue = 0) noexcept
      : Value(RawValue) {}

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr bool has(InlineAsmExtraInfoFlag Flag) const noexcept {
    return (Value & static_cast<std::uint32_t>(Flag)) != 0;
  }

  constexpr void set(InlineAsmExtraInfoFlag Flag) noexcept {
    Value |= static_cast<std::uint32_t>(Flag);
  }

  [[nodiscard]] static constexpr std::optional<std::uint32_t>
  namedFlagBits(std::string_view Name) noexcept {
    if (Name == "sideeffect")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::HasSideEffects);
    if (Name == "mayload")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::MayLoad);
    if (Name == "maystore")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::MayStore);
    if (Name == "isconvergent")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::Convergent);
    if (Name == "alignstack")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::AlignStack);
    if (Name == "unwind")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::MayUnwind);
    if (Name == "attdialect")
      return 0;
    if (Name == "inteldialect")
      return static_cast<std::uint32_t>(InlineAsmExtraInfoFlag::IntelDialect);
    return std::nullopt;
  }

  [[nodiscard]] std::vector<std::string_view> names() const {
    std::vector<std::string_view> Result;
    Result.reserve(7);
    if (has(InlineAsmExtraInfoFlag::HasSideEffects))
      Result.push_back("sideeffect");
    if (has(InlineAsmExtraInfoFlag::MayLoad))
      Result.push_back("mayload");
    if (has(InlineAsmExtraInfoFlag::MayStore))
      Result.push_back("maystore");
    if (has(InlineAsmExtraInfoFlag::Convergent))
      Result.push_back("isconvergent");
    if (has(InlineAsmExtraInfoFlag::AlignStack))
      Result.push_back("alignstack");
    if (has(InlineAsmExtraInfoFlag::MayUnwind))
      Result.push_back("unwind");
    Result.push_back(has(InlineAsmExtraInfoFlag::IntelDialect) ? "inteldialect" : "attdialect");
    return Result;
  }

  friend constexpr bool operator==(InlineAsmExtraInfo Left,
                                   InlineAsmExtraInfo Right) noexcept = default;

private:
  std::uint32_t Value = 0;
};

enum class InlineAsmOperandKind : std::uint8_t {
  RegisterUse = 1,
  RegisterDef = 2,
  RegisterDefEarlyClobber = 3,
  Clobber = 4,
  Immediate = 5,
  Memory = 6,
  Function = 7,
};

enum class InlineAsmMemoryConstraint : std::uint8_t {
  Unknown = 0,
  ES,
  I,
  K,
  M,
  O,
  V,
  A,
  Q,
  R,
  S,
  T,
  UM,
  UN,
  UQ,
  US,
  UT,
  UV,
  UY,
  X,
  Z,
  ZB,
  ZC,
  ZY,
  P,
  ZQ,
  ZR,
  ZS,
  ZT,
};

struct InlineAsmMemoryConstraintName final {
  std::string_view Name;
  InlineAsmMemoryConstraint Constraint = InlineAsmMemoryConstraint::Unknown;
};

inline constexpr std::array InlineAsmMemoryConstraintNames{
    InlineAsmMemoryConstraintName{"es", InlineAsmMemoryConstraint::ES},
    InlineAsmMemoryConstraintName{"i", InlineAsmMemoryConstraint::I},
    InlineAsmMemoryConstraintName{"k", InlineAsmMemoryConstraint::K},
    InlineAsmMemoryConstraintName{"m", InlineAsmMemoryConstraint::M},
    InlineAsmMemoryConstraintName{"o", InlineAsmMemoryConstraint::O},
    InlineAsmMemoryConstraintName{"v", InlineAsmMemoryConstraint::V},
    InlineAsmMemoryConstraintName{"A", InlineAsmMemoryConstraint::A},
    InlineAsmMemoryConstraintName{"Q", InlineAsmMemoryConstraint::Q},
    InlineAsmMemoryConstraintName{"R", InlineAsmMemoryConstraint::R},
    InlineAsmMemoryConstraintName{"S", InlineAsmMemoryConstraint::S},
    InlineAsmMemoryConstraintName{"T", InlineAsmMemoryConstraint::T},
    InlineAsmMemoryConstraintName{"Um", InlineAsmMemoryConstraint::UM},
    InlineAsmMemoryConstraintName{"Un", InlineAsmMemoryConstraint::UN},
    InlineAsmMemoryConstraintName{"Uq", InlineAsmMemoryConstraint::UQ},
    InlineAsmMemoryConstraintName{"Us", InlineAsmMemoryConstraint::US},
    InlineAsmMemoryConstraintName{"Ut", InlineAsmMemoryConstraint::UT},
    InlineAsmMemoryConstraintName{"Uv", InlineAsmMemoryConstraint::UV},
    InlineAsmMemoryConstraintName{"Uy", InlineAsmMemoryConstraint::UY},
    InlineAsmMemoryConstraintName{"X", InlineAsmMemoryConstraint::X},
    InlineAsmMemoryConstraintName{"Z", InlineAsmMemoryConstraint::Z},
    InlineAsmMemoryConstraintName{"ZB", InlineAsmMemoryConstraint::ZB},
    InlineAsmMemoryConstraintName{"ZC", InlineAsmMemoryConstraint::ZC},
    InlineAsmMemoryConstraintName{"Zy", InlineAsmMemoryConstraint::ZY},
    InlineAsmMemoryConstraintName{"p", InlineAsmMemoryConstraint::P},
    InlineAsmMemoryConstraintName{"ZQ", InlineAsmMemoryConstraint::ZQ},
    InlineAsmMemoryConstraintName{"ZR", InlineAsmMemoryConstraint::ZR},
    InlineAsmMemoryConstraintName{"ZS", InlineAsmMemoryConstraint::ZS},
    InlineAsmMemoryConstraintName{"ZT", InlineAsmMemoryConstraint::ZT},
};

class InlineAsmOperandFlag final {
public:
  constexpr InlineAsmOperandFlag(InlineAsmOperandKind Kind, std::uint32_t OperandCount = 1) noexcept
      : Value(static_cast<std::uint32_t>(Kind) |
              ((OperandCount & OperandCountMask) << OperandCountShift)) {}

  explicit constexpr InlineAsmOperandFlag(std::uint32_t RawValue) noexcept : Value(RawValue) {}

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Value;
  }

  [[nodiscard]] constexpr InlineAsmOperandKind kind() const noexcept {
    return static_cast<InlineAsmOperandKind>(Value & KindMask);
  }

  [[nodiscard]] constexpr std::uint32_t operandCount() const noexcept {
    return (Value >> OperandCountShift) & OperandCountMask;
  }

  [[nodiscard]] constexpr bool isMatched() const noexcept {
    return (Value & MatchedMask) != 0;
  }

  [[nodiscard]] constexpr std::optional<std::uint32_t> matchingOperand() const noexcept {
    return isMatched() ? std::optional((Value >> PayloadShift) & MatchedOperandMask)
                       : std::nullopt;
  }

  [[nodiscard]] constexpr std::optional<std::uint32_t> registerClass() const noexcept {
    if (isMatched())
      return std::nullopt;
    const std::uint32_t Encoded = (Value >> PayloadShift) & RegisterClassMask;
    return Encoded == 0 ? std::nullopt : std::optional(Encoded);
  }

  [[nodiscard]] constexpr InlineAsmMemoryConstraint memoryConstraint() const noexcept {
    if (isMatched())
      return InlineAsmMemoryConstraint::Unknown;
    return static_cast<InlineAsmMemoryConstraint>((Value >> PayloadShift) & MatchedOperandMask);
  }

  [[nodiscard]] constexpr bool setMatchingOperand(std::uint32_t OperandNumber) noexcept {
    if (OperandNumber > MatchedOperandMask)
      return false;
    Value &= ~(MatchedMask | (MatchedOperandMask << PayloadShift));
    Value |= MatchedMask | (OperandNumber << PayloadShift);
    return true;
  }

  [[nodiscard]] constexpr bool setRegisterClass(std::uint32_t RegisterClass) noexcept {
    if (RegisterClass == 0 || RegisterClass > RegisterClassMask || isMatched())
      return false;
    Value &= ~(RegisterClassMask << PayloadShift);
    Value |= RegisterClass << PayloadShift;
    return true;
  }

  [[nodiscard]] constexpr bool
  setMemoryConstraint(InlineAsmMemoryConstraint Constraint) noexcept {
    const std::uint32_t Encoded = static_cast<std::uint32_t>(Constraint);
    if (Encoded == 0 || Encoded > MatchedOperandMask || isMatched())
      return false;
    Value &= ~(MatchedOperandMask << PayloadShift);
    Value |= Encoded << PayloadShift;
    return true;
  }

  [[nodiscard]] static constexpr std::optional<InlineAsmOperandKind>
  kindForName(std::string_view Name) noexcept {
    if (Name == "regdef")
      return InlineAsmOperandKind::RegisterDef;
    if (Name == "reguse")
      return InlineAsmOperandKind::RegisterUse;
    if (Name == "regdef-ec")
      return InlineAsmOperandKind::RegisterDefEarlyClobber;
    if (Name == "clobber")
      return InlineAsmOperandKind::Clobber;
    if (Name == "imm")
      return InlineAsmOperandKind::Immediate;
    if (Name == "mem")
      return InlineAsmOperandKind::Memory;
    return std::nullopt;
  }

  [[nodiscard]] static constexpr std::string_view
  kindName(InlineAsmOperandKind Kind) noexcept {
    switch (Kind) {
    case InlineAsmOperandKind::RegisterUse:
      return "reguse";
    case InlineAsmOperandKind::RegisterDef:
      return "regdef";
    case InlineAsmOperandKind::RegisterDefEarlyClobber:
      return "regdef-ec";
    case InlineAsmOperandKind::Clobber:
      return "clobber";
    case InlineAsmOperandKind::Immediate:
      return "imm";
    case InlineAsmOperandKind::Memory:
    case InlineAsmOperandKind::Function:
      return "mem";
    }
    return {};
  }

  [[nodiscard]] static constexpr std::optional<InlineAsmMemoryConstraint>
  memoryConstraintForName(std::string_view Name) noexcept {
    for (const InlineAsmMemoryConstraintName &Entry : InlineAsmMemoryConstraintNames) {
      if (Entry.Name == Name)
        return Entry.Constraint;
    }
    return std::nullopt;
  }

  [[nodiscard]] static constexpr std::optional<std::string_view>
  memoryConstraintName(InlineAsmMemoryConstraint Constraint) noexcept {
    for (const InlineAsmMemoryConstraintName &Entry : InlineAsmMemoryConstraintNames) {
      if (Entry.Constraint == Constraint)
        return Entry.Name;
    }
    return std::nullopt;
  }

  [[nodiscard]] constexpr bool isRegisterKind() const noexcept {
    return kind() == InlineAsmOperandKind::RegisterUse ||
           kind() == InlineAsmOperandKind::RegisterDef ||
           kind() == InlineAsmOperandKind::RegisterDefEarlyClobber;
  }

private:
  static constexpr std::uint32_t KindMask = 0x7U;
  static constexpr std::uint32_t OperandCountShift = 3U;
  static constexpr std::uint32_t OperandCountMask = 0x1FFFU;
  static constexpr std::uint32_t PayloadShift = 16U;
  static constexpr std::uint32_t MatchedOperandMask = 0x7FFFU;
  static constexpr std::uint32_t RegisterClassMask = 0x3FFFU;
  static constexpr std::uint32_t MatchedMask = 0x80000000U;

  std::uint32_t Value = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_INLINEASM_H
