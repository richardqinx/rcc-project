// SPDX-License-Identifier: GPL-3.0-or-later

#include "RISCV.h"
#include "rcc/CodeGen/LiveIntervals.h"
#include "rcc/CodeGen/LiveVariables.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/RegAllocFast.h"
#include "rcc/CodeGen/RegAllocPBQP.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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
    ++Failures;
  }

  [[nodiscard]] int result() const noexcept {
    return Failures == 0 ? 0 : 1;
  }

private:
  int Failures = 0;
};

[[nodiscard]] constexpr rcc::Opcode targetOpcode(rcc::riscv::InstructionOpcode Value) noexcept {
  return rcc::Opcode::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] constexpr rcc::RegisterClassID
registerClass(rcc::riscv::RegisterClass Value) noexcept {
  return rcc::RegisterClassID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] constexpr rcc::RegisterBankID registerBank(rcc::riscv::RegisterBank Value) noexcept {
  return rcc::RegisterBankID::fromValue(static_cast<std::uint32_t>(Value));
}

[[nodiscard]] rcc::Register physical(rcc::riscv::PhysicalRegisterID ID) {
  return rcc::Register::fromPhysical(*rcc::riscv::physicalRegister(ID));
}

struct FunctionFixture final {
  std::unique_ptr<rcc::MachineFunction> Function;
  rcc::MachineBasicBlock *Block = nullptr;
};

[[nodiscard]] rcc::Expected<FunctionFixture> createFunction(std::string Name, std::uint64_t ID) {
  auto Function =
      rcc::MachineFunction::create(std::move(Name), rcc::MachineFunctionStableID::fromValue(ID));
  if (!Function)
    return std::unexpected(Function.error());
  auto Block = (*Function)->createBasicBlock();
  if (!Block)
    return std::unexpected(Block.error());
  auto Appended = (*Function)->appendBlock(**Block);
  if (!Appended)
    return std::unexpected(Appended.error());
  return FunctionFixture{std::move(*Function), *Block};
}

[[nodiscard]] rcc::Expected<rcc::Register> createVReg(rcc::MachineFunction &Function, rcc::LLT Type,
                                                      rcc::riscv::RegisterBank Bank,
                                                      rcc::riscv::RegisterClass Class) {
  auto Reg = Function.registerInfo().createVirtualRegister(Type);
  if (!Reg)
    return std::unexpected(Reg.error());
  if (auto Result = Function.registerInfo().setRegisterBank(*Reg, registerBank(Bank)); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Function.registerInfo().setRegisterClass(*Reg, registerClass(Class)); !Result)
    return std::unexpected(Result.error());
  return *Reg;
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildAddImmediate(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
                  rcc::MachineBasicBlock &Block, rcc::Register Destination, rcc::Register Source,
                  std::int64_t Immediate) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(targetOpcode(rcc::riscv::InstructionOpcode::ADDI));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Source); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(Immediate); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *> buildAdd(rcc::MachineFunction &Function,
                                                          const rcc::TargetDescription &Target,
                                                          rcc::MachineBasicBlock &Block,
                                                          rcc::Register Destination,
                                                          rcc::Register Left, rcc::Register Right) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(targetOpcode(rcc::riscv::InstructionOpcode::ADD));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Left); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Right); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<void> prepareForAllocation(rcc::MachineFunction &Function,
                                                       const rcc::TargetDescription &Target) {
  auto Rebuilt = rcc::rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  for (const rcc::MachineFunctionProperty Property : {
           rcc::MachineFunctionProperty::TracksLiveness,
           rcc::MachineFunctionProperty::Legalized,
           rcc::MachineFunctionProperty::RegBankSelected,
           rcc::MachineFunctionProperty::Selected,
           rcc::MachineFunctionProperty::NoPHIs,
           rcc::MachineFunctionProperty::TiedOpsRewritten,
       }) {
    Function.properties().set(Property);
  }
  return {};
}

enum class AllocatorKind : std::uint8_t {
  Basic,
  Fast,
  PBQP,
};

