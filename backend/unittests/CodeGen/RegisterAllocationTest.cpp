// SPDX-License-Identifier: GPL-3.0-or-later

#include "Target/TestTarget.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/LiveRangeEdit.h"
#include "rcc/CodeGen/LiveRegMatrix.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFGUpdater.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachinePass.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/RegAllocCommon.h"
#include "rcc/CodeGen/RegAllocFast.h"
#include "rcc/CodeGen/RegAllocPBQP.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/Spiller.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/VirtRegMap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t DefOpcode = 3000;
constexpr std::uint32_t RematerializableDefOpcode = 3001;
constexpr std::uint32_t UseOpcode = 3002;
constexpr std::uint32_t BinaryOpcode = 3003;
constexpr std::uint32_t SpillLoadOpcode = 3004;
constexpr std::uint32_t SpillStoreOpcode = 3005;
constexpr std::uint32_t MaskOpcode = 3006;
constexpr std::uint32_t EarlyOpcode = 3007;
constexpr std::uint32_t TiedOpcode = 3008;
constexpr std::uint32_t CallDefOpcode = 3009;

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

[[nodiscard]] constexpr rcc::Opcode testOpcode(std::uint32_t Value) noexcept {
  return rcc::Opcode::fromValue(Value);
}

[[nodiscard]] rcc::PhysicalRegister physical(std::uint32_t Number) {
  return *rcc::PhysicalRegister::create(Number);
}

[[nodiscard]] rcc::OperandDescriptor registerOperand(rcc::RegisterOperandRole Role,
                                                     bool EarlyClobber = false) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {rcc::MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.AllowSubReg = true;
  Result.EarlyClobber = EarlyClobber;
  return Result;
}

[[nodiscard]] rcc::OperandDescriptor plainOperand(rcc::MachineOperand::Kind Kind) {
  rcc::OperandDescriptor Result;
  Result.Kinds = {Kind};
  return Result;
}

[[nodiscard]] rcc::InstructionDescriptor descriptor(std::uint32_t Value, std::string Name) {
  rcc::InstructionDescriptor Result;
  Result.InstructionOpcode = testOpcode(Value);
  Result.Name = std::move(Name);
  Result.Flags = {rcc::InstructionFlag::ProducesCode};
  return Result;
}

[[nodiscard]] rcc::Expected<rcc::TargetDescription>
createTarget(bool AddSharedMultiLaneUnit = false) {
  std::vector<rcc::InstructionDescriptor> Descriptors;

  auto Def = descriptor(DefOpcode, "RA_DEF");
  Def.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                  plainOperand(rcc::MachineOperand::Kind::Immediate)};
  Def.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Def));

  auto Remat = descriptor(RematerializableDefOpcode, "RA_REMAT_DEF");
  Remat.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                    plainOperand(rcc::MachineOperand::Kind::Immediate)};
  Remat.NumExplicitDefs = 1;
  Remat.Flags.set(rcc::InstructionFlag::Rematerializable);
  Descriptors.push_back(std::move(Remat));

  auto Use = descriptor(UseOpcode, "RA_USE");
  Use.Operands = {registerOperand(rcc::RegisterOperandRole::Use)};
  Use.Flags.set(rcc::InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(Use));

  auto Binary = descriptor(BinaryOpcode, "RA_BINARY");
  Binary.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                     registerOperand(rcc::RegisterOperandRole::Use),
                     registerOperand(rcc::RegisterOperandRole::Use)};
  Binary.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Binary));

  auto Reload = descriptor(SpillLoadOpcode, "RA_RELOAD");
  Reload.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                     plainOperand(rcc::MachineOperand::Kind::FrameIndex)};
  Reload.NumExplicitDefs = 1;
  Reload.Flags.set(rcc::InstructionFlag::MayLoad);
  Reload.RequiresMemoryOperands = true;
  Descriptors.push_back(std::move(Reload));

  auto Spill = descriptor(SpillStoreOpcode, "RA_SPILL");
  Spill.Operands = {registerOperand(rcc::RegisterOperandRole::Use),
                    plainOperand(rcc::MachineOperand::Kind::FrameIndex)};
  Spill.Flags.set(rcc::InstructionFlag::MayStore);
  Spill.RequiresMemoryOperands = true;
  Descriptors.push_back(std::move(Spill));

  auto Mask = descriptor(MaskOpcode, "RA_MASK");
  Mask.Operands = {plainOperand(rcc::MachineOperand::Kind::RegisterMask)};
  Mask.Flags.set(rcc::InstructionFlag::Call);
  Mask.Flags.set(rcc::InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(Mask));

  auto Early = descriptor(EarlyOpcode, "RA_EARLY");
  Early.Operands = {registerOperand(rcc::RegisterOperandRole::Def, true),
                    registerOperand(rcc::RegisterOperandRole::Use)};
  Early.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Early));

  auto Tied = descriptor(TiedOpcode, "RA_TIED");
  auto TiedDef = registerOperand(rcc::RegisterOperandRole::Def);
  auto TiedUse = registerOperand(rcc::RegisterOperandRole::Use);
  TiedDef.DefaultTiedOperand = 1;
  TiedUse.DefaultTiedOperand = 0;
  Tied.Operands = {TiedDef, TiedUse};
  Tied.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Tied));

  auto CallDef = descriptor(CallDefOpcode, "RA_CALL_DEF");
  CallDef.Operands = {registerOperand(rcc::RegisterOperandRole::Def),
                      plainOperand(rcc::MachineOperand::Kind::RegisterMask)};
  CallDef.NumExplicitDefs = 1;
  CallDef.Flags.set(rcc::InstructionFlag::Call);
  CallDef.Flags.set(rcc::InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(CallDef));

  auto Instructions = rcc::InstructionSet::create(std::move(Descriptors), {});
  if (!Instructions)
    return std::unexpected(Instructions.error());
  if (AddSharedMultiLaneUnit) {
    auto Description = rcc::test::createTestTargetRegisterInfoDescription();
    const rcc::RegisterUnitID SharedUnit = rcc::RegisterUnitID::fromValue(12);
    Description.RegisterUnits.push_back({SharedUnit, "shared.multi-lane"});
    for (rcc::PhysicalRegisterDesc &Register : Description.PhysicalRegisters) {
      if (Register.Reg == physical(rcc::test::TestRegister::G0) ||
          Register.Reg == physical(rcc::test::TestRegister::G1)) {
        Register.Units.push_back({SharedUnit, rcc::LaneMask::fromRaw(0xF)});
      }
    }
    auto Registers = rcc::TargetRegisterInfo::create(std::move(Description));
    if (!Registers)
      return std::unexpected(Registers.error());
    return rcc::TargetDescription::create(std::move(*Instructions), std::move(*Registers),
                                          rcc::test::createTestTargetMIRTextInfoDescription());
  }
  return rcc::test::createTestTargetDescription(std::move(*Instructions));
}

