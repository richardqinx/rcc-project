// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/IR/Constants.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace rcc {
namespace {

[[nodiscard]] bool isIdentifierStart(char Character) noexcept {
  return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z') ||
         Character == '-' || Character == '$' || Character == '.' || Character == '_';
}

[[nodiscard]] bool isIdentifierBody(char Character) noexcept {
  return isIdentifierStart(Character) || (Character >= '0' && Character <= '9');
}
[[nodiscard]] bool canUseBareName(std::string_view Name) noexcept {
  if (Name.empty() || !isIdentifierStart(Name.front()))
    return false;
  return std::ranges::all_of(Name.substr(1), isIdentifierBody);
}

[[nodiscard]] std::string printName(char Prefix, std::string_view Name, bool Numeric) {
  std::string Result(1, Prefix);
  if (Numeric || canUseBareName(Name)) {
    Result.append(Name);
    return Result;
  }
  static constexpr std::string_view HexDigits = "0123456789ABCDEF";
  Result.push_back('"');
  for (const char RawCharacter : Name) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (Character >= 0x20U && Character <= 0x7eU && Character != '"' && Character != '\\') {
      Result.push_back(static_cast<char>(Character));
      continue;
    }
    Result.push_back('\\');
    Result.push_back(HexDigits[Character >> 4U]);
    Result.push_back(HexDigits[Character & 0xfU]);
  }
  Result.push_back('"');
  return Result;
}

[[nodiscard]] Expected<std::string> printType(const GenericConstantType &Type);

[[nodiscard]] Expected<std::string> printTypeList(std::span<const GenericConstantType> Types,
                                                  std::string_view Open, std::string_view Close) {
  std::string Result(Open);
  for (std::size_t Index = 0; Index < Types.size(); ++Index) {
    if (Index != 0)
      Result.append(", ");
    auto Element = printType(Types[Index]);
    if (!Element)
      return std::unexpected(Element.error());
    Result.append(*Element);
  }
  Result.append(Close);
  return Result;
}

Expected<std::string> printType(const GenericConstantType &Type) {
  auto Verification = Type.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  switch (Type.Kind) {
  case GenericConstantTypeKind::Invalid:
    break;
  case GenericConstantTypeKind::Void:
    return std::string("void");
  case GenericConstantTypeKind::Label:
    return std::string("label");
  case GenericConstantTypeKind::Token:
    return std::string("token");
  case GenericConstantTypeKind::Metadata:
    return std::string("metadata");
  case GenericConstantTypeKind::Integer:
    return "i" + std::to_string(Type.BitWidth);
  case GenericConstantTypeKind::Byte:
    return std::string("byte");
  case GenericConstantTypeKind::Floating:
    switch (Type.Semantics) {
    case FloatSemantics::IEEEHalf:
      return std::string("half");
    case FloatSemantics::BFloat:
      return std::string("bfloat");
    case FloatSemantics::IEEESingle:
      return std::string("float");
    case FloatSemantics::IEEEDouble:
      return std::string("double");
    case FloatSemantics::IEEEQuad:
      return std::string("fp128");
    case FloatSemantics::PPCDoubleDouble:
      return std::string("ppc_fp128");
    case FloatSemantics::X87DoubleExtended:
      return std::string("x86_fp80");
    default:
      return makeUnexpected(ErrorCode::Unsupported,
                            "floating semantics has no LLVM IR type spelling");
    }
  case GenericConstantTypeKind::Pointer:
    if (Type.AddressSpace == 0)
      return std::string("ptr");
    return "ptr addrspace(" + std::to_string(Type.AddressSpace) + ")";
  case GenericConstantTypeKind::Array: {
    auto Element = printType(Type.Elements.front());
    if (!Element)
      return std::unexpected(Element.error());
    return "[" + std::to_string(Type.ElementCount) + " x " + *Element + "]";
  }
  case GenericConstantTypeKind::Vector: {
    auto Element = printType(Type.Elements.front());
    if (!Element)
      return std::unexpected(Element.error());
    std::string Result = "<";
    if (Type.Scalable)
      Result.append("vscale x ");
    Result.append(std::to_string(Type.ElementCount));
    Result.append(" x ");
    Result.append(*Element);
    Result.push_back('>');
    return Result;
  }
  case GenericConstantTypeKind::Struct:
    return printTypeList(Type.Elements, Type.Packed ? "<{ " : "{ ", Type.Packed ? " }>" : " }");
  case GenericConstantTypeKind::Function: {
    auto Return = printType(Type.Elements.front());
    if (!Return)
      return std::unexpected(Return.error());
    std::string Result = *Return + " (";
    for (std::size_t Index = 1; Index < Type.Elements.size(); ++Index) {
      if (Index != 1)
        Result.append(", ");
      auto Parameter = printType(Type.Elements[Index]);
      if (!Parameter)
        return std::unexpected(Parameter.error());
      Result.append(*Parameter);
    }
    if (Type.VarArg) {
      if (Type.Elements.size() != 1)
        Result.append(", ");
      Result.append("...");
    }
    Result.push_back(')');
    return Result;
  }
  case GenericConstantTypeKind::Named:
    return printName('%', Type.Name, Type.NumericName);
  }
  return makeUnexpected(ErrorCode::VerificationError, "generic constant type is invalid");
}

