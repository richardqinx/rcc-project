// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <unordered_set>

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

void testLowLevelTypes(TestContext &Context) {
  const rcc::LLT Invalid;
  Context.expect(!Invalid.isValid(), "default LLT must be invalid");
  Context.expect(Invalid.toString() == "LLT_invalid", "invalid LLT spelling must be stable");

  const rcc::LLT Token = rcc::LLT::token();
  Context.expect(Token.isToken(), "token LLT must preserve its kind");
  Context.expect(Token.scalarBitWidth() == 0, "token LLT must have no bit width");
  Context.expect(Token.toString() == "s0", "token LLT must use the MIR token spelling");

  auto Any = rcc::LLT::makeAnyScalar(3);
  auto Integer = rcc::LLT::makeInteger(3);
  Context.expect(Any && Integer, "arbitrary positive scalar widths must be accepted");
  if (Any && Integer) {
    Context.expect(Any->isAnyScalar(), "bag-of-bits LLT must preserve its kind");
    Context.expect(Integer->isInteger(), "integer LLT must preserve its kind");
    Context.expect(*Any != *Integer, "bag-of-bits and integer LLTs must remain distinct");
    Context.expect(Any->toString() == "s3", "bag-of-bits LLT spelling must include its width");
    Context.expect(Integer->toString() == "i3", "integer LLT spelling must include its width");
  }

  Context.expect(!rcc::LLT::makeInteger(0), "zero-width integer LLT must be rejected");
  Context.expect(!rcc::LLT::makeInteger(std::uint64_t{1} << 16),
                 "integer LLT wider than the encoding must be rejected");

  constexpr std::array FloatCases{
      std::pair{rcc::FloatSemantics::IEEEHalf, std::uint32_t{16}},
      std::pair{rcc::FloatSemantics::BFloat, std::uint32_t{16}},
      std::pair{rcc::FloatSemantics::IEEESingle, std::uint32_t{32}},
      std::pair{rcc::FloatSemantics::IEEEDouble, std::uint32_t{64}},
      std::pair{rcc::FloatSemantics::IEEEQuad, std::uint32_t{128}},
      std::pair{rcc::FloatSemantics::PPCDoubleDouble, std::uint32_t{128}},
      std::pair{rcc::FloatSemantics::Float8E5M2, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float8E5M2FNUZ, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float8E4M3, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float8E4M3FN, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float8E4M3FNUZ, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float8E4M3B11FNUZ, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float8E3M4, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::FloatTF32, std::uint32_t{19}},
      std::pair{rcc::FloatSemantics::Float8E8M0FNU, std::uint32_t{8}},
      std::pair{rcc::FloatSemantics::Float6E3M2FN, std::uint32_t{6}},
      std::pair{rcc::FloatSemantics::Float6E2M3FN, std::uint32_t{6}},
      std::pair{rcc::FloatSemantics::Float4E2M1FN, std::uint32_t{4}},
      std::pair{rcc::FloatSemantics::X87DoubleExtended, std::uint32_t{80}},
  };

  std::unordered_set<rcc::LLT> FloatTypes;
  for (const auto &[Semantics, BitWidth] : FloatCases) {
    auto Type = rcc::LLT::makeFloat(Semantics);
    Context.expect(Type.has_value(), "known floating-point semantics must be accepted");
    if (!Type)
      continue;
    Context.expect(Type->scalarBitWidth() == BitWidth,
                   "float LLT must preserve its semantics bit width");
    Context.expect(Type->floatSemantics() == Semantics,
                   "float LLT must preserve exact floating-point semantics");
    FloatTypes.insert(*Type);
  }
  Context.expect(FloatTypes.size() == FloatCases.size(),
                 "distinct floating-point semantics must produce distinct LLTs");
  Context.expect(!rcc::LLT::makeFloat(rcc::FloatSemantics::Count),
                 "unknown floating-point semantics must be rejected");

  auto Pointer = rcc::LLT::makePointer(rcc::LLT::MaxAddressSpace, 64);
  Context.expect(Pointer.has_value(), "maximum encodable pointer address space must be accepted");
  if (Pointer) {
    Context.expect(Pointer->isPointer(), "pointer LLT must preserve its kind");
    Context.expect(Pointer->addressSpace() == rcc::LLT::MaxAddressSpace,
                   "pointer LLT must preserve its address space");
    Context.expect(Pointer->toString() == "p16777215:64",
                   "pointer LLT spelling must preserve address space and width");
  }
  Context.expect(!rcc::LLT::makePointer(std::uint64_t{1} << 24, 64),
                 "pointer address spaces wider than the encoding must be rejected");

  auto I33 = rcc::LLT::makeInteger(33);
  if (!I33) {
    Context.expect(false, "i33 construction must succeed before vector tests");
    return;
  }

  auto FixedVector = rcc::LLT::makeFixedVector(4, *I33);
  Context.expect(FixedVector.has_value(), "fixed integer vector LLT must be accepted");
  if (FixedVector) {
    Context.expect(FixedVector->isVector(), "fixed vector LLT must preserve vector kind");
    Context.expect(!FixedVector->isScalableVector(),
                   "fixed vector LLT must not be marked scalable");
    Context.expect(FixedVector->minimumElementCount() == 4,
                   "fixed vector LLT must preserve element count");
    Context.expect(FixedVector->scalarElementType() == I33,
                   "fixed vector LLT must preserve its scalar element type");
    Context.expect(FixedVector->toString() == "<4 x i33>",
                   "fixed vector LLT spelling must be stable");
  }

  auto BFloat = rcc::LLT::makeFloat(rcc::FloatSemantics::BFloat);
  if (!BFloat) {
    Context.expect(false, "bfloat construction must succeed before vector tests");
    return;
  }
  auto ScalableVector = rcc::LLT::makeScalableVector(1, *BFloat);
  Context.expect(ScalableVector.has_value(), "scalable vector with one minimum lane must work");
  if (ScalableVector) {
    Context.expect(ScalableVector->isScalableVector(),
                   "scalable vector LLT must preserve scalability");
    Context.expect(ScalableVector->isFloatVector() && !ScalableVector->isFloat(),
                   "float vectors must not be classified as scalar floats");
    Context.expect(ScalableVector->floatSemantics() == rcc::FloatSemantics::BFloat,
                   "float vectors must preserve exact floating-point semantics");
    Context.expect(ScalableVector->toString() == "<vscale x 1 x bf16>",
                   "scalable float vector spelling must preserve exact semantics");
  }

  Context.expect(!rcc::LLT::makeFixedVector(1, *I33), "single-lane fixed vectors must be rejected");
  Context.expect(!rcc::LLT::makeScalableVector(0, *I33),
                 "zero-lane scalable vectors must be rejected");
  Context.expect(!rcc::LLT::makeFixedVector(4, Token), "token vector elements must be rejected");
  if (FixedVector) {
    Context.expect(!rcc::LLT::makeFixedVector(4, *FixedVector),
                   "nested LLT vectors must be rejected");
  }
}