[[nodiscard]] rcc::Expected<const rcc::MachineMemOperand *>
createStackMemory(rcc::MachineIRBuilder &Builder, rcc::FrameIndex StackSlot,
                  rcc::MemoryAccessFlag Access) {
  auto Size = rcc::MemorySize::fixed(8);
  auto Align = rcc::Alignment::create(8);
  if (!Size)
    return std::unexpected(Size.error());
  if (!Align)
    return std::unexpected(Align.error());
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = {Access};
  Description.Size = *Size;
  Description.Align = *Align;
  Description.Base = rcc::MemoryBase::frameIndex(StackSlot);
  return Builder.function().createMemOperand(std::move(Description));
}

class TestRegisterAllocationInfo final : public rcc::TargetRegisterAllocationInfo {
public:
  [[nodiscard]] rcc::Expected<void>
  validateSpillReload(rcc::RegisterClassID Class,
                      const rcc::TargetDescription &Target) const override {
    const rcc::RegisterClassDesc *Description = Target.registers().registerClass(Class);
    if (Description == nullptr || !Description->Allocatable || Description->SpillSizeInBits != 64 ||
        Description->SpillAlignmentInBits != 64) {
      return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                                 "test target register class is not spillable");
    }
    return {};
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *>
  emitReload(rcc::MachineIRBuilder &Builder, rcc::Register Destination, rcc::RegisterClassID Class,
             rcc::FrameIndex StackSlot, rcc::LaneMask Lanes) const override {
    auto Validated = validateSpillReload(Class, Builder.target());
    if (!Validated)
      return std::unexpected(Validated.error());
    if (Lanes.empty())
      return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument, "test reload requires lanes");
    auto Memory = createStackMemory(Builder, StackSlot, rcc::MemoryAccessFlag::Load);
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Pending = Builder.buildInstr(testOpcode(SpillLoadOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addDef(Destination); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addFrameIndex(StackSlot); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addMemOperand(**Memory); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *>
  emitSpill(rcc::MachineIRBuilder &Builder, rcc::Register Source, rcc::RegisterClassID Class,
            rcc::FrameIndex StackSlot, rcc::LaneMask Lanes) const override {
    auto Validated = validateSpillReload(Class, Builder.target());
    if (!Validated)
      return std::unexpected(Validated.error());
    if (Lanes.empty())
      return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument, "test spill requires lanes");
    auto Memory = createStackMemory(Builder, StackSlot, rcc::MemoryAccessFlag::Store);
    if (!Memory)
      return std::unexpected(Memory.error());
    auto Pending = Builder.buildInstr(testOpcode(SpillStoreOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addUse(Source); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addFrameIndex(StackSlot); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addMemOperand(**Memory); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] rcc::Expected<bool>
  isRematerializable(const rcc::MachineInstr &Instruction, rcc::Register VirtualReg,
                     const rcc::MachineFunction &, const rcc::TargetDescription &) const override {
    const rcc::MachineOperand *Operand = Instruction.operandAt(0);
    const rcc::Register *Reg = Operand == nullptr ? nullptr : Operand->getIf<rcc::Register>();
    return Instruction.opcode() == testOpcode(RematerializableDefOpcode) && Reg != nullptr &&
           *Reg == VirtualReg;
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *>
  rematerialize(rcc::MachineIRBuilder &Builder, const rcc::MachineInstr &Original,
                rcc::Register Destination, rcc::Register OriginalRegister,
                const rcc::TargetDescription &Target) const override {
    auto Accepted = isRematerializable(Original, OriginalRegister, Builder.function(), Target);
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted)
      return rcc::makeUnexpected(rcc::ErrorCode::InvalidArgument,
                                 "test instruction is not rematerializable");
    auto Pending = Builder.buildInstr(testOpcode(RematerializableDefOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addDef(Destination); !Result)
      return std::unexpected(Result.error());
    const rcc::MachineOperand *Immediate = Original.operandAt(1);
    if (Immediate == nullptr)
      return rcc::makeUnexpected(rcc::ErrorCode::InternalError,
                                 "test rematerialization lost its immediate");
    if (auto Result = Pending->addOperand(Immediate->cloneDetached()); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }
};

class RejectingRegisterAllocationInfo final : public rcc::TargetRegisterAllocationInfo {
public:
  [[nodiscard]] rcc::Expected<void>
  validateSpillReload(rcc::RegisterClassID, const rcc::TargetDescription &) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                               "test target intentionally rejects spills");
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *> emitReload(rcc::MachineIRBuilder &,
                                                              rcc::Register, rcc::RegisterClassID,
                                                              rcc::FrameIndex,
                                                              rcc::LaneMask) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                               "test target intentionally rejects reloads");
  }

  [[nodiscard]] rcc::Expected<rcc::MachineInstr *> emitSpill(rcc::MachineIRBuilder &, rcc::Register,
                                                             rcc::RegisterClassID, rcc::FrameIndex,
                                                             rcc::LaneMask) const override {
    return rcc::makeUnexpected(rcc::ErrorCode::Unsupported,
                               "test target intentionally rejects spills");
  }
};

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

[[nodiscard]] rcc::Expected<rcc::Register>
createVReg(rcc::MachineFunction &Function,
           rcc::RegisterClassID Class = rcc::test::TestRegisterClass::GPR64) {
  auto Type = rcc::LLT::makeInteger(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  if (!Reg)
    return std::unexpected(Reg.error());
  if (auto Result = Function.registerInfo().setRegisterBank(*Reg, rcc::test::TestRegisterBank::GPR);
      !Result) {
    return std::unexpected(Result.error());
  }
  if (auto Result = Function.registerInfo().setRegisterClass(*Reg, Class); !Result)
    return std::unexpected(Result.error());
  return *Reg;
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildDef(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Reg, std::int64_t Value,
         rcc::SubRegIndex SubReg = {}, bool Rematerializable = false) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending =
      Builder.buildInstr(testOpcode(Rematerializable ? RematerializableDefOpcode : DefOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Reg, {}, SubReg); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addImmediate(Value); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildUse(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
         rcc::MachineBasicBlock &Block, rcc::Register Reg, rcc::SubRegIndex SubReg = {}) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(UseOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addUse(Reg, {}, SubReg); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildMask(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
          rcc::MachineBasicBlock &Block, std::shared_ptr<const rcc::RegisterMask> Mask) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(MaskOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addRegisterMask(std::move(Mask)); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildCallDef(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
             rcc::MachineBasicBlock &Block, rcc::Register Destination,
             std::shared_ptr<const rcc::RegisterMask> Mask) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(CallDefOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addRegisterMask(std::move(Mask)); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *>
buildEarly(rcc::MachineFunction &Function, const rcc::TargetDescription &Target,
           rcc::MachineBasicBlock &Block, rcc::Register Destination, rcc::Register Source) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(EarlyOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  rcc::RegisterOperandFlags Flags;
  Flags.set(rcc::RegisterOperandFlag::EarlyClobber);
  if (auto Result = Pending->addDef(Destination, Flags); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Source); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] rcc::Expected<rcc::MachineInstr *> buildTied(rcc::MachineFunction &Function,
                                                           const rcc::TargetDescription &Target,
                                                           rcc::MachineBasicBlock &Block,
                                                           rcc::Register Reg) {
  rcc::MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(TiedOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Reg); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Reg); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->tieOperands(0, 1); !Result)
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
                                               const rcc::TargetRegisterAllocationInfo &Info,
                                               AllocatorKind Kind,
                                               rcc::RegisterAllocationOptions Options = {}) {
  rcc::MachineAnalysisManager Analyses;
  auto Registered = rcc::registerStage11MachineAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  std::unique_ptr<rcc::MachineFunctionPass> Pass;
  switch (Kind) {
  case AllocatorKind::Basic:
    Pass = rcc::createBasicAllocatorPass(Info, Options);
    break;
  case AllocatorKind::Fast:
    Pass = std::make_unique<rcc::FastAllocatorPass>(Info);
    break;
  case AllocatorKind::PBQP:
    Pass = std::make_unique<rcc::PBQPAllocatorPass>(Info, Options);
    break;
  }
  rcc::MachinePassManager Pipeline;
  auto Added = Pipeline.addPass(std::move(Pass));
  if (!Added)
    return std::unexpected(Added.error());
  return Pipeline.run(Function, Target, Analyses);
}

[[nodiscard]] std::size_t countOpcode(const rcc::MachineFunction &Function, std::uint32_t Value) {
  std::size_t Count = 0;
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Count += Instruction->opcode() == testOpcode(Value) ? 1U : 0U;
    }
  }
  return Count;
}

[[nodiscard]] bool hasInterference(std::span<const rcc::RegisterInterference> Values,
                                   rcc::RegisterInterferenceKind Kind) {
  return std::ranges::any_of(
      Values, [Kind](const rcc::RegisterInterference &Value) { return Value.Kind == Kind; });
}

[[nodiscard]] std::vector<std::uint64_t> functionSignature(const rcc::MachineFunction &Function) {
  std::vector<std::uint64_t> Result;
  Result.push_back(Function.frameInfo().liveObjectCount());
  for (const rcc::MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    Result.push_back(Block->stableID().value());
    for (const rcc::MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      Result.push_back(Instruction->opcode().value());
      Result.push_back(Instruction->operandCount());
      for (const rcc::MachineOperand &Operand : Instruction->operands()) {
        Result.push_back(static_cast<std::uint64_t>(Operand.kind()));
        const rcc::Register *Reg = Operand.getIf<rcc::Register>();
        Result.push_back(Reg == nullptr ? 0 : Reg->opaqueValue());
      }
    }
  }
  return Result;
}

void testVirtRegMapAndEdit(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("virtreg-map", 1);
  if (!Fixture)
    return;
  auto Parent = createVReg(*Fixture->Function);
  if (!Parent)
    return;
  rcc::VirtRegMap Mapping(*Fixture->Function);
  Context.expect(Mapping.add(*Parent).has_value(),
                 "VirtRegMap must accept a known virtual register");
  rcc::LiveRangeEdit Edit(*Fixture->Function, Target, Mapping);
  auto Child = Edit.createFrom(*Parent, false);
  auto Slot = Edit.getOrCreateSpillSlot(*Parent);
  Context.expect(Child && Slot && Mapping.stackSlot(*Parent) == Slot &&
                     Mapping.entry(*Child) != nullptr &&
                     Mapping.entry(*Child)->SplitParent == *Parent &&
                     Mapping.entry(*Child)->Origin == *Parent && Mapping.verify().has_value(),
                 "LiveRangeEdit must preserve class, origin, split, and spill-slot state");
  if (!Child)
    return;
  const auto G0 = physical(rcc::test::TestRegister::G0);
  Context.expect(Mapping.assign(*Child, G0).has_value() && Mapping.physicalRegister(*Child) == G0 &&
                     !Mapping.isSpillable(*Child),
                 "VirtRegMap must publish deterministic child assignments");
}

void testLiveRegMatrix(TestContext &Context, const rcc::TargetDescription &Target) {
  auto LaneFixture = createFunction("lane-matrix", 2);
  if (!LaneFixture)
    return;
  auto Low = createVReg(*LaneFixture->Function);
  auto High = createVReg(*LaneFixture->Function);
  if (!Low || !High)
    return;
  if (!buildDef(*LaneFixture->Function, Target, *LaneFixture->Block, *Low, 1,
                rcc::test::TestSubReg::Low32) ||
      !buildDef(*LaneFixture->Function, Target, *LaneFixture->Block, *High, 2,
                rcc::test::TestSubReg::High32) ||
      !buildUse(*LaneFixture->Function, Target, *LaneFixture->Block, *High,
                rcc::test::TestSubReg::High32) ||
      !buildUse(*LaneFixture->Function, Target, *LaneFixture->Block, *Low,
                rcc::test::TestSubReg::Low32)) {
    return;
  }
  if (!rcc::rebuildMachineLiveness(*LaneFixture->Function, Target))
    return;
  rcc::MachineAnalysisManager LaneAnalyses;
  if (!rcc::registerStage11MachineAnalyses(LaneAnalyses))
    return;
  auto LaneIntervals =
      LaneAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*LaneFixture->Function, Target);
  if (!LaneIntervals)
    return;
  const auto G0 = physical(rcc::test::TestRegister::G0);
  rcc::LiveRegMatrix LaneMatrix(*LaneFixture->Function, Target, **LaneIntervals);
  Context.expect(LaneMatrix.assign(*Low, G0).has_value() &&
                     LaneMatrix.checkInterference(*High, G0).Kind ==
                         rcc::RegisterInterferenceKind::Free &&
                     LaneMatrix.assign(*High, G0).has_value(),
                 "LiveRegMatrix must permit overlapping intervals on disjoint subregister lanes");

  auto FixedFixture = createFunction("fixed-matrix", 3);
  if (!FixedFixture)
    return;
  auto Value = createVReg(*FixedFixture->Function);
  if (!Value || !buildDef(*FixedFixture->Function, Target, *FixedFixture->Block, *Value, 7) ||
      !buildMask(*FixedFixture->Function, Target, *FixedFixture->Block,
                 rcc::test::createTestCallPreservedMask()) ||
      !buildUse(*FixedFixture->Function, Target, *FixedFixture->Block,
                rcc::Register::fromPhysical(G0)) ||
      !buildUse(*FixedFixture->Function, Target, *FixedFixture->Block, *Value)) {
    return;
  }
  if (!rcc::rebuildMachineLiveness(*FixedFixture->Function, Target))
    return;
  rcc::MachineAnalysisManager FixedAnalyses;
  if (!rcc::registerStage11MachineAnalyses(FixedAnalyses))
    return;
  auto FixedIntervals =
      FixedAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*FixedFixture->Function, Target);
  if (!FixedIntervals)
    return;
  rcc::LiveRegMatrix FixedMatrix(*FixedFixture->Function, Target, **FixedIntervals);
  const auto G0Interferences = FixedMatrix.collectInterferences(*Value, G0);
  const auto G1 = physical(rcc::test::TestRegister::G1);
  Context.expect(
      hasInterference(G0Interferences, rcc::RegisterInterferenceKind::RegisterMask) &&
          (hasInterference(G0Interferences, rcc::RegisterInterferenceKind::FixedRegister) ||
           hasInterference(G0Interferences, rcc::RegisterInterferenceKind::RegisterUnit)) &&
          FixedMatrix.checkInterference(*Value, G1).Kind == rcc::RegisterInterferenceKind::Free,
      "LiveRegMatrix must classify fixed, unit, and RegMask interference separately");

  auto FixedLaneFixture = createFunction("fixed-lane-matrix", 4);
  if (!FixedLaneFixture)
    return;
  auto HighValue = createVReg(*FixedLaneFixture->Function);
  const auto G0Low32 = physical(rcc::test::TestRegister::G0Low32);
  if (!HighValue ||
      !buildDef(*FixedLaneFixture->Function, Target, *FixedLaneFixture->Block, *HighValue, 9,
                rcc::test::TestSubReg::High32) ||
      !buildUse(*FixedLaneFixture->Function, Target, *FixedLaneFixture->Block,
                rcc::Register::fromPhysical(G0Low32)) ||
      !buildUse(*FixedLaneFixture->Function, Target, *FixedLaneFixture->Block, *HighValue,
                rcc::test::TestSubReg::High32) ||
      !rcc::rebuildMachineLiveness(*FixedLaneFixture->Function, Target)) {
    return;
  }
  rcc::MachineAnalysisManager FixedLaneAnalyses;
  if (!rcc::registerStage11MachineAnalyses(FixedLaneAnalyses))
    return;
  auto FixedLaneIntervals =
      FixedLaneAnalyses.getResult<rcc::LiveIntervalsAnalysis>(*FixedLaneFixture->Function, Target);
  if (!FixedLaneIntervals)
    return;
  rcc::LiveRegMatrix FixedLaneMatrix(*FixedLaneFixture->Function, Target, **FixedLaneIntervals);
  Context.expect(FixedLaneMatrix.checkInterference(*HighValue, G0).Kind ==
                     rcc::RegisterInterferenceKind::Free,
                 "a fixed subregister must not interfere with a disjoint virtual lane");
}

void testSharedUnitLaneMatrix(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createFunction("shared-unit-lane-matrix", 5);
  if (!Fixture)
    return;
  auto Low = createVReg(*Fixture->Function);
  auto High = createVReg(*Fixture->Function);
  if (!Low || !High ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *Low, 1,
                rcc::test::TestSubReg::Low32) ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *High, 2,
                rcc::test::TestSubReg::High32) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *High,
                rcc::test::TestSubReg::High32) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Low, rcc::test::TestSubReg::Low32) ||
      !rcc::rebuildMachineLiveness(*Fixture->Function, Target)) {
    return;
  }
  rcc::MachineAnalysisManager Analyses;
  if (!rcc::registerStage11MachineAnalyses(Analyses))
    return;
  auto Intervals = Analyses.getResult<rcc::LiveIntervalsAnalysis>(*Fixture->Function, Target);
  if (!Intervals)
    return;
  const auto G0 = physical(rcc::test::TestRegister::G0);
  const auto G1 = physical(rcc::test::TestRegister::G1);
  rcc::LiveRegMatrix Matrix(*Fixture->Function, Target, **Intervals);
  Context.expect(Target.registers().regsOverlap(G0, G1) && Matrix.assign(*Low, G0).has_value() &&
                     Matrix.checkInterference(*High, G1).Kind ==
                         rcc::RegisterInterferenceKind::Free &&
                     Matrix.assign(*High, G1).has_value(),
                 "disjoint lanes must coexist on a shared multi-lane RegisterUnit");
}

