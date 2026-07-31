// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/RegisterMask.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <type_traits>
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

void testStrongValues(TestContext &Context) {
  static_assert(!std::is_same_v<rcc::FrameIndex, rcc::ConstantPoolIndex>);
  static_assert(!std::is_same_v<rcc::ConstantPoolIndex, rcc::JumpTableIndex>);
  static_assert(!std::is_same_v<rcc::RegisterBankID, rcc::RegisterClassID>);

  const auto Frame = rcc::FrameIndex::fromValue(7);
  const auto Constant = rcc::ConstantPoolIndex::fromValue(7);
  Context.expect(Frame.value() == 7 && Constant.value() == 7,
                 "strong machine indices must preserve their values");

  const auto Opcode = rcc::Opcode::fromValue(19);
  Context.expect(Opcode.isValid() && Opcode.value() == 19, "Opcode must preserve its opaque value");
  Context.expect(!rcc::Opcode{}.isValid(), "Opcode zero must remain invalid");

  const auto LowLanes = rcc::LaneMask::fromRaw(0x3);
  const auto HighLanes = rcc::LaneMask::fromRaw(0x6);
  Context.expect(LowLanes.intersects(HighLanes), "overlapping lane masks must intersect");
  Context.expect((LowLanes | HighLanes).rawValue() == 0x7,
                 "lane mask union must preserve all lanes");
}

void testBranchProbability(TestContext &Context) {
  auto Half = rcc::BranchProbability::fromFraction(1, 2);
  Context.expect(Half.has_value(), "valid branch probability fraction must be accepted");
  if (Half) {
    const auto Sum = Half->saturatingAdd(*Half);
    const auto RawDistance = rcc::BranchProbability::one().rawValue() - Sum.rawValue();
    Context.expect(RawDistance <= 1,
                   "two rounded half probabilities must sum to one within one unit");
    Context.expect(Half->complement().rawValue() == Half->rawValue() ||
                       Half->complement().rawValue() + 1 == Half->rawValue(),
                   "half probability complement must preserve fixed-point rounding");
  }

  Context.expect(!rcc::BranchProbability::fromFraction(1, 0),
                 "zero branch probability denominator must be rejected");
  Context.expect(!rcc::BranchProbability::fromFraction(3, 2),
                 "branch probability greater than one must be rejected");
}

void testRegisterMask(TestContext &Context) {
  std::vector<std::uint64_t> Words(2);
  Words[0] = std::uint64_t{1} << 7;
  Words[1] = std::uint64_t{1} << 6;
  const rcc::RegisterMask Mask(std::move(Words));

  auto Reg7 = rcc::PhysicalRegister::create(7);
  auto Reg70 = rcc::PhysicalRegister::create(70);
  auto Reg71 = rcc::PhysicalRegister::create(71);
  Context.expect(Reg7 && Reg70 && Reg71, "physical registers for mask test must be valid");
  if (Reg7 && Reg70 && Reg71) {
    Context.expect(Mask.preserves(*Reg7), "preserved register mask bit must be one");
    Context.expect(Mask.preserves(*Reg70), "preserved register mask must cross word boundaries");
    Context.expect(Mask.clobbers(*Reg71), "clear register mask bit must mean clobbered");
  }
}

rcc::MachineMemOperandDesc makeBasicLoadDescription() {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = rcc::MemoryAccessFlags{
      rcc::MemoryAccessFlag::Load,
      rcc::MemoryAccessFlag::Dereferenceable,
  };
  Description.Size = *rcc::MemorySize::fixed(8);
  Description.Align = *rcc::Alignment::create(8);
  Description.AddressSpace = 0;
  Description.Base = rcc::MemoryBase::frameIndex(rcc::FrameIndex::fromValue(3));
  Description.Offset = 16;
  Description.AliasSource = rcc::MemoryAliasSourceID::fromValue(11);
  return Description;
}