[[nodiscard]] rcc::Expected<void> runAllocator(rcc::MachineFunction &Function,
                                               const rcc::TargetDescription &Target,
                                               AllocatorKind Kind,
                                               rcc::RegisterAllocationOptions Options = {}) {
  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  std::unique_ptr<rcc::MachineFunctionPass> Pass;
  switch (Kind) {
  case AllocatorKind::Basic:
    Pass = rcc::createBasicAllocatorPass(rcc::riscv::registerAllocationInfo(), Options);
    break;
  case AllocatorKind::Fast:
    Pass = std::make_unique<rcc::FastAllocatorPass>(rcc::riscv::registerAllocationInfo());
    break;
  case AllocatorKind::PBQP:
    Pass = std::make_unique<rcc::PBQPAllocatorPass>(rcc::riscv::registerAllocationInfo(), Options);
    break;
  }
  rcc::MachinePassManager Pipeline;
  auto Added = Pipeline.addPass(std::move(Pass));
  if (!Added)
    return std::unexpected(Added.error());
  return Pipeline.run(Function, Target, Analyses);
}

[[nodiscard]] std::size_t countOpcode(const rcc::MachineFunction &Function,
                                      rcc::riscv::InstructionOpcode Value) {
  std::size_t Count = 0;
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Count += Instruction->opcode() == targetOpcode(Value) ? 1U : 0U;
    }
  }
  return Count;
}

[[nodiscard]] std::vector<std::uint64_t> allocationSignature(const rcc::MachineFunction &Function) {
  std::vector<std::uint64_t> Result;
  Result.push_back(Function.frameInfo().liveObjectCount());
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Result.push_back(Instruction->opcode().value());
      for (const rcc::MachineOperand &Operand : Instruction->operands()) {
        Result.push_back(static_cast<std::uint64_t>(Operand.kind()));
        const rcc::Register *Reg = Operand.getIf<rcc::Register>();
        Result.push_back(Reg == nullptr ? 0 : Reg->opaqueValue());
        const auto *Frame = Operand.getIf<rcc::FrameIndexOperand>();
        Result.push_back(Frame == nullptr ? 0
                                          : static_cast<std::uint64_t>(Frame->Index.value()) + 1U);
      }
    }
  }
  return Result;
}

void testTargetHooks(TestContext &Context, const rcc::TargetDescription &Target) {
  const rcc::TargetRegisterAllocationInfo &Info = rcc::riscv::registerAllocationInfo();
  for (const rcc::riscv::RegisterClass Class : {
           rcc::riscv::RegisterClass::GPR,
           rcc::riscv::RegisterClass::GPRC,
           rcc::riscv::RegisterClass::FPR64,
           rcc::riscv::RegisterClass::FPR64C,
           rcc::riscv::RegisterClass::FPR32,
           rcc::riscv::RegisterClass::FPR32C,
           rcc::riscv::RegisterClass::GPR_NX0,
           rcc::riscv::RegisterClass::GPR_NX0_NX2,
       }) {
    Context.expect(Info.validateSpillReload(registerClass(Class), Target).has_value(),
                   "every allocatable RV64GC scalar class must have spill/reload hooks");
  }

  auto Fixture = createFunction("riscv-spill-hooks", 1);
  if (!Fixture)
    return;
  rcc::StackObjectDesc StackDescription;
  StackDescription.Kind = rcc::StackObjectKind::SpillSlot;
  StackDescription.Size = 8;
  auto Align = rcc::Alignment::create(8);
  if (!Align)
    return;
  StackDescription.Align = *Align;
  auto Slot = Fixture->Function->frameInfo().createObject(std::move(StackDescription));
  if (!Slot)
    return;
  rcc::MachineIRBuilder Builder(*Fixture->Function, Target);
  if (!Builder.setInsertPoint(*Fixture->Block))
    return;

  auto GPRReload =
      Info.emitReload(Builder, physical(rcc::riscv::PhysicalRegisterID::X5),
                      registerClass(rcc::riscv::RegisterClass::GPR), *Slot, rcc::LaneMask::all());
  auto GPRSpill =
      Info.emitSpill(Builder, physical(rcc::riscv::PhysicalRegisterID::X5),
                     registerClass(rcc::riscv::RegisterClass::GPR), *Slot, rcc::LaneMask::all());
  auto F32Reload =
      Info.emitReload(Builder, physical(rcc::riscv::PhysicalRegisterID::F10_F),
                      registerClass(rcc::riscv::RegisterClass::FPR32), *Slot, rcc::LaneMask::all());
  auto F32Spill =
      Info.emitSpill(Builder, physical(rcc::riscv::PhysicalRegisterID::F10_F),
                     registerClass(rcc::riscv::RegisterClass::FPR32), *Slot, rcc::LaneMask::all());
  auto F64Reload =
      Info.emitReload(Builder, physical(rcc::riscv::PhysicalRegisterID::F11_D),
                      registerClass(rcc::riscv::RegisterClass::FPR64), *Slot, rcc::LaneMask::all());
  auto F64Spill =
      Info.emitSpill(Builder, physical(rcc::riscv::PhysicalRegisterID::F11_D),
                     registerClass(rcc::riscv::RegisterClass::FPR64), *Slot, rcc::LaneMask::all());
  Context.expect(GPRReload && GPRSpill && F32Reload && F32Spill && F64Reload && F64Spill &&
                     (*GPRReload)->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::LD) &&
                     (*GPRSpill)->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::SD) &&
                     (*F32Reload)->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::FLW) &&
                     (*F32Spill)->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::FSW) &&
                     (*F64Reload)->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::FLD) &&
                     (*F64Spill)->opcode() == targetOpcode(rcc::riscv::InstructionOpcode::FSD),
                 "RV64GC hooks must select width- and bank-correct stack instructions");
  if (GPRReload && F32Reload && F64Reload) {
    Context.expect(
        (*GPRReload)->operandAt(1)->getIf<rcc::FrameIndexOperand>() != nullptr &&
            (*GPRReload)->memOperands().front()->description().Size.minimumBytes() == 8 &&
            (*F32Reload)->memOperands().front()->description().Size.minimumBytes() == 4 &&
            (*F64Reload)->memOperands().front()->description().Size.minimumBytes() == 8,
        "RV64GC stack instructions must retain precise FrameIndex MMO size metadata");
  }
  auto EmptyLane = Info.emitReload(Builder, physical(rcc::riscv::PhysicalRegisterID::X5),
                                   registerClass(rcc::riscv::RegisterClass::GPR), *Slot, {});
  Context.expect(!EmptyLane && EmptyLane.error().code() == rcc::ErrorCode::InvalidArgument,
                 "RV64GC spill hooks must reject an empty lane request");
}