[[nodiscard]] rcc::Expected<FunctionFixture>
createPressureFunction(const rcc::TargetDescription &Target, std::uint64_t ID) {
  auto Fixture = createFunction("pressure-" + std::to_string(ID), ID);
  if (!Fixture)
    return std::unexpected(Fixture.error());
  std::vector<rcc::Register> Values;
  for (std::int64_t Index = 0; Index < 3; ++Index) {
    auto Reg = createVReg(*Fixture->Function);
    if (!Reg)
      return std::unexpected(Reg.error());
    if (auto Built = buildDef(*Fixture->Function, Target, *Fixture->Block, *Reg, Index); !Built)
      return std::unexpected(Built.error());
    Values.push_back(*Reg);
  }
  for (const rcc::Register Reg : Values) {
    if (auto Built = buildUse(*Fixture->Function, Target, *Fixture->Block, Reg); !Built)
      return std::unexpected(Built.error());
  }
  auto Prepared = prepareForAllocation(*Fixture->Function, Target);
  if (!Prepared)
    return std::unexpected(Prepared.error());
  return Fixture;
}

void testAllocatorsUnderPressure(TestContext &Context, const rcc::TargetDescription &Target,
                                 const TestRegisterAllocationInfo &Info) {
  for (const auto [Kind, ID, Name] : {
           std::tuple{AllocatorKind::Basic, std::uint64_t{10}, std::string_view{"Basic"}},
           std::tuple{AllocatorKind::Fast, std::uint64_t{11}, std::string_view{"Fast"}},
           std::tuple{AllocatorKind::PBQP, std::uint64_t{12}, std::string_view{"PBQP"}},
       }) {
    auto Fixture = createPressureFunction(Target, ID);
    if (!Fixture)
      continue;
    auto Allocated =
        runAllocator(*Fixture->Function, Target, Info, Kind, {.EnableRematerialization = false});
    if (!Allocated)
      std::cerr << Name << " allocation error: " << Allocated.error().message() << '\n';
    Context.expect(Allocated.has_value() &&
                       Fixture->Function->properties().has(rcc::MachineFunctionProperty::NoVRegs) &&
                       Fixture->Function->frameInfo().liveObjectCount() != 0 &&
                       countOpcode(*Fixture->Function, SpillLoadOpcode) != 0 &&
                       countOpcode(*Fixture->Function, SpillStoreOpcode) != 0,
                   "all allocators must close a deterministic spill/reload cycle under pressure");
  }
}

