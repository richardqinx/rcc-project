// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetConstant.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
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

struct TestRelocConstant final {
  std::int64_t Addend = 0;
  std::string Symbol;

  [[nodiscard]] static constexpr rcc::TargetConstantTypeID stableTypeID() noexcept {
    return rcc::TargetConstantTypeID::fromValue(0x5253434354430001ULL);
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 2;
  }

  [[nodiscard]] rcc::Expected<void> verify() const {
    if (Symbol.empty()) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "test target constant symbol cannot be empty");
    }
    return {};
  }

  [[nodiscard]] rcc::Expected<TestRelocConstant> clone() const {
    return TestRelocConstant{Addend, Symbol};
  }

  [[nodiscard]] std::uint64_t stableHash() const noexcept {
    std::uint64_t Hash = 1469598103934665603ULL;
    for (const char RawCharacter : Symbol) {
      const auto Character = static_cast<unsigned char>(RawCharacter);
      Hash ^= Character;
      Hash *= 1099511628211ULL;
    }
    Hash ^= static_cast<std::uint64_t>(Addend);
    return Hash * 1099511628211ULL;
  }

  [[nodiscard]] std::string print() const {
    return Symbol + "+" + std::to_string(Addend);
  }

  [[nodiscard]] std::vector<std::byte> addendBytes() const {
    std::vector<std::byte> Bytes(8);
    const std::uint64_t Bits = static_cast<std::uint64_t>(Addend);
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
      Bytes[Index] = static_cast<std::byte>((Bits >> (Index * 8U)) & 0xffU);
    return Bytes;
  }

  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    std::vector<std::byte> Payload = addendBytes();
    for (const char RawCharacter : Symbol) {
      const auto Character = static_cast<unsigned char>(RawCharacter);
      Payload.push_back(static_cast<std::byte>(Character));
    }
    return Payload;
  }

  [[nodiscard]] rcc::Expected<rcc::TargetConstantEmission> emit() const {
    rcc::TargetConstantEmission Emission;
    Emission.Bytes = addendBytes();
    Emission.Fixups.push_back({0, rcc::TargetRelocationKindID::fromValue(9), Symbol, Addend});
    return Emission;
  }

  [[nodiscard]] rcc::Alignment requiredAlignment() const noexcept {
    return *rcc::Alignment::create(8);
  }

  [[nodiscard]] std::optional<rcc::TargetRelocationKindID> relocationKind() const noexcept {
    return rcc::TargetRelocationKindID::fromValue(9);
  }

  [[nodiscard]] static rcc::Expected<TestRelocConstant> parse(std::string_view Text) {
    if (Text.empty())
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError, "test constant text is empty");
    return TestRelocConstant{7, std::string(Text)};
  }

  [[nodiscard]] static rcc::Expected<TestRelocConstant> decode(std::uint32_t Version,
                                                               std::span<const std::byte> Payload) {
    if (Version != schemaVersion())
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "unsupported test constant schema version");
    if (Payload.size() <= 8)
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError, "test constant payload is incomplete");

    std::uint64_t Bits = 0;
    for (std::size_t Index = 0; Index < 8; ++Index)
      Bits |= std::to_integer<std::uint64_t>(Payload[Index]) << (Index * 8U);
    std::string Symbol;
    Symbol.reserve(Payload.size() - 8);
    for (const std::byte Character : Payload.subspan(8))
      Symbol.push_back(static_cast<char>(std::to_integer<unsigned char>(Character)));
    return TestRelocConstant{std::bit_cast<std::int64_t>(Bits), std::move(Symbol)};
  }

  friend bool operator==(const TestRelocConstant &Left, const TestRelocConstant &Right) = default;
};

struct CollidingConstant final {
  TestRelocConstant Value;

  [[nodiscard]] static constexpr rcc::TargetConstantTypeID stableTypeID() noexcept {
    return TestRelocConstant::stableTypeID();
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return TestRelocConstant::schemaVersion();
  }

  [[nodiscard]] rcc::Expected<void> verify() const {
    return Value.verify();
  }

  [[nodiscard]] rcc::Expected<CollidingConstant> clone() const {
    return CollidingConstant{Value};
  }

  [[nodiscard]] std::uint64_t stableHash() const noexcept {
    return Value.stableHash();
  }

  [[nodiscard]] std::string print() const {
    return Value.print();
  }

  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    return Value.encode();
  }

  [[nodiscard]] rcc::Expected<rcc::TargetConstantEmission> emit() const {
    return Value.emit();
  }

  [[nodiscard]] rcc::Alignment requiredAlignment() const noexcept {
    return Value.requiredAlignment();
  }

  [[nodiscard]] std::optional<rcc::TargetRelocationKindID> relocationKind() const noexcept {
    return Value.relocationKind();
  }

  [[nodiscard]] static rcc::Expected<CollidingConstant> parse(std::string_view Text) {
    auto Parsed = TestRelocConstant::parse(Text);
    if (!Parsed)
      return std::unexpected(Parsed.error());
    return CollidingConstant{std::move(*Parsed)};
  }

  [[nodiscard]] static rcc::Expected<CollidingConstant> decode(std::uint32_t Version,
                                                               std::span<const std::byte> Payload) {
    auto Decoded = TestRelocConstant::decode(Version, Payload);
    if (!Decoded)
      return std::unexpected(Decoded.error());
    return CollidingConstant{std::move(*Decoded)};
  }

  friend bool operator==(const CollidingConstant &Left, const CollidingConstant &Right) = default;
};