void testRematerialization(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("riscv-rematerialization", 2);
  auto I64 = rcc::LLT::makeInteger(64);
  if (!Fixture || !I64)
    return;
  auto Original = createVReg(*Fixture->Function, *I64, rcc::riscv::RegisterBank::GPR,
                             rcc::riscv::RegisterClass::GPR);
  auto Child = createVReg(*Fixture->Function, *I64, rcc::riscv::RegisterBank::GPR,
                          rcc::riscv::RegisterClass::GPR);
  if (!Original || !Child)
    return;
  auto Definition = buildAddImmediate(*Fixture->Function, Target, *Fixture->Block, *Original,
                                      physical(rcc::riscv::PhysicalRegisterID::X0), 17);
  if (!Definition)
    return;
  const rcc::TargetRegisterAllocationInfo &Info = rcc::riscv::registerAllocationInfo();
  auto Accepted = Info.isRematerializable(**Definition, *Original, *Fixture->Function, Target);
  rcc::MachineIRBuilder Builder(*Fixture->Function, Target);
  if (!Builder.setInsertPointAfter(**Definition))
    return;
  auto Materialized = Info.rematerialize(Builder, **Definition, *Child, *Original, Target);
  Context.expect(Accepted && *Accepted && Materialized &&
                     (*Materialized)->opcode() ==
                         targetOpcode(rcc::riscv::InstructionOpcode::ADDI) &&
                     (*Materialized)->operandAt(0)->registerValue() == *Child &&
                     (*Materialized)->operandAt(1)->registerValue() ==
                         physical(rcc::riscv::PhysicalRegisterID::X0),
                 "RV64GC must rematerialize constant ADDI forms with a replacement definition");
}

[[nodiscard]] rcc::Expected<FunctionFixture>
createSimpleAllocationFunction(const rcc::TargetDescription &Target, std::uint64_t ID) {
  auto Fixture = createFunction("riscv-ra-" + std::to_string(ID), ID);
  auto I64 = rcc::LLT::makeInteger(64);
  if (!Fixture)
    return std::unexpected(Fixture.error());
  if (!I64)
    return std::unexpected(I64.error());
  std::vector<rcc::Register> Values;
  for (std::int64_t Immediate : {1, 2, 3}) {
    auto Reg = createVReg(*Fixture->Function, *I64, rcc::riscv::RegisterBank::GPR,
                          rcc::riscv::RegisterClass::GPR);
    if (!Reg)
      return std::unexpected(Reg.error());
    if (auto Built = buildAddImmediate(*Fixture->Function, Target, *Fixture->Block, *Reg,
                                       physical(rcc::riscv::PhysicalRegisterID::X0), Immediate);
        !Built) {
      return std::unexpected(Built.error());
    }
    Values.push_back(*Reg);
  }
  auto Sum = createVReg(*Fixture->Function, *I64, rcc::riscv::RegisterBank::GPR,
                        rcc::riscv::RegisterClass::GPR);
  if (!Sum)
    return std::unexpected(Sum.error());
  if (auto Built =
          buildAdd(*Fixture->Function, Target, *Fixture->Block, *Sum, Values[0], Values[1]);
      !Built) {
    return std::unexpected(Built.error());
  }
  if (auto Built = buildAdd(*Fixture->Function, Target, *Fixture->Block,
                            physical(rcc::riscv::PhysicalRegisterID::X0), *Sum, Values[2]);
      !Built) {
    return std::unexpected(Built.error());
  }
  auto Prepared = prepareForAllocation(*Fixture->Function, Target);
  if (!Prepared)
    return std::unexpected(Prepared.error());
  return Fixture;
}