void testFailureAtomicity(TestContext &Context, const rcc::TargetDescription &Target) {
  auto Fixture = createPressureFunction(Target, 13);
  if (!Fixture)
    return;
  const std::vector<std::uint64_t> Before = functionSignature(*Fixture->Function);
  const std::size_t KnownBefore = static_cast<std::size_t>(std::ranges::count_if(
      Fixture->Function->registerInfo().virtualRegisters(),
      [](const rcc::VirtualRegisterInfo &Info) { return !Info.IsTombstone; }));
  RejectingRegisterAllocationInfo Rejecting;
  auto Failed = runAllocator(*Fixture->Function, Target, Rejecting, AllocatorKind::Basic,
                             {.EnableRematerialization = false});
  const std::size_t KnownAfter = static_cast<std::size_t>(std::ranges::count_if(
      Fixture->Function->registerInfo().virtualRegisters(),
      [](const rcc::VirtualRegisterInfo &Info) { return !Info.IsTombstone; }));
  Context.expect(!Failed && Failed.error().code() == rcc::ErrorCode::InternalError &&
                     functionSignature(*Fixture->Function) == Before && KnownAfter == KnownBefore &&
                     !Fixture->Function->properties().has(rcc::MachineFunctionProperty::NoVRegs),
                 "failed register allocation must leave the original MachineFunction unchanged");
}