[[nodiscard]] std::string_view
expressionOpcodeName(GenericConstantExpressionOpcode Opcode) noexcept {
  switch (Opcode) {
  case GenericConstantExpressionOpcode::Trunc:
    return "trunc";
  case GenericConstantExpressionOpcode::BitCast:
    return "bitcast";
  case GenericConstantExpressionOpcode::AddressSpaceCast:
    return "addrspacecast";
  case GenericConstantExpressionOpcode::IntToPtr:
    return "inttoptr";
  case GenericConstantExpressionOpcode::PtrToAddr:
    return "ptrtoaddr";
  case GenericConstantExpressionOpcode::PtrToInt:
    return "ptrtoint";
  case GenericConstantExpressionOpcode::Add:
    return "add";
  case GenericConstantExpressionOpcode::Sub:
    return "sub";
  case GenericConstantExpressionOpcode::Xor:
    return "xor";
  case GenericConstantExpressionOpcode::GetElementPtr:
    return "getelementptr";
  case GenericConstantExpressionOpcode::ShuffleVector:
    return "shufflevector";
  case GenericConstantExpressionOpcode::InsertElement:
    return "insertelement";
  case GenericConstantExpressionOpcode::ExtractElement:
    return "extractelement";
  case GenericConstantExpressionOpcode::Splat:
    return "splat";
  case GenericConstantExpressionOpcode::Invalid:
    return {};
  }
  return {};
}

[[nodiscard]] Expected<std::string> printValue(const GenericConstantData &Value, bool IncludeType);

[[nodiscard]] Expected<void> appendTypedOperands(std::string &Output,
                                                 std::span<const GenericConstantData> Operands) {
  for (std::size_t Index = 0; Index < Operands.size(); ++Index) {
    if (Index != 0)
      Output.append(", ");
    auto Operand = printValue(Operands[Index], true);
    if (!Operand)
      return std::unexpected(Operand.error());
    Output.append(*Operand);
  }
  return {};
}