void testRealAllocators(TestContext &Context, const rcc::TargetDescription &Target) {
  for (const auto [Kind, ID, Name] : {
           std::tuple{AllocatorKind::Basic, std::uint64_t{10}, std::string_view{"Basic"}},
           std::tuple{AllocatorKind::Fast, std::uint64_t{11}, std::string_view{"Fast"}},
           std::tuple{AllocatorKind::PBQP, std::uint64_t{12}, std::string_view{"PBQP"}},
       }) {
    auto Fixture = createSimpleAllocationFunction(Target, ID);
    if (!Fixture)
      continue;
    auto Allocated = runAllocator(*Fixture->Function, Target, Kind);
    if (!Allocated)
      std::cerr << "RV64GC " << Name << " error: " << Allocated.error().message() << '\n';
    Context.expect(
        Allocated && Fixture->Function->properties().has(rcc::MachineFunctionProperty::NoVRegs) &&
            Fixture->Function->registerInfo().virtualRegisters().size() == 4,
        "every allocator must rewrite real RV64GC selected instructions to physical registers");
    if (!Allocated)
      continue;
    bool OnlyPhysical = true;
    for (const rcc::MachineInstr *Instruction : Fixture->Function->allInstructions()) {
      if (Instruction == nullptr || Instruction->parent() == nullptr)
        continue;
      for (const rcc::MachineOperand &Operand : Instruction->operands()) {
        const rcc::Register *Reg = Operand.getIf<rcc::Register>();
        OnlyPhysical &= Reg == nullptr || !Reg->isVirtual();
      }
    }
    Context.expect(OnlyPhysical,
                   "RV64GC allocator output must not retain a linked virtual-register operand");
  }
}

void testFunctionReservedRegisters(TestContext &Context, const rcc::TargetDescription &Target) {
  for (const auto [Kind, ID, Name] : {
           std::tuple{AllocatorKind::Basic, std::uint64_t{13}, std::string_view{"Basic"}},
           std::tuple{AllocatorKind::Fast, std::uint64_t{14}, std::string_view{"Fast"}},
           std::tuple{AllocatorKind::PBQP, std::uint64_t{15}, std::string_view{"PBQP"}},
       }) {
    auto Fixture = createSimpleAllocationFunction(Target, ID);
    auto Align64 = rcc::Alignment::create(64);
    if (!Fixture || !Align64)
      continue;
    rcc::StackObjectDesc Description;
    Description.Size = 8;
    Description.Align = *Align64;
    if (!Fixture->Function->frameInfo().createObject(std::move(Description)))
      continue;
    auto Allocated = runAllocator(*Fixture->Function, Target, Kind);
    const rcc::PhysicalRegister FP =
        *rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X8);
    const rcc::PhysicalRegister BP =
        *rcc::riscv::physicalRegister(rcc::riscv::PhysicalRegisterID::X9);
    bool AllocatedFrameRegister = false;
    for (const rcc::MachineInstr *Instruction : Fixture->Function->allInstructions()) {
      if (Instruction == nullptr || Instruction->parent() == nullptr)
        continue;
      for (const rcc::MachineOperand &Operand : Instruction->operands()) {
        const rcc::Register *Reg = Operand.getIf<rcc::Register>();
        const auto Physical =
            Reg != nullptr && Reg->isPhysical() ? Reg->physicalRegister() : std::nullopt;
        AllocatedFrameRegister |= Physical && (*Physical == FP || *Physical == BP);
      }
    }
    if (!Allocated)
      std::cerr << "RV64GC " << Name << " reservation error: " << Allocated.error().message()
                << '\n';
    Context.expect(Allocated && Fixture->Function->registerInfo().isReserved(FP) &&
                       Fixture->Function->registerInfo().isReserved(BP) && !AllocatedFrameRegister,
                   "every allocator must persist and honor function-level FP/BP reservations");
  }
}