void testFastBlockBoundary(TestContext &Context, const rcc::TargetDescription &Target,
                           const TestRegisterAllocationInfo &Info) {
  auto Fixture = createFunction("fast-block-boundary", 14);
  if (!Fixture)
    return;
  auto Exit = Fixture->Function->createBasicBlock();
  if (!Exit || !Fixture->Function->appendBlock(**Exit) ||
      !Fixture->Function->cfgUpdater().addSuccessor(*Fixture->Block, **Exit,
                                                    rcc::BranchProbability::one())) {
    return;
  }
  auto Value = createVReg(*Fixture->Function);
  const rcc::Register G0 = rcc::Register::fromPhysical(physical(rcc::test::TestRegister::G0));
  if (!Value || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 9) ||
      !buildUse(*Fixture->Function, Target, **Exit, *Value) ||
      !buildUse(*Fixture->Function, Target, **Exit, G0) ||
      !prepareForAllocation(*Fixture->Function, Target)) {
    return;
  }
  auto Allocated = runAllocator(*Fixture->Function, Target, Info, AllocatorKind::Fast);
  Context.expect(
      Allocated && countOpcode(*Fixture->Function, SpillStoreOpcode) == 1 &&
          countOpcode(*Fixture->Function, SpillLoadOpcode) == 1 &&
          (*Exit)->firstInstruction()->next() != nullptr &&
          (*Exit)->firstInstruction()->next()->operandAt(0)->registerValue() != G0,
      "FastAllocator must preserve block live-ins and virtual values across a CFG boundary");
}