Expected<std::string> printValue(const GenericConstantData &Value, bool IncludeType) {
  auto Verification = Value.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  std::string Result;
  if (IncludeType) {
    auto Type = printType(Value.Type);
    if (!Type)
      return std::unexpected(Type.error());
    Result.append(*Type);
    Result.push_back(' ');
  }

  switch (Value.Kind) {
  case GenericConstantKind::Invalid:
    break;
  case GenericConstantKind::Integer:
    if (!Value.IntegerValue)
      return makeUnexpected(ErrorCode::VerificationError,
                            "integer constant is missing its integer payload");
    if (Value.Type.resolved().Kind == GenericConstantTypeKind::Integer &&
        Value.Type.resolved().BitWidth == 1)
      Result.append(Value.IntegerValue.value().isZero() ? "false" : "true");
    else
      Result.append(Value.IntegerValue.value().toUnsignedDecimal());
    return Result;
  case GenericConstantKind::Floating:
    if (!Value.FloatingValue)
      return makeUnexpected(ErrorCode::VerificationError,
                            "floating constant is missing its floating payload");
    Result.append(Value.FloatingValue->toString());
    return Result;
  case GenericConstantKind::NullPointer:
    Result.append("null");
    return Result;
  case GenericConstantKind::ZeroInitializer:
    Result.append("zeroinitializer");
    return Result;
  case GenericConstantKind::Undef:
    Result.append("undef");
    return Result;
  case GenericConstantKind::Poison:
    Result.append("poison");
    return Result;
  case GenericConstantKind::Aggregate: {
    const GenericConstantType &EffectiveType = Value.Type.resolved();
    const bool Packed =
        EffectiveType.Kind == GenericConstantTypeKind::Struct && EffectiveType.Packed;
    Result.append(Packed                                                  ? "<{ "
                  : EffectiveType.Kind == GenericConstantTypeKind::Array  ? "[ "
                  : EffectiveType.Kind == GenericConstantTypeKind::Struct ? "{ "
                                                                          : "< ");
    auto Appended = appendTypedOperands(Result, Value.Elements);
    if (!Appended)
      return std::unexpected(Appended.error());
    Result.append(Packed                                                  ? " }>"
                  : EffectiveType.Kind == GenericConstantTypeKind::Array  ? " ]"
                  : EffectiveType.Kind == GenericConstantTypeKind::Struct ? " }"
                                                                          : " >");
    return Result;
  }
  case GenericConstantKind::ByteString: {
    static constexpr std::string_view Digits = "0123456789ABCDEF";
    Result.append("c\"");
    for (const std::byte Byte : Value.ByteValue) {
      const unsigned Character = std::to_integer<unsigned>(Byte);
      if (Character >= 0x20U && Character <= 0x7eU && Character != '"' && Character != '\\') {
        Result.push_back(static_cast<char>(Character));
      } else {
        Result.push_back('\\');
        Result.push_back(Digits[Character >> 4U]);
        Result.push_back(Digits[Character & 0xfU]);
      }
    }
    Result.push_back('"');
    return Result;
  }
  case GenericConstantKind::GlobalAddress:
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    return Result;
  case GenericConstantKind::BlockAddress:
    Result.append("blockaddress(");
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    Result.append(", ");
    Result.append(printName('%', Value.Block, Value.NumericBlock));
    Result.push_back(')');
    return Result;
  case GenericConstantKind::DSOLocalEquivalent:
    Result.append("dso_local_equivalent ");
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    return Result;
  case GenericConstantKind::NoCFI:
    Result.append("no_cfi ");
    Result.append(printName('@', Value.Symbol, Value.NumericSymbol));
    return Result;
  case GenericConstantKind::PointerAuth:
    Result.append("ptrauth (");
    if (auto Appended = appendTypedOperands(Result, Value.Elements); !Appended)
      return std::unexpected(Appended.error());
    Result.push_back(')');
    return Result;
  case GenericConstantKind::Expression: {
    const std::string_view Opcode = expressionOpcodeName(Value.ExpressionOpcode);
    if (Opcode.empty())
      return makeUnexpected(ErrorCode::VerificationError,
                            "constant expression has an invalid opcode");
    Result.append(Opcode);
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::InBounds))
      Result.append(" inbounds");
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::NoUnsignedSignedWrap))
      Result.append(" nusw");
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::GEPNoUnsignedWrap) ||
        Value.ExpressionFlags.has(GenericConstantExpressionFlag::NoUnsignedWrap))
      Result.append(" nuw");
    if (Value.ExpressionFlags.has(GenericConstantExpressionFlag::NoSignedWrap))
      Result.append(" nsw");
    if (Value.InRangeStart) {
      if (!Value.InRangeEnd) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "constant expression is missing its inrange end");
      }
      const auto &InRangeStart = Value.InRangeStart.value();
      const auto &InRangeEnd = Value.InRangeEnd.value();
      Result.append(" inrange(");
      Result.append(std::to_string(std::bit_cast<std::int64_t>(InRangeStart.words()[0])));
      Result.append(", ");
      Result.append(std::to_string(std::bit_cast<std::int64_t>(InRangeEnd.words()[0])));
      Result.push_back(')');
    }
    Result.append(" (");
    if (Value.ExpressionOpcode == GenericConstantExpressionOpcode::GetElementPtr) {
      if (!Value.SourceElementType) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "getelementptr constant is missing its source element type");
      }
      auto SourceType = printType(Value.SourceElementType.value());
      if (!SourceType)
        return std::unexpected(SourceType.error());
      Result.append(*SourceType);
      Result.append(", ");
    }
    auto First = printValue(Value.Elements.front(), true);
    if (!First)
      return std::unexpected(First.error());
    Result.append(*First);
    const bool IsCast =
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::Trunc ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::BitCast ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::AddressSpaceCast ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::IntToPtr ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::PtrToAddr ||
        Value.ExpressionOpcode == GenericConstantExpressionOpcode::PtrToInt;
    if (IsCast) {
      auto Destination = printType(Value.Type);
      if (!Destination)
        return std::unexpected(Destination.error());
      Result.append(" to ");
      Result.append(*Destination);
    } else {
      for (std::size_t Index = 1; Index < Value.Elements.size(); ++Index) {
        Result.append(", ");
        auto Operand = printValue(Value.Elements[Index], true);
        if (!Operand)
          return std::unexpected(Operand.error());
        Result.append(*Operand);
      }
    }
    Result.push_back(')');
    return Result;
  }
  }
  return makeUnexpected(ErrorCode::VerificationError, "generic constant value is invalid");
}

} // namespace

Expected<std::string> GenericConstantData::toLLVMString() const {
  return printValue(*this, true);
}

} // namespace rcc