[[nodiscard]] rcc::Expected<FunctionFixture>
createPressureFunction(const rcc::TargetDescription &Target, std::uint64_t ID) {
  auto Fixture = createFunction("riscv-pressure-" + std::to_string(ID), ID);
  auto I64 = rcc::LLT::makeInteger(64);
  if (!Fixture)
    return std::unexpected(Fixture.error());
  if (!I64)
    return std::unexpected(I64.error());

  std::vector<rcc::Register> Values;
  Values.reserve(30);
  for (std::int64_t Immediate = 0; Immediate < 30; ++Immediate) {
    auto Reg = createVReg(*Fixture->Function, *I64, rcc::riscv::RegisterBank::GPR,
                          rcc::riscv::RegisterClass::GPR);
    if (!Reg)
      return std::unexpected(Reg.error());
    auto Built = buildAddImmediate(*Fixture->Function, Target, *Fixture->Block, *Reg,
                                   physical(rcc::riscv::PhysicalRegisterID::X0), Immediate);
    if (!Built)
      return std::unexpected(Built.error());
    Values.push_back(*Reg);
  }
  const rcc::Register Zero = physical(rcc::riscv::PhysicalRegisterID::X0);
  for (const rcc::Register Value : Values) {
    auto Built = buildAdd(*Fixture->Function, Target, *Fixture->Block, Zero, Value, Zero);
    if (!Built)
      return std::unexpected(Built.error());
  }
  auto Prepared = prepareForAllocation(*Fixture->Function, Target);
  if (!Prepared)
    return std::unexpected(Prepared.error());
  return Fixture;
}

void testRealPressureAndDeterminism(TestContext &Context, const rcc::TargetDescription &Target) {
  std::vector<std::uint64_t> FirstPBQPSignature;
  for (const auto [Kind, ID, Name] : {
           std::tuple{AllocatorKind::Basic, std::uint64_t{20}, std::string_view{"Basic"}},
           std::tuple{AllocatorKind::PBQP, std::uint64_t{21}, std::string_view{"PBQP"}},
           std::tuple{AllocatorKind::PBQP, std::uint64_t{22}, std::string_view{"PBQP-repeat"}},
       }) {
    auto Fixture = createPressureFunction(Target, ID);
    if (!Fixture)
      continue;
    auto Allocated =
        runAllocator(*Fixture->Function, Target, Kind, {.EnableRematerialization = false});
    if (!Allocated)
      std::cerr << "RV64GC pressure " << Name << " error: " << Allocated.error().message() << '\n';
    Context.expect(Allocated && Fixture->Function->frameInfo().liveObjectCount() != 0 &&
                       countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::LD) != 0 &&
                       countOpcode(*Fixture->Function, rcc::riscv::InstructionOpcode::SD) != 0,
                   "RV64GC global allocators must spill and reload real GPR pressure");
    if (!Allocated || Kind != AllocatorKind::PBQP)
      continue;
    const std::vector<std::uint64_t> Signature = allocationSignature(*Fixture->Function);
    if (FirstPBQPSignature.empty())
      FirstPBQPSignature = Signature;
    else
      Context.expect(Signature == FirstPBQPSignature,
                     "RV64GC PBQP allocation must be stable across identical functions");
  }

  auto RematerializedPressure = createPressureFunction(Target, 23);
  if (RematerializedPressure) {
    auto Allocated = runAllocator(*RematerializedPressure->Function, Target, AllocatorKind::PBQP);
    if (!Allocated)
      std::cerr << "RV64GC rematerialization pressure error: " << Allocated.error().message()
                << '\n';
    Context.expect(Allocated && RematerializedPressure->Function->properties().has(
                                    rcc::MachineFunctionProperty::NoVRegs),
                   "RV64GC PBQP rematerialization must finish with a valid register map");
  }
}

} // namespace

int main() {
  TestContext Context;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC register-allocation target must be constructible");
  if (!Target) {
    std::cerr << "target construction error: " << Target.error().message() << '\n';
    return Context.result();
  }
  testTargetHooks(Context, *Target);
  testRematerialization(Context, *Target);
  testRealAllocators(Context, *Target);
  testFunctionReservedRegisters(Context, *Target);
  testRealPressureAndDeterminism(Context, *Target);
  return Context.result();
}