void testRegisters(TestContext &Context) {
  const rcc::Register NoRegister{0};
  Context.expect(NoRegister.isNoRegister(), "Register{0} must represent NoRegister");
  Context.expect(NoRegister.toString() == "noreg", "NoRegister spelling must be stable");

  auto PhysReg = rcc::PhysicalRegister::create(7);
  Context.expect(PhysReg.has_value(), "positive physical register numbers must be accepted");
  if (PhysReg) {
    const rcc::Register Reg = rcc::Register::fromPhysical(*PhysReg);
    Context.expect(Reg.isPhysical(), "physical Register must preserve its category");
    Context.expect(Reg.physicalRegister() == PhysReg,
                   "physical Register must round-trip through PhysicalRegister");
    Context.expect(Reg.toString() == "$physreg7",
                   "target-independent physical register spelling must be stable");
  }

  Context.expect(!rcc::PhysicalRegister::create(0), "physical register zero must remain reserved");
  Context.expect(!rcc::Register::makePhysical(std::uint64_t{1} << 31),
                 "out-of-range physical registers must be rejected");

  auto FirstVirtual = rcc::Register::makeVirtual(0);
  auto LastVirtual = rcc::Register::makeVirtual((std::uint64_t{1} << 31) - 1);
  Context.expect(FirstVirtual && LastVirtual,
                 "the complete virtual register index domain must be accepted");
  if (FirstVirtual) {
    Context.expect(FirstVirtual->isVirtual(), "virtual Register must preserve its category");
    Context.expect(FirstVirtual->virtualIndex() == 0, "virtual Register must preserve index zero");
    Context.expect(FirstVirtual->toString() == "%0",
                   "virtual Register spelling must match MIR numbering");
  }
  Context.expect(!rcc::Register::makeVirtual(std::uint64_t{1} << 31),
                 "out-of-range virtual register indices must be rejected");
}

