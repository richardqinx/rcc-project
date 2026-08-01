// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetFunctionState.h"

#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionCloner.h"
#include "rcc/CodeGen/MachineVerifier.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
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

struct TestFunctionInfo final {
  std::uint64_t StackProbeSize = 0;
  bool UsesShadowCallStack = false;
  std::string CallingConvention;

  [[nodiscard]] static constexpr rcc::TargetFunctionStateTypeID stableTypeID() noexcept {
    return rcc::TargetFunctionStateTypeID::fromValue(0x5243435446530001ULL);
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 3;
  }

  [[nodiscard]] rcc::Expected<void> verify() const {
    if (StackProbeSize == 0 || CallingConvention.empty()) {
      return rcc::makeUnexpected(rcc::ErrorCode::VerificationError,
                                 "test function info is incomplete");
    }
    return {};
  }

  [[nodiscard]] rcc::Expected<TestFunctionInfo>
  clone(const rcc::MachineFunctionCloneContext &) const {
    return TestFunctionInfo{StackProbeSize, UsesShadowCallStack, CallingConvention};
  }

  [[nodiscard]] std::string print() const {
    return CallingConvention + ":" + std::to_string(StackProbeSize) + ":" +
           (UsesShadowCallStack ? "shadow" : "plain");
  }

  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    std::vector<std::byte> Payload;
    Payload.reserve(9 + CallingConvention.size());
    for (std::size_t Index = 0; Index < 8; ++Index) {
      Payload.push_back(static_cast<std::byte>((StackProbeSize >> (Index * 8U)) & 0xffU));
    }
    Payload.push_back(static_cast<std::byte>(UsesShadowCallStack ? 1 : 0));
    for (const char RawCharacter : CallingConvention) {
      Payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(RawCharacter)));
    }
    return Payload;
  }

  [[nodiscard]] static rcc::Expected<TestFunctionInfo> decode(std::uint32_t Version,
                                                              std::span<const std::byte> Payload) {
    if (Version != schemaVersion()) {
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "unsupported test function info version");
    }
    if (Payload.size() <= 9 || std::to_integer<unsigned int>(Payload[8]) > 1) {
      return rcc::makeUnexpected(rcc::ErrorCode::ParseError,
                                 "test function info payload is invalid");
    }

    std::uint64_t StackProbeSize = 0;
    for (std::size_t Index = 0; Index < 8; ++Index) {
      StackProbeSize |= std::to_integer<std::uint64_t>(Payload[Index]) << (Index * 8U);
    }
    std::string CallingConvention;
    CallingConvention.reserve(Payload.size() - 9);
    for (const std::byte Character : Payload.subspan(9)) {
      CallingConvention.push_back(static_cast<char>(std::to_integer<unsigned char>(Character)));
    }
    return TestFunctionInfo{StackProbeSize, std::to_integer<unsigned int>(Payload[8]) != 0,
                            std::move(CallingConvention)};
  }

  friend bool operator==(const TestFunctionInfo &Left, const TestFunctionInfo &Right) = default;
};

struct CollidingFunctionInfo final {
  TestFunctionInfo Value;

  [[nodiscard]] static constexpr rcc::TargetFunctionStateTypeID stableTypeID() noexcept {
    return TestFunctionInfo::stableTypeID();
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return TestFunctionInfo::schemaVersion();
  }

  [[nodiscard]] rcc::Expected<void> verify() const {
    return Value.verify();
  }

  [[nodiscard]] rcc::Expected<CollidingFunctionInfo>
  clone(const rcc::MachineFunctionCloneContext &) const {
    return CollidingFunctionInfo{Value};
  }

  [[nodiscard]] std::string print() const {
    return Value.print();
  }

  [[nodiscard]] rcc::Expected<std::vector<std::byte>> encode() const {
    return Value.encode();
  }

  [[nodiscard]] static rcc::Expected<CollidingFunctionInfo>
  decode(std::uint32_t Version, std::span<const std::byte> Payload) {
    auto Decoded = TestFunctionInfo::decode(Version, Payload);
    if (!Decoded)
      return std::unexpected(Decoded.error());
    return CollidingFunctionInfo{std::move(*Decoded)};
  }

  friend bool operator==(const CollidingFunctionInfo &Left,
                         const CollidingFunctionInfo &Right) = default;
};

[[nodiscard]] rcc::Expected<rcc::InstructionSet> createInstructionSet() {
  rcc::InstructionDescriptor Descriptor;
  Descriptor.InstructionOpcode = rcc::Opcode::fromValue(1);
  Descriptor.Name = "TEST_NOP";
  Descriptor.Flags = {rcc::InstructionFlag::ProducesCode};
  std::vector<rcc::InstructionDescriptor> Descriptors;
  Descriptors.push_back(std::move(Descriptor));
  return rcc::InstructionSet::create(std::move(Descriptors));
}