void testFastRegMaskCalls(TestContext &Context, const rcc::TargetDescription &Target,
                          const TestRegisterAllocationInfo &Info) {
  auto LiveThrough = createFunction("fast-regmask-live-through", 16);
  if (!LiveThrough)
    return;
  auto Value = createVReg(*LiveThrough->Function);
  if (!Value || !buildDef(*LiveThrough->Function, Target, *LiveThrough->Block, *Value, 11) ||
      !buildMask(*LiveThrough->Function, Target, *LiveThrough->Block,
                 rcc::test::createTestCallPreservedMask()) ||
      !buildUse(*LiveThrough->Function, Target, *LiveThrough->Block, *Value) ||
      !prepareForAllocation(*LiveThrough->Function, Target)) {
    return;
  }
  auto LiveThroughAllocated =
      runAllocator(*LiveThrough->Function, Target, Info, AllocatorKind::Fast);
  Context.expect(LiveThroughAllocated &&
                     countOpcode(*LiveThrough->Function, SpillStoreOpcode) == 1 &&
                     countOpcode(*LiveThrough->Function, SpillLoadOpcode) == 1,
                 "FastAllocator must spill and reload a value live through a clobbering RegMask");

  auto CallResult = createFunction("fast-regmask-call-result", 17);
  if (!CallResult)
    return;
  auto Result = createVReg(*CallResult->Function);
  if (!Result ||
      !buildCallDef(*CallResult->Function, Target, *CallResult->Block, *Result,
                    rcc::test::createTestCallPreservedMask()) ||
      !buildUse(*CallResult->Function, Target, *CallResult->Block, *Result) ||
      !prepareForAllocation(*CallResult->Function, Target)) {
    return;
  }
  auto ResultAllocated = runAllocator(*CallResult->Function, Target, Info, AllocatorKind::Fast);
  Context.expect(ResultAllocated && countOpcode(*CallResult->Function, SpillStoreOpcode) == 0 &&
                     countOpcode(*CallResult->Function, SpillLoadOpcode) == 0,
                 "FastAllocator must retain a call-defined result without reloading an "
                 "uninitialized spill slot");
}