void testStableIDs(TestContext &Context) {
  static_assert(!std::is_same_v<rcc::MachineFunctionStableID, rcc::MachineBlockStableID>);
  static_assert(!std::is_same_v<rcc::MachineBlockStableID, rcc::MachineInstrStableID>);
  static_assert(!std::is_copy_constructible_v<rcc::StableIDGenerator<rcc::MachineInstrStableID>>);

  const rcc::MachineInstrStableID Invalid;
  Context.expect(!Invalid.isValid(), "zero StableID must be invalid");

  rcc::StableIDGenerator<rcc::MachineInstrStableID> Generator;
  auto First = Generator.allocate();
  auto Second = Generator.allocate();
  Context.expect(First && Second, "StableID generator must allocate IDs");
  if (First && Second) {
    Context.expect(First->value() == 1 && Second->value() == 2,
                   "StableID generator must allocate monotonically");
    Context.expect(*First != *Second, "StableID generator must not reuse live IDs");
  }
}

void testMachineFunctionProperties(TestContext &Context) {
  using enum rcc::MachineFunctionProperty;

  rcc::MachineFunctionProperties Properties{IsSSA, TracksLiveness};
  Context.expect(Properties.count() == 2, "property set must count active properties");
  Context.expect(Properties.has(IsSSA), "property set must retain IsSSA");
  Context.expect(Properties.toString() == "IsSSA, TracksLiveness",
                 "property spelling must have deterministic enum order");

  const auto Original = Properties;
  auto MissingRequired =
      Properties.applyTransition(rcc::MachineFunctionProperties{Legalized},
                                 rcc::MachineFunctionProperties{RegBankSelected}, {});
  Context.expect(!MissingRequired, "property transition must reject missing requirements");
  Context.expect(Properties == Original, "failed property transition must be atomic");
  if (!MissingRequired) {
    Context.expect(MissingRequired.error().code() == rcc::ErrorCode::VerificationError,
                   "missing property requirements must be verification errors");
  }

  auto ConflictingTransition = Properties.applyTransition(
      {}, rcc::MachineFunctionProperties{Selected}, rcc::MachineFunctionProperties{Selected});
  Context.expect(!ConflictingTransition,
                 "property transition must reject overlapping sets and clears");
  Context.expect(Properties == Original, "conflicting property transition must be atomic");

  auto ValidTransition = Properties.applyTransition(rcc::MachineFunctionProperties{IsSSA},
                                                    rcc::MachineFunctionProperties{NoPHIs},
                                                    rcc::MachineFunctionProperties{IsSSA});
  Context.expect(ValidTransition.has_value(), "valid property transition must succeed");
  Context.expect(Properties.has(NoPHIs) && !Properties.has(IsSSA),
                 "valid property transition must apply sets and clears together");

  auto RawRoundTrip = rcc::MachineFunctionProperties::fromRaw(Properties.rawValue());
  Context.expect(RawRoundTrip && *RawRoundTrip == Properties,
                 "known property masks must round-trip");
  Context.expect(!rcc::MachineFunctionProperties::fromRaw(std::uint64_t{1} << 63),
                 "unknown property bits must be rejected");
}

} // namespace

int main() {
  TestContext Context;
  testLowLevelTypes(Context);
  testRegisters(Context);
  testStableIDs(Context);
  testMachineFunctionProperties(Context);
  return Context.result();
}
