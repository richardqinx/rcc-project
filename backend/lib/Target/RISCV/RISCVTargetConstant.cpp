// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVTargetConstant.h"

#include "rcc/Target/RISCV/RISCVTargetRelocationKinds.h"

#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] std::string_view kindSpelling(RISCVSymbolConstantKind Kind) noexcept {
  switch (Kind) {
  case RISCVSymbolConstantKind::GlobalValue:
    return "global";
  case RISCVSymbolConstantKind::ExternalSymbol:
    return "external";
  }
  return {};
}

[[nodiscard]] Expected<RISCVSymbolConstantKind> parseKind(std::string_view Text) {
  if (Text == "global")
    return RISCVSymbolConstantKind::GlobalValue;
  if (Text == "external")
    return RISCVSymbolConstantKind::ExternalSymbol;
  return makeUnexpected(ErrorCode::ParseError, "unknown RV64GC symbol constant kind");
}

void appendU64(std::vector<std::byte> &Bytes, std::uint64_t Value) {
  for (std::uint8_t Shift = 0; Shift < 64; Shift += 8)
    Bytes.push_back(static_cast<std::byte>(Value >> Shift));
}

[[nodiscard]] std::uint64_t readU64(std::span<const std::byte> Bytes) noexcept {
  std::uint64_t Result = 0;
  for (std::uint8_t Shift = 0; Shift < 64; Shift += 8)
    Result |= std::to_integer<std::uint64_t>(Bytes[Shift / 8U]) << Shift;
  return Result;
}

} // namespace

Expected<void> RISCVSymbolConstant::verify() const {
  if (kindSpelling(ConstantKind).empty())
    return makeUnexpected(ErrorCode::VerificationError, "RV64GC symbol constant kind is invalid");
  if (SymbolName.empty() || SymbolName.find('\0') != std::string::npos) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "RV64GC symbol constant requires a valid symbol name");
  }
  return {};
}

Expected<RISCVSymbolConstant> RISCVSymbolConstant::clone() const {
  return RISCVSymbolConstant(ConstantKind, SymbolName, SymbolAddend);
}

std::uint64_t RISCVSymbolConstant::stableHash() const noexcept {
  constexpr std::uint64_t OffsetBasis = 14695981039346656037ULL;
  constexpr std::uint64_t Prime = 1099511628211ULL;
  std::uint64_t Hash = OffsetBasis;
  const auto Mix = [&Hash](std::uint8_t Byte) {
    Hash ^= Byte;
    Hash *= Prime;
  };
  Mix(static_cast<std::uint8_t>(ConstantKind));
  const std::uint64_t AddendBits = std::bit_cast<std::uint64_t>(SymbolAddend);
  for (std::uint8_t Shift = 0; Shift < 64; Shift += 8)
    Mix(static_cast<std::uint8_t>(AddendBits >> Shift));
  for (const char Character : SymbolName)
    Mix(static_cast<unsigned char>(Character));
  return Hash;
}

std::string RISCVSymbolConstant::print() const {
  return std::string(kindSpelling(ConstantKind)) + "," + std::to_string(SymbolAddend) + "," +
         SymbolName;
}

Expected<std::vector<std::byte>> RISCVSymbolConstant::encode() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  std::vector<std::byte> Payload;
  Payload.reserve(9U + SymbolName.size());
  Payload.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(ConstantKind)));
  appendU64(Payload, std::bit_cast<std::uint64_t>(SymbolAddend));
  for (const char Character : SymbolName)
    Payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
  return Payload;
}

Expected<TargetConstantEmission> RISCVSymbolConstant::emit() const {
  auto Verification = verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  TargetConstantEmission Emission;
  Emission.Bytes.resize(8);
  Emission.Fixups.push_back(TargetConstantFixup{
      .Offset = 0,
      .Kind = targetRelocationKind(RISCVTargetRelocationKind::Absolute64),
      .Symbol = SymbolName,
      .Addend = SymbolAddend,
  });
  return Emission;
}

Alignment RISCVSymbolConstant::requiredAlignment() const noexcept {
  return *Alignment::create(8);
}

std::optional<TargetRelocationKindID> RISCVSymbolConstant::relocationKind() const noexcept {
  return targetRelocationKind(RISCVTargetRelocationKind::Absolute64);
}

Expected<RISCVSymbolConstant> RISCVSymbolConstant::parse(std::string_view Text) {
  const std::size_t KindEnd = Text.find(',');
  const std::size_t AddendEnd =
      KindEnd == std::string_view::npos ? std::string_view::npos : Text.find(',', KindEnd + 1U);
  if (KindEnd == std::string_view::npos || AddendEnd == std::string_view::npos) {
    return makeUnexpected(ErrorCode::ParseError,
                          "RV64GC symbol constant must be 'kind,addend,symbol'");
  }
  auto Kind = parseKind(Text.substr(0, KindEnd));
  if (!Kind)
    return std::unexpected(Kind.error());
  std::int64_t Addend = 0;
  const std::string_view AddendText = Text.substr(KindEnd + 1U, AddendEnd - KindEnd - 1U);
  const auto [End, Error] =
      std::from_chars(AddendText.data(), AddendText.data() + AddendText.size(), Addend);
  if (Error != std::errc{} || End != AddendText.data() + AddendText.size())
    return makeUnexpected(ErrorCode::ParseError, "RV64GC symbol constant addend is invalid");
  RISCVSymbolConstant Result(*Kind, std::string(Text.substr(AddendEnd + 1U)), Addend);
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

Expected<RISCVSymbolConstant> RISCVSymbolConstant::decode(std::uint32_t Version,
                                                          std::span<const std::byte> Payload) {
  if (Version != schemaVersion() || Payload.size() < 10) {
    return makeUnexpected(ErrorCode::ParseError,
                          "encoded RV64GC symbol constant has an invalid version or size");
  }
  const auto KindValue = std::to_integer<std::uint8_t>(Payload.front());
  if (KindValue > static_cast<std::uint8_t>(RISCVSymbolConstantKind::ExternalSymbol))
    return makeUnexpected(ErrorCode::ParseError, "encoded RV64GC symbol constant kind is invalid");
  std::string Symbol;
  Symbol.reserve(Payload.size() - 9U);
  for (const std::byte Character : Payload.subspan(9))
    Symbol.push_back(static_cast<char>(std::to_integer<unsigned char>(Character)));
  RISCVSymbolConstant Result(static_cast<RISCVSymbolConstantKind>(KindValue), std::move(Symbol),
                             std::bit_cast<std::int64_t>(readU64(Payload.subspan(1, 8))));
  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

} // namespace rcc::riscv