void testTiedAndEarlyClobber(TestContext &Context, const rcc::TargetDescription &Target,
                             const TestRegisterAllocationInfo &Info) {
  auto Fixture = createFunction("tied-early", 15);
  if (!Fixture)
    return;
  auto Source = createVReg(*Fixture->Function);
  auto Destination = createVReg(*Fixture->Function);
  if (!Source || !Destination ||
      !buildDef(*Fixture->Function, Target, *Fixture->Block, *Source, 1)) {
    return;
  }
  auto Early = buildEarly(*Fixture->Function, Target, *Fixture->Block, *Destination, *Source);
  auto Tied = buildTied(*Fixture->Function, Target, *Fixture->Block, *Destination);
  if (!Early || !Tied || !buildUse(*Fixture->Function, Target, *Fixture->Block, *Destination) ||
      !prepareForAllocation(*Fixture->Function, Target)) {
    return;
  }
  auto Allocated = runAllocator(*Fixture->Function, Target, Info, AllocatorKind::Basic);
  const std::optional<rcc::Register> EarlyDef = (*Early)->operandAt(0)->registerValue();
  const std::optional<rcc::Register> EarlyUse = (*Early)->operandAt(1)->registerValue();
  const std::optional<rcc::Register> TiedDef = (*Tied)->operandAt(0)->registerValue();
  const std::optional<rcc::Register> TiedUse = (*Tied)->operandAt(1)->registerValue();
  Context.expect(Allocated && EarlyDef && EarlyUse && *EarlyDef != *EarlyUse && TiedDef &&
                     TiedUse && *TiedDef == *TiedUse,
                 "allocation must separate early-clobber operands and preserve rewritten ties");
}

void testRematerialization(TestContext &Context, const rcc::TargetDescription &Target,
                           const TestRegisterAllocationInfo &Info) {
  auto Fixture = createFunction("rematerialization", 20);
  if (!Fixture)
    return;
  auto Value = createVReg(*Fixture->Function);
  if (!Value || !buildDef(*Fixture->Function, Target, *Fixture->Block, *Value, 42, {}, true) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value) ||
      !buildUse(*Fixture->Function, Target, *Fixture->Block, *Value)) {
    return;
  }
  rcc::VirtRegMap Mapping(*Fixture->Function);
  if (!Mapping.add(*Value))
    return;
  rcc::Spiller Spiller(*Fixture->Function, Target, Info, Mapping, true);
  auto Children = Spiller.spill(*Value);
  Context.expect(Children && Children->size() == 2 &&
                     Fixture->Function->frameInfo().liveObjectCount() == 0 &&
                     countOpcode(*Fixture->Function, RematerializableDefOpcode) == 2 &&
                     countOpcode(*Fixture->Function, SpillLoadOpcode) == 0 &&
                     countOpcode(*Fixture->Function, SpillStoreOpcode) == 0,
                 "Spiller must rematerialize each use without creating a stack object");

  auto Unspillable = createVReg(*Fixture->Function);
  if (!Unspillable || !Mapping.add(*Unspillable, false))
    return;
  auto Rejected = Spiller.spill(*Unspillable);
  Context.expect(!Rejected && Rejected.error().code() == rcc::ErrorCode::RegisterAllocationFailed,
                 "Spiller must reject pressure on a non-spillable virtual register");
}

void testRewriteAndConstraints(TestContext &Context, const rcc::TargetDescription &Target,
                               const TestRegisterAllocationInfo &Info) {
  auto RewriteFixture = createFunction("subregister-rewrite", 30);
  if (!RewriteFixture)
    return;
  auto Value = createVReg(*RewriteFixture->Function);
  if (!Value ||
      !buildDef(*RewriteFixture->Function, Target, *RewriteFixture->Block, *Value, 1,
                rcc::test::TestSubReg::Low32) ||
      !buildUse(*RewriteFixture->Function, Target, *RewriteFixture->Block, *Value,
                rcc::test::TestSubReg::Low32)) {
    return;
  }
  rcc::VirtRegMap Mapping(*RewriteFixture->Function);
  const auto G0 = physical(rcc::test::TestRegister::G0);
  if (!Mapping.add(*Value) || !Mapping.assign(*Value, G0))
    return;
  auto Rewritten = rcc::rewriteVirtualRegisters(*RewriteFixture->Function, Target, Mapping);
  const auto G0Low32 = physical(rcc::test::TestRegister::G0Low32);
  Context.expect(Rewritten && *Rewritten &&
                     RewriteFixture->Block->firstInstruction()->operandAt(0)->registerValue() ==
                         rcc::Register::fromPhysical(G0Low32) &&
                     RewriteFixture->Block->firstInstruction()->operandAt(0)->subReg().value() == 0,
                 "physical rewrite must project and clear virtual subregister operands");

  auto MaskFixture = createFunction("mask-allocation", 31);
  if (!MaskFixture)
    return;
  auto AcrossCall = createVReg(*MaskFixture->Function);
  if (!AcrossCall ||
      !buildDef(*MaskFixture->Function, Target, *MaskFixture->Block, *AcrossCall, 5) ||
      !buildMask(*MaskFixture->Function, Target, *MaskFixture->Block,
                 rcc::test::createTestCallPreservedMask()) ||
      !buildUse(*MaskFixture->Function, Target, *MaskFixture->Block, *AcrossCall) ||
      !prepareForAllocation(*MaskFixture->Function, Target)) {
    return;
  }
  auto Allocated = runAllocator(*MaskFixture->Function, Target, Info, AllocatorKind::Basic);
  const rcc::MachineInstr *FinalUse = MaskFixture->Block->lastInstruction();
  const auto G1 = physical(rcc::test::TestRegister::G1);
  Context.expect(Allocated && FinalUse != nullptr &&
                     FinalUse->operandAt(0)->registerValue() == rcc::Register::fromPhysical(G1),
                 "global allocation must keep call-crossing values in a preserved register");
}

