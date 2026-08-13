// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineFunctionProperties.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/StableID.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <type_traits>

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
  const rcc::LowLevelType Invalid;
  Context.expect(!Invalid.isValid(), "default LowLevelType must be invalid");
  Context.expect(Invalid.toString() == "LLT_invalid",
                 "invalid LowLevelType spelling must be stable");

  const rcc::LowLevelType Token = rcc::LowLevelType::token();
  Context.expect(Token.isToken(), "token LowLevelType must preserve its kind");
  Context.expect(Token.scalarBitWidth() == 0, "token LowLevelType must have no bit width");
  Context.expect(Token.toString() == "s0", "token LowLevelType must use the MIR token spelling");

  auto Scalar = rcc::LowLevelType::makeCategoryNeutralScalar(3);
  auto SameScalar = rcc::LowLevelType::makeCategoryNeutralScalar(3);
  Context.expect(Scalar && SameScalar, "arbitrary positive scalar widths must be accepted");
  if (Scalar && SameScalar) {
    Context.expect(Scalar->isCategoryNeutralScalar(),
                   "scalar LowLevelType must preserve its category-neutral kind");
    Context.expect(*Scalar == *SameScalar,
                   "equal-width scalars must have one canonical low-level type");
    Context.expect(Scalar->toString() == "s3",
                   "scalar LowLevelType spelling must include its width");
  }

  Context.expect(!rcc::LowLevelType::makeCategoryNeutralScalar(0),
                 "zero-width scalar LowLevelType must be rejected");
  Context.expect(!rcc::LowLevelType::makeCategoryNeutralScalar(std::uint64_t{1} << 16),
                 "scalar LowLevelType wider than the encoding must be rejected");

  auto Pointer = rcc::LowLevelType::makePointer(rcc::LowLevelType::MaxAddressSpace, 64);
  Context.expect(Pointer.has_value(), "maximum encodable pointer address space must be accepted");
  if (Pointer) {
    Context.expect(Pointer->isPointer(), "pointer LowLevelType must preserve its kind");
    Context.expect(Pointer->addressSpace() == rcc::LowLevelType::MaxAddressSpace,
                   "pointer LowLevelType must preserve its address space");
    Context.expect(Pointer->toString() == "p16777215:64",
                   "pointer LowLevelType spelling must preserve address space and width");
  }
  Context.expect(!rcc::LowLevelType::makePointer(std::uint64_t{1} << 24, 64),
                 "pointer address spaces wider than the encoding must be rejected");

  auto I33 = rcc::LowLevelType::makeCategoryNeutralScalar(33);
  if (!I33) {
    Context.expect(false, "i33 construction must succeed before vector tests");
    return;
  }

  auto FixedVector = rcc::LowLevelType::makeFixedVector(4, *I33);
  Context.expect(FixedVector.has_value(), "fixed integer vector LowLevelType must be accepted");
  if (FixedVector) {
    Context.expect(FixedVector->isVector(), "fixed vector LowLevelType must preserve vector kind");
    Context.expect(!FixedVector->isScalableVector(),
                   "fixed vector LowLevelType must not be marked scalable");
    Context.expect(FixedVector->minimumElementCount() == 4,
                   "fixed vector LowLevelType must preserve element count");
    Context.expect(FixedVector->scalarElementType() == I33,
                   "fixed vector LowLevelType must preserve its scalar element type");
    Context.expect(FixedVector->toString() == "<4 x s33>",
                   "fixed vector LowLevelType spelling must be stable");
  }

  auto S16 = rcc::LowLevelType::makeCategoryNeutralScalar(16);
  if (!S16) {
    Context.expect(false, "s16 construction must succeed before vector tests");
    return;
  }
  auto ScalableVector = rcc::LowLevelType::makeScalableVector(1, *S16);
  Context.expect(ScalableVector.has_value(), "scalable vector with one minimum lane must work");
  if (ScalableVector) {
    Context.expect(ScalableVector->isScalableVector(),
                   "scalable vector LowLevelType must preserve scalability");
    Context.expect(ScalableVector->isVectorValue(),
                   "category-neutral vectors must preserve their value kind");
    Context.expect(ScalableVector->scalarElementType() == S16,
                   "scalable vectors must preserve their element type");
    Context.expect(ScalableVector->toString() == "<vscale x 1 x s16>",
                   "scalable vector spelling must preserve its shape");
  }

  Context.expect(!rcc::LowLevelType::makeFixedVector(1, *I33),
                 "single-lane fixed vectors must be rejected");
  Context.expect(!rcc::LowLevelType::makeScalableVector(0, *I33),
                 "zero-lane scalable vectors must be rejected");
  Context.expect(!rcc::LowLevelType::makeFixedVector(4, Token),
                 "token vector elements must be rejected");
  if (FixedVector) {
    Context.expect(!rcc::LowLevelType::makeFixedVector(4, *FixedVector),
                   "nested LowLevelType vectors must be rejected");
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
