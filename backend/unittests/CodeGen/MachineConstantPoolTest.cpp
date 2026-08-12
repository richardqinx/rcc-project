// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineConstantPool.h"

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

struct PoolTargetConstant final {
  std::uint64_t Value = 0;
  std::string Symbol;

  [[nodiscard]] static constexpr rcc::TargetConstantTypeID stableTypeID() noexcept {
    return rcc::TargetConstantTypeID::fromValue(0x5253434350430001ULL);
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 1;
  }

  [[nodiscard]] rcc::Expected<void> verify() const {
    if (Value == 0) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "pool target constant value must be nonzero");
    }
    return {};
  }

  [[nodiscard]] rcc::Expected<PoolTargetConstant> clone() const {
    return PoolTargetConstant{Value, Symbol};
  }

  [[nodiscard]] std::uint64_t stableHash() const noexcept {
    std::uint64_t Hash = Value;
    for (const char Character : Symbol)
      Hash = (Hash * 16777619U) ^ static_cast<unsigned char>(Character);
    return Hash;
  }

  [[nodiscard]] std::string print() const {
    return Symbol.empty() ? std::to_string(Value) : Symbol + "+" + std::to_string(Value);
  }

  [[nodiscard]] std::vector<std::byte> bytes() const {
    std::vector<std::byte> Bytes(8);
    for (std::size_t Index = 0; Index < Bytes.size(); ++Index)
      Bytes[Index] = static_cast<std::byte>((Value >> (Index * 8U)) & 0xffU);
    return Bytes;
  }

  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    std::vector<std::byte> Payload = bytes();
    for (const char Character : Symbol)
      Payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
    return Payload;
  }

  [[nodiscard]] rcc::Expected<rcc::TargetConstantEmission> emit() const {
    rcc::TargetConstantEmission Emission;
    Emission.Bytes = bytes();
    if (!Symbol.empty()) {
      Emission.Fixups.push_back({0, rcc::TargetRelocationKindID::fromValue(3), Symbol,
                                 std::bit_cast<std::int64_t>(Value)});
    }
    return Emission;
  }

  [[nodiscard]] rcc::Alignment requiredAlignment() const noexcept {
    return *rcc::Alignment::create(8);
  }

  [[nodiscard]] std::optional<rcc::TargetRelocationKindID> relocationKind() const noexcept {
    if (Symbol.empty())
      return std::nullopt;
    return rcc::TargetRelocationKindID::fromValue(3);
  }

  [[nodiscard]] static rcc::Expected<PoolTargetConstant> parse(std::string_view Text) {
    if (Text.empty())
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError, "pool constant text is empty");
    return PoolTargetConstant{1, std::string(Text)};
  }

  [[nodiscard]] static rcc::Expected<PoolTargetConstant>
  decode(std::uint32_t Version, std::span<const std::byte> Payload) {
    if (Version != schemaVersion() || Payload.size() < 8) {
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "pool target constant encoding is invalid");
    }
    std::uint64_t Value = 0;
    for (std::size_t Index = 0; Index < 8; ++Index)
      Value |= std::to_integer<std::uint64_t>(Payload[Index]) << (Index * 8U);
    std::string Symbol;
    for (const std::byte Character : Payload.subspan(8))
      Symbol.push_back(static_cast<char>(std::to_integer<unsigned char>(Character)));
    return PoolTargetConstant{Value, std::move(Symbol)};
  }

  friend bool operator==(const PoolTargetConstant &Left, const PoolTargetConstant &Right) = default;
};

[[nodiscard]] rcc::MachineConstantPoolEntryDesc
genericDescription(rcc::ConstantPoolMergeKind Merge) {
  rcc::MachineConstantPoolEntryDesc Description;
  auto Integer = rcc::APInt::fromUnsigned(32, 0x04030201U);
  Description.Value = std::move(*rcc::GenericConstantData::integer(std::move(*Integer)));
  Description.Size = 4;
  Description.Align = *rcc::Alignment::create(4);
  Description.Merge = Merge;
  if (Merge == rcc::ConstantPoolMergeKind::Exact)
    Description.Placement.Section = rcc::ConstantPoolSectionKind::MergeableReadOnly;
  return Description;
}

[[nodiscard]] rcc::MachineConstantPoolEntryDesc targetDescription(bool Expression) {
  rcc::MachineConstantPoolEntryDesc Description;
  auto Constant = rcc::TargetConstant::create(
      PoolTargetConstant{Expression ? 17U : 9U, Expression ? "symbol" : ""});
  if (Expression)
    Description.Value = rcc::TargetExpressionConstant{std::move(*Constant)};
  else
    Description.Value = std::move(*Constant);
  Description.Size = 8;
  Description.Align = *rcc::Alignment::create(8);
  Description.Relocation =
      Expression ? rcc::ConstantPoolRelocationMode::Target : rcc::ConstantPoolRelocationMode::None;
  return Description;
}

[[nodiscard]] rcc::MachineConstantPoolEntryDesc symbolDifferenceDescription() {
  rcc::MachineConstantPoolEntryDesc Description;
  Description.Value = rcc::SymbolDifferenceConstant{"upper", "lower", 4};
  Description.Size = 4;
  Description.Align = *rcc::Alignment::create(4);
  Description.Relocation = rcc::ConstantPoolRelocationMode::PositionIndependent;
  return Description;
}