void testInvalidRewriteMappings(TestContext &Context, const rcc::TargetDescription &Target) {
  auto ExpectAtomicFailure = [&](FunctionFixture &Fixture, const rcc::VirtRegMap &Mapping,
                                 std::string_view Message) {
    const std::vector<std::uint64_t> Before = functionSignature(*Fixture.Function);
    auto Rewritten = rcc::rewriteVirtualRegisters(*Fixture.Function, Target, Mapping);
    Context.expect(!Rewritten &&
                       Rewritten.error().code() == rcc::ErrorCode::RegisterAllocationFailed &&
                       functionSignature(*Fixture.Function) == Before,
                   Message);
  };

  auto ClassFixture = createFunction("invalid-class-mapping", 40);
  if (!ClassFixture)
    return;
  auto ClassValue = createVReg(*ClassFixture->Function);
  if (!ClassValue ||
      !buildDef(*ClassFixture->Function, Target, *ClassFixture->Block, *ClassValue, 1) ||
      !buildUse(*ClassFixture->Function, Target, *ClassFixture->Block, *ClassValue) ||
      !prepareForAllocation(*ClassFixture->Function, Target)) {
    return;
  }
  rcc::VirtRegMap ClassMapping(*ClassFixture->Function);
  const auto F0 = physical(rcc::test::TestRegister::F0);
  if (!ClassMapping.add(*ClassValue) || !ClassMapping.assign(*ClassValue, F0))
    return;
  ExpectAtomicFailure(*ClassFixture, ClassMapping,
                      "class-incompatible rewrite mappings must fail atomically");

  auto ReservedFixture = createFunction("reserved-register-mapping", 41);
  if (!ReservedFixture)
    return;
  auto ReservedValue = createVReg(*ReservedFixture->Function);
  const auto G0 = physical(rcc::test::TestRegister::G0);
  if (!ReservedValue ||
      !buildDef(*ReservedFixture->Function, Target, *ReservedFixture->Block, *ReservedValue, 2) ||
      !buildUse(*ReservedFixture->Function, Target, *ReservedFixture->Block, *ReservedValue) ||
      !ReservedFixture->Function->registerInfo().reservePhysicalRegister(G0) ||
      !prepareForAllocation(*ReservedFixture->Function, Target)) {
    return;
  }
  rcc::VirtRegMap ReservedMapping(*ReservedFixture->Function);
  if (!ReservedMapping.add(*ReservedValue) || !ReservedMapping.assign(*ReservedValue, G0))
    return;
  ExpectAtomicFailure(*ReservedFixture, ReservedMapping,
                      "reserved-register rewrite mappings must fail atomically");

  auto OverlapFixture = createFunction("overlapping-register-mapping", 42);
  if (!OverlapFixture)
    return;
  auto Left = createVReg(*OverlapFixture->Function);
  auto Right = createVReg(*OverlapFixture->Function);
  if (!Left || !Right ||
      !buildDef(*OverlapFixture->Function, Target, *OverlapFixture->Block, *Left, 3) ||
      !buildDef(*OverlapFixture->Function, Target, *OverlapFixture->Block, *Right, 4) ||
      !buildUse(*OverlapFixture->Function, Target, *OverlapFixture->Block, *Left) ||
      !buildUse(*OverlapFixture->Function, Target, *OverlapFixture->Block, *Right) ||
      !prepareForAllocation(*OverlapFixture->Function, Target)) {
    return;
  }
  rcc::VirtRegMap OverlapMapping(*OverlapFixture->Function);
  if (!OverlapMapping.add(*Left) || !OverlapMapping.add(*Right) ||
      !OverlapMapping.assign(*Left, G0) || !OverlapMapping.assign(*Right, G0)) {
    return;
  }
  ExpectAtomicFailure(*OverlapFixture, OverlapMapping,
                      "interfering rewrite mappings must fail atomically");
}

} // namespace

int main() {
  TestContext Context;
  auto Target = createTarget();
  auto SharedUnitTarget = createTarget(true);
  Context.expect(Target.has_value(), "register-allocation TestTarget must be constructible");
  Context.expect(SharedUnitTarget.has_value(),
                 "shared-unit register-allocation TestTarget must be constructible");
  if (!Target) {
    std::cerr << "target construction error: " << Target.error().message() << '\n';
    return Context.result();
  }
  TestRegisterAllocationInfo Info;
  testVirtRegMapAndEdit(Context, *Target);
  testLiveRegMatrix(Context, *Target);
  if (SharedUnitTarget)
    testSharedUnitLaneMatrix(Context, *SharedUnitTarget);
  testAllocatorsUnderPressure(Context, *Target, Info);
  testFailureAtomicity(Context, *Target);
  testFastBlockBoundary(Context, *Target, Info);
  testFastRegMaskCalls(Context, *Target, Info);
  testTiedAndEarlyClobber(Context, *Target, Info);
  testRematerialization(Context, *Target, Info);
  testRewriteAndConstraints(Context, *Target, Info);
  testInvalidRewriteMappings(Context, *Target);
  return Context.result();
}