void testMemoryValueTypes(TestContext &Context) {
  Context.expect(!rcc::Alignment::create(0), "zero alignment must be rejected");
  Context.expect(!rcc::Alignment::create(3), "non-power-of-two alignment must be rejected");
  Context.expect(rcc::Alignment::create(16).has_value(), "power-of-two alignment must be accepted");
  Context.expect(!rcc::MemorySize::fixed(0), "zero fixed memory size must be rejected");
  Context.expect(!rcc::MemorySize::scalable(0), "zero scalable memory minimum must be rejected");

  auto Range = rcc::MemoryOffsetRange::create(-4, 12);
  Context.expect(Range && Range->Lower == -4 && Range->Upper == 12,
                 "valid memory offset range must preserve both bounds");
  Context.expect(!rcc::MemoryOffsetRange::create(1, -1),
                 "reversed memory offset range must be rejected");

  const auto Symbol = rcc::MemoryBase::globalSymbol("global");
  Context.expect(Symbol.kind() == rcc::MemoryBase::Kind::GlobalSymbol,
                 "global memory base must preserve its kind");
  Context.expect(Symbol.symbolName() == "global", "global memory base must preserve its symbol");
}

void testMachineMemOperand(TestContext &Context) {
  auto Load = rcc::MachineMemOperand::create(makeBasicLoadDescription());
  Context.expect(Load.has_value(), "complete ordinary load MMO must be accepted");
  if (Load) {
    Context.expect(Load->isLoad() && !Load->isStore(),
                   "ordinary load MMO must preserve access direction");
    Context.expect(!Load->isAtomic() && !Load->isVolatile(),
                   "ordinary load MMO must remain unordered and non-volatile");
  }

  rcc::MachineMemOperandDesc NoAccess;
  Context.expect(!rcc::MachineMemOperand::create(NoAccess),
                 "MMO without load or store access must be rejected");

  auto InvariantStore = makeBasicLoadDescription();
  InvariantStore.Accesses.set(rcc::MemoryAccessFlag::Store);
  InvariantStore.Accesses.set(rcc::MemoryAccessFlag::Invariant);
  Context.expect(!rcc::MachineMemOperand::create(InvariantStore),
                 "invariant store MMO must be rejected");

  auto AtomicLoad = makeBasicLoadDescription();
  AtomicLoad.AtomicKind = rcc::AtomicOperation::Load;
  AtomicLoad.SuccessOrdering = rcc::AtomicOrdering::Acquire;
  Context.expect(rcc::MachineMemOperand::create(AtomicLoad).has_value(),
                 "acquire atomic load MMO must be accepted");

  AtomicLoad.SuccessOrdering = rcc::AtomicOrdering::Release;
  Context.expect(!rcc::MachineMemOperand::create(AtomicLoad),
                 "release ordering on atomic load must be rejected");

  auto CompareExchange = makeBasicLoadDescription();
  CompareExchange.Accesses.set(rcc::MemoryAccessFlag::Store);
  CompareExchange.AtomicKind = rcc::AtomicOperation::CompareExchange;
  CompareExchange.SuccessOrdering = rcc::AtomicOrdering::AcquireRelease;
  CompareExchange.FailureOrdering = rcc::AtomicOrdering::Acquire;
  Context.expect(rcc::MachineMemOperand::create(CompareExchange).has_value(),
                 "compatible compare-exchange orderings must be accepted");

  CompareExchange.SuccessOrdering = rcc::AtomicOrdering::Release;
  Context.expect(!rcc::MachineMemOperand::create(CompareExchange),
                 "failure ordering stronger than success must be rejected");

  auto BadAddressSpace = makeBasicLoadDescription();
  BadAddressSpace.AddressSpace = rcc::LLT::MaxAddressSpace + 1;
  Context.expect(!rcc::MachineMemOperand::create(BadAddressSpace),
                 "MMO address space wider than LLT encoding must be rejected");

  auto EmptySymbol = makeBasicLoadDescription();
  EmptySymbol.Base = rcc::MemoryBase::externalSymbol("");
  Context.expect(!rcc::MachineMemOperand::create(EmptySymbol),
                 "empty MMO symbol base must be rejected");
}

} // namespace

int main() {
  TestContext Context;
  testStrongValues(Context);
  testBranchProbability(Context);
  testRegisterMask(Context);
  testMemoryValueTypes(Context);
  testMachineMemOperand(Context);
  return Context.result();
}