[[nodiscard]] rcc::MachineConstantPoolEntryDesc blockAddressDescription() {
  rcc::MachineConstantPoolEntryDesc Description;
  Description.Value = rcc::BlockAddressConstant{"function", "block", 0};
  Description.Size = 8;
  Description.Align = *rcc::Alignment::create(8);
  Description.Relocation = rcc::ConstantPoolRelocationMode::Static;
  return Description;
}

void testPoolLifecycle(TestContext &Context) {
  rcc::MachineConstantPool Pool;

  auto MergedA = Pool.addEntry(genericDescription(rcc::ConstantPoolMergeKind::Exact));
  auto MergedB = Pool.addEntry(genericDescription(rcc::ConstantPoolMergeKind::Exact));
  auto Unmerged = Pool.addEntry(genericDescription(rcc::ConstantPoolMergeKind::Never));
  Context.expect(MergedA && MergedB && Unmerged && *MergedA == *MergedB && MergedA->value() == 0 &&
                     Unmerged->value() == 1 && Pool.entries().size() == 2 &&
                     Pool.liveEntryCount() == 2,
                 "exact constants must merge while nonmergeable constants retain identity");
  if (!MergedA || !Unmerged)
    return;

  Context.expect(
      !Pool.updateEntry(*Unmerged, genericDescription(rcc::ConstantPoolMergeKind::Exact)),
      "pool update must reject duplicate mergeable entries");
  Context.expect(Pool.eraseEntry(*Unmerged).has_value() && Pool.entry(*Unmerged) == nullptr,
                 "constant pool erase must leave a tombstone");

  auto Target = Pool.addEntry(targetDescription(false));
  auto Difference = Pool.addEntry(symbolDifferenceDescription());
  auto Address = Pool.addEntry(blockAddressDescription());
  auto Expression = Pool.addEntry(targetDescription(true));
  Context.expect(Target && Difference && Address && Expression && Target->value() == 2 &&
                     Expression->value() == 5 && Pool.liveEntryCount() == 5,
                 "all constant pool value kinds must receive monotonic indices");
  if (!Target || !Expression)
    return;

  const rcc::MachineConstantPoolEntry *TargetEntry = Pool.entry(*Target);
  auto ClonedDescription =
      TargetEntry == nullptr ? rcc::Expected<rcc::MachineConstantPoolEntryDesc>(rcc::makeUnexpected(
                                   rcc::ErrorCode::InternalError, "missing target entry"))
                             : TargetEntry->cloneDescription();
  Context.expect(ClonedDescription &&
                     TargetEntry->description()->semanticallyEquals(*ClonedDescription),
                 "target constant pool descriptions must deep-clone");

  Context.expect(Pool.verify().has_value() && Pool.freeze().has_value() && Pool.isFrozen(),
                 "valid constant pool must freeze after verification");
  Context.expect(!Pool.addEntry(blockAddressDescription()) && !Pool.eraseEntry(*Target) &&
                     !Pool.updateEntry(*Expression, targetDescription(true)) && !Pool.freeze(),
                 "frozen constant pool must reject every mutation");
}

void testInvalidEntries(TestContext &Context) {
  rcc::MachineConstantPool Pool;

  auto SizeMismatch = genericDescription(rcc::ConstantPoolMergeKind::Never);
  SizeMismatch.Size = 8;
  Context.expect(!Pool.addEntry(std::move(SizeMismatch)),
                 "generic constant byte size must match its entry size");

  auto RelocatingBytes = genericDescription(rcc::ConstantPoolMergeKind::Never);
  RelocatingBytes.Relocation = rcc::ConstantPoolRelocationMode::Static;
  Context.expect(!Pool.addEntry(std::move(RelocatingBytes)),
                 "raw generic bytes must reject implicit relocation semantics");

  auto WeakTargetAlignment = targetDescription(false);
  WeakTargetAlignment.Align = *rcc::Alignment::create(4);
  Context.expect(!Pool.addEntry(std::move(WeakTargetAlignment)),
                 "target constant must enforce its required alignment");

  auto MissingTargetRelocation = targetDescription(true);
  MissingTargetRelocation.Relocation = rcc::ConstantPoolRelocationMode::None;
  Context.expect(!Pool.addEntry(std::move(MissingTargetRelocation)),
                 "target expression must agree with its relocation contract");

  auto InvalidPlacement = blockAddressDescription();
  InvalidPlacement.Placement.Section = rcc::ConstantPoolSectionKind::Target;
  Context.expect(!Pool.addEntry(std::move(InvalidPlacement)),
                 "target placement must carry target flags");

  auto ZeroDistance = blockAddressDescription();
  ZeroDistance.Placement.MaximumDistance = 0;
  Context.expect(!Pool.addEntry(std::move(ZeroDistance)),
                 "constant pool maximum distance must be nonzero");
}

} // namespace

int main() {
  TestContext Context;
  testPoolLifecycle(Context);
  testInvalidEntries(Context);
  return Context.result();
}