void testTargetConstantContract(TestContext &Context) {
  auto Constant = rcc::TargetConstant::create(TestRelocConstant{11, "symbol"});
  Context.expect(Constant.has_value(), "valid target constant must be type-erased");
  if (!Constant)
    return;

  Context.expect(Constant->typeID() == TestRelocConstant::stableTypeID() &&
                     Constant->schemaVersion() == TestRelocConstant::schemaVersion() &&
                     Constant->getIf<TestRelocConstant>() != nullptr &&
                     Constant->getIf<CollidingConstant>() == nullptr,
                 "strong query must require both stable and local type identity");
  auto Printed = Constant->print();
  auto RequiredAlignment = Constant->requiredAlignment();
  auto RelocationKind = Constant->relocationKind();
  Context.expect(Constant->verify().has_value() && Printed && *Printed == "symbol+11" &&
                     RequiredAlignment && *RequiredAlignment == *rcc::Alignment::create(8) &&
                     RelocationKind && *RelocationKind == rcc::TargetRelocationKindID::fromValue(9),
                 "target constant semantic hooks must remain available through erasure");

  auto Cloned = Constant->clone();
  Context.expect(Cloned && Constant->equals(*Cloned) &&
                     Cloned->getIf<TestRelocConstant>() != Constant->getIf<TestRelocConstant>(),
                 "target constant clone must own an equal independent value");

  auto Encoded = Constant->encode();
  if (!Encoded)
    return;
  auto Decoded = rcc::TargetConstant::decode<TestRelocConstant>(*Encoded);
  Context.expect(Encoded && Decoded && Constant->equals(*Decoded),
                 "versioned target constant encoding must round-trip losslessly");

  auto Parsed = rcc::TargetConstant::parse<TestRelocConstant>("parsed");
  const TestRelocConstant *ParsedValue = Parsed ? Parsed->getIf<TestRelocConstant>() : nullptr;
  Context.expect(ParsedValue != nullptr && ParsedValue->Addend == 7 &&
                     ParsedValue->Symbol == "parsed",
                 "target constant text parser must construct the same strong type");

  auto Emission = Constant->emit();
  Context.expect(Emission && Emission->Bytes.size() == 8 && Emission->Fixups.size() == 1 &&
                     Emission->Fixups[0].Symbol == "symbol" &&
                     Emission->Fixups[0].Kind == rcc::TargetRelocationKindID::fromValue(9),
                 "target constant emission must preserve bytes and structured fixups");
}

void testTargetConstantFailures(TestContext &Context) {
  rcc::TargetConstant Empty;
  Context.expect(!Empty.verify() && !Empty.clone() && !Empty.encode() && !Empty.emit(),
                 "empty target constant must reject every semantic operation");
  Context.expect(!rcc::TargetConstant::create(TestRelocConstant{0, ""}),
                 "target constant creation must verify its value");

  auto Constant = rcc::TargetConstant::create(TestRelocConstant{3, "symbol"});
  if (!Constant)
    return;
  auto Encoded = Constant->encode();
  if (!Encoded)
    return;
  Encoded->Version = 99;
  Context.expect(!rcc::TargetConstant::decode<TestRelocConstant>(*Encoded),
                 "target constant decode must reject unsupported versions");
  Encoded->Version = TestRelocConstant::schemaVersion();
  Encoded->TypeID = rcc::TargetConstantTypeID::fromValue(1);
  Context.expect(!rcc::TargetConstant::decode<TestRelocConstant>(*Encoded),
                 "target constant decode must reject a mismatched TypeID");

  rcc::TargetConstantEmission Invalid;
  Invalid.Bytes.resize(4);
  Invalid.Fixups.push_back({4, rcc::TargetRelocationKindID::fromValue(1), "symbol", 0});
  Context.expect(!Invalid.verify(), "target emission must reject out-of-range fixups");
  Invalid.Fixups[0].Offset = 0;
  Invalid.Fixups[0].Kind = {};
  Context.expect(!Invalid.verify(), "target emission must reject invalid relocation kinds");
  Invalid.Fixups[0].Kind = rcc::TargetRelocationKindID::fromValue(1);
  Invalid.Fixups.push_back(Invalid.Fixups[0]);
  Context.expect(!Invalid.verify(), "target emission must reject duplicate fixup offsets");
}

} // namespace

int main() {
  TestContext Context;
  testTargetConstantContract(Context);
  testTargetConstantFailures(Context);
  return Context.result();
}