void testTargetFunctionStateContract(TestContext &Context) {
  auto State = rcc::TargetFunctionState::create(TestFunctionInfo{4096, true, "lp64d"});
  Context.expect(State.has_value(), "valid target function state must be type-erased");
  if (!State)
    return;

  Context.expect(State->typeID() == TestFunctionInfo::stableTypeID() &&
                     State->schemaVersion() == TestFunctionInfo::schemaVersion() &&
                     State->getIf<TestFunctionInfo>() != nullptr &&
                     State->getIf<CollidingFunctionInfo>() == nullptr,
                 "strong query must require stable and local type identity");
  auto Printed = State->print();
  Context.expect(State->verify().has_value() && Printed && *Printed == "lp64d:4096:shadow",
                 "type-erased target state must preserve its semantic hooks");

  auto Encoded = State->encode();
  if (!Encoded)
    return;
  auto Decoded = rcc::TargetFunctionState::decode<TestFunctionInfo>(*Encoded);
  Context.expect(Decoded && State->equals(*Decoded) &&
                     State->getIf<TestFunctionInfo>() != Decoded->getIf<TestFunctionInfo>(),
                 "versioned target function state must round-trip losslessly");

  auto Collision = rcc::TargetFunctionState::create(
      CollidingFunctionInfo{TestFunctionInfo{4096, true, "lp64d"}});
  Context.expect(Collision && !State->equals(*Collision),
                 "colliding stable TypeIDs must not defeat local type safety");
}

void testTargetFunctionStateFailures(TestContext &Context) {
  rcc::TargetFunctionState Empty;
  Context.expect(!Empty.verify() && !Empty.print() && !Empty.encode(),
                 "empty target function state must reject semantic operations");
  Context.expect(!rcc::TargetFunctionState::create(TestFunctionInfo{0, false, ""}),
                 "target function state creation must verify its concrete value");

  auto State = rcc::TargetFunctionState::create(TestFunctionInfo{2048, false, "lp64"});
  if (!State)
    return;
  auto Encoded = State->encode();
  if (!Encoded)
    return;

  Encoded->Version = 0;
  Context.expect(!rcc::TargetFunctionState::decode<TestFunctionInfo>(*Encoded),
                 "target function state decode must reject version zero");
  Encoded->Version = 99;
  Context.expect(!rcc::TargetFunctionState::decode<TestFunctionInfo>(*Encoded),
                 "target function state decode must reject unsupported versions");
  Encoded->Version = TestFunctionInfo::schemaVersion();
  Encoded->TypeID = rcc::TargetFunctionStateTypeID::fromValue(1);
  Context.expect(!rcc::TargetFunctionState::decode<TestFunctionInfo>(*Encoded),
                 "target function state decode must reject a mismatched TypeID");
}

void testMachineFunctionOwnershipAndVerification(TestContext &Context) {
  auto InstructionSet = createInstructionSet();
  auto Function =
      rcc::MachineFunction::create("target_state", rcc::MachineFunctionStableID::fromValue(1));
  if (!InstructionSet || !Function)
    return;

  Context.expect(!(*Function)->setTargetState(rcc::TargetFunctionState{}),
                 "machine function must reject an empty target state");
  auto State = rcc::TargetFunctionState::create(TestFunctionInfo{4096, false, "lp64d"});
  Context.expect(State && (*Function)->setTargetState(std::move(*State)).has_value(),
                 "machine function must accept one verified target state");
  Context.expect((*Function)->getTargetInfo<TestFunctionInfo>() != nullptr &&
                     (*Function)->getTargetInfo<CollidingFunctionInfo>() == nullptr &&
                     rcc::MachineVerifier::verify(**Function, *InstructionSet).has_value(),
                 "machine function must expose and verify its strong target state");

  auto Replacement = rcc::TargetFunctionState::create(TestFunctionInfo{8192, true, "lp64d"});
  Context.expect(Replacement && !(*Function)->setTargetState(std::move(*Replacement)),
                 "machine function must not accept a second current target state");

  TestFunctionInfo *Info = (*Function)->getTargetInfo<TestFunctionInfo>();
  if (Info == nullptr)
    return;
  Info->CallingConvention.clear();
  Context.expect(!rcc::MachineVerifier::verify(**Function, *InstructionSet),
                 "machine verifier must reject invalid concrete target state");
}

} // namespace

int main() {
  TestContext Context;
  testTargetFunctionStateContract(Context);
  testTargetFunctionStateFailures(Context);
  testMachineFunctionOwnershipAndVerification(Context);
  return Context.result();
}
