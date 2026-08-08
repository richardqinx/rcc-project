// SPDX-License-Identifier: GPL-3.0-or-later

// The private test header has no stable include-root-relative guard name.
// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_BACKEND_REGISTER_ALLOCATION_TEST_SUPPORT_H
#define RCC_BACKEND_REGISTER_ALLOCATION_TEST_SUPPORT_H

#include "Target/TestTarget.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/LiveRangeEdit.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineIRBuilder.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineLiveness.h"
#include "rcc/CodeGen/RegAllocFast.h"
#include "rcc/CodeGen/RegAllocPBQP.h"
#include "rcc/CodeGen/Spiller.h"
#include "rcc/CodeGen/TargetRegisterAllocationInfo.h"
#include "rcc/CodeGen/VirtRegMap.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace rcc::test::ra {

enum class AllocatorKind : std::uint8_t { Basic, Fast, PBQP };

inline constexpr std::uint32_t DefOpcode = 3000;
inline constexpr std::uint32_t RematerializableDefOpcode = 3001;
inline constexpr std::uint32_t UseOpcode = 3002;
inline constexpr std::uint32_t BinaryOpcode = 3003;
inline constexpr std::uint32_t SpillLoadOpcode = 3004;
inline constexpr std::uint32_t SpillStoreOpcode = 3005;
inline constexpr std::uint32_t MaskOpcode = 3006;
inline constexpr std::uint32_t EarlyOpcode = 3007;
inline constexpr std::uint32_t TiedOpcode = 3008;
inline constexpr std::uint32_t CallDefOpcode = 3009;

[[nodiscard]] constexpr Opcode testOpcode(std::uint32_t Value) noexcept {
  return Opcode::fromValue(Value);
}

[[nodiscard]] inline PhysicalRegister physical(std::uint32_t Number) {
  return *PhysicalRegister::create(Number);
}

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

[[nodiscard]] inline OperandDescriptor registerOperand(RegisterOperandRole Role,
                                                       bool EarlyClobber = false) {
  OperandDescriptor Result;
  Result.Kinds = {MachineOperand::Kind::Register};
  Result.RegisterRole = Role;
  Result.AllowSubReg = true;
  Result.EarlyClobber = EarlyClobber;
  return Result;
}

[[nodiscard]] inline OperandDescriptor plainOperand(MachineOperand::Kind Kind) {
  OperandDescriptor Result;
  Result.Kinds = {Kind};
  return Result;
}

[[nodiscard]] inline InstructionDescriptor descriptor(std::uint32_t Value, std::string Name) {
  InstructionDescriptor Result;
  Result.InstructionOpcode = testOpcode(Value);
  Result.Name = std::move(Name);
  Result.Flags = {InstructionFlag::ProducesCode};
  return Result;
}

[[nodiscard]] inline Expected<TargetDescription> createTarget() {
  std::vector<InstructionDescriptor> Descriptors;

  auto Def = descriptor(DefOpcode, "RA_DEF");
  Def.Operands = {registerOperand(RegisterOperandRole::Def),
                  plainOperand(MachineOperand::Kind::Immediate)};
  Def.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Def));

  auto Remat = descriptor(RematerializableDefOpcode, "RA_REMAT_DEF");
  Remat.Operands = {registerOperand(RegisterOperandRole::Def),
                    plainOperand(MachineOperand::Kind::Immediate)};
  Remat.NumExplicitDefs = 1;
  Remat.Flags.set(InstructionFlag::Rematerializable);
  Descriptors.push_back(std::move(Remat));

  auto Use = descriptor(UseOpcode, "RA_USE");
  Use.Operands = {registerOperand(RegisterOperandRole::Use)};
  Use.Flags.set(InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(Use));

  auto Binary = descriptor(BinaryOpcode, "RA_BINARY");
  Binary.Operands = {registerOperand(RegisterOperandRole::Def),
                     registerOperand(RegisterOperandRole::Use),
                     registerOperand(RegisterOperandRole::Use)};
  Binary.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Binary));

  auto Reload = descriptor(SpillLoadOpcode, "RA_RELOAD");
  Reload.Operands = {registerOperand(RegisterOperandRole::Def),
                     plainOperand(MachineOperand::Kind::FrameIndex)};
  Reload.NumExplicitDefs = 1;
  Reload.Flags.set(InstructionFlag::MayLoad);
  Reload.RequiresMemoryOperands = true;
  Descriptors.push_back(std::move(Reload));

  auto Spill = descriptor(SpillStoreOpcode, "RA_SPILL");
  Spill.Operands = {registerOperand(RegisterOperandRole::Use),
                    plainOperand(MachineOperand::Kind::FrameIndex)};
  Spill.Flags.set(InstructionFlag::MayStore);
  Spill.RequiresMemoryOperands = true;
  Descriptors.push_back(std::move(Spill));

  auto Mask = descriptor(MaskOpcode, "RA_MASK");
  Mask.Operands = {plainOperand(MachineOperand::Kind::RegisterMask)};
  Mask.Flags.set(InstructionFlag::Call);
  Mask.Flags.set(InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(Mask));

  auto Early = descriptor(EarlyOpcode, "RA_EARLY");
  Early.Operands = {registerOperand(RegisterOperandRole::Def, true),
                    registerOperand(RegisterOperandRole::Use)};
  Early.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Early));

  auto Tied = descriptor(TiedOpcode, "RA_TIED");
  auto TiedDef = registerOperand(RegisterOperandRole::Def);
  auto TiedUse = registerOperand(RegisterOperandRole::Use);
  TiedDef.DefaultTiedOperand = 1;
  TiedUse.DefaultTiedOperand = 0;
  Tied.Operands = {TiedDef, TiedUse};
  Tied.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Tied));

  auto CallDef = descriptor(CallDefOpcode, "RA_CALL_DEF");
  CallDef.Operands = {registerOperand(RegisterOperandRole::Def),
                      plainOperand(MachineOperand::Kind::RegisterMask)};
  CallDef.NumExplicitDefs = 1;
  CallDef.Flags.set(InstructionFlag::Call);
  CallDef.Flags.set(InstructionFlag::HasSideEffects);
  Descriptors.push_back(std::move(CallDef));

  auto Copy = descriptor(static_cast<std::uint32_t>(GenericOpcode::COPY), "COPY");
  Copy.Operands = {registerOperand(RegisterOperandRole::Def),
                   registerOperand(RegisterOperandRole::Use)};
  Copy.NumExplicitDefs = 1;
  Descriptors.push_back(std::move(Copy));

  auto Instructions = InstructionSet::create(std::move(Descriptors), {});
  if (!Instructions)
    return std::unexpected(Instructions.error());
  return test::createTestTargetDescription(std::move(*Instructions));
}

struct FunctionFixture final {
  std::unique_ptr<MachineFunction> Function;
  MachineBasicBlock *Block = nullptr;
};

[[nodiscard]] inline Expected<FunctionFixture> createFunction(std::string Name, std::uint64_t ID) {
  auto Function = MachineFunction::create(std::move(Name), MachineFunctionStableID::fromValue(ID));
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

[[nodiscard]] inline Expected<Register>
createVReg(MachineFunction &Function, RegisterClassID Class = TestRegisterClass::GPR64) {
  auto Type = LLT::makeInteger(64);
  if (!Type)
    return std::unexpected(Type.error());
  auto Reg = Function.registerInfo().createVirtualRegister(*Type);
  if (!Reg)
    return std::unexpected(Reg.error());
  if (auto Result = Function.registerInfo().setRegisterBank(*Reg, TestRegisterBank::GPR); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Function.registerInfo().setRegisterClass(*Reg, Class); !Result)
    return std::unexpected(Result.error());
  return *Reg;
}

[[nodiscard]] inline Expected<MachineInstr *>
buildDef(MachineFunction &Function, const TargetDescription &Target, MachineBasicBlock &Block,
         Register Reg, std::int64_t Value, SubRegIndex SubReg = {}, bool Rematerializable = false) {
  MachineIRBuilder Builder(Function, Target);
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

[[nodiscard]] inline Expected<MachineInstr *>
buildUse(MachineFunction &Function, const TargetDescription &Target, MachineBasicBlock &Block,
         Register Reg, SubRegIndex SubReg = {}, bool Debug = false) {
  MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(UseOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  RegisterOperandFlags Flags;
  if (Debug)
    Flags.set(RegisterOperandFlag::Debug);
  if (auto Result = Pending->addUse(Reg, Flags, SubReg); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] inline Expected<MachineInstr *> buildCopy(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineBasicBlock &Block,
                                                        Register Destination, Register Source) {
  MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(opcode(GenericOpcode::COPY));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addDef(Destination); !Result)
    return std::unexpected(Result.error());
  if (auto Result = Pending->addUse(Source); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] inline Expected<MachineInstr *> buildMask(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineBasicBlock &Block,
                                                        std::shared_ptr<const RegisterMask> Mask) {
  MachineIRBuilder Builder(Function, Target);
  if (auto Result = Builder.setInsertPoint(Block); !Result)
    return std::unexpected(Result.error());
  auto Pending = Builder.buildInstr(testOpcode(MaskOpcode));
  if (!Pending)
    return std::unexpected(Pending.error());
  if (auto Result = Pending->addRegisterMask(std::move(Mask)); !Result)
    return std::unexpected(Result.error());
  return Pending->build();
}

[[nodiscard]] inline Expected<MachineInstr *> buildTied(MachineFunction &Function,
                                                        const TargetDescription &Target,
                                                        MachineBasicBlock &Block, Register Reg) {
  MachineIRBuilder Builder(Function, Target);
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

[[nodiscard]] inline Expected<void> prepareForAllocation(MachineFunction &Function,
                                                         const TargetDescription &Target) {
  auto Rebuilt = rebuildMachineLiveness(Function, Target);
  if (!Rebuilt)
    return std::unexpected(Rebuilt.error());
  for (const MachineFunctionProperty Property : {
           MachineFunctionProperty::TracksLiveness,
           MachineFunctionProperty::Legalized,
           MachineFunctionProperty::RegBankSelected,
           MachineFunctionProperty::Selected,
           MachineFunctionProperty::NoPHIs,
           MachineFunctionProperty::TiedOpsRewritten,
       })
    Function.properties().set(Property);
  return {};
}

class TestRegisterAllocationInfo : public TargetRegisterAllocationInfo {
public:
  std::uint64_t RegisterCost = 0;
  std::uint64_t SpillCost = 1;

  [[nodiscard]] Expected<void> validateSpillReload(RegisterClassID Class,
                                                   const TargetDescription &Target) const override {
    const RegisterClassDesc *Description = Target.registers().registerClass(Class);
    if (Description == nullptr || !Description->Allocatable || Description->SpillSizeInBits == 0 ||
        Description->SpillAlignmentInBits == 0)
      return makeUnexpected(ErrorCode::Unsupported, "test register class is not spillable");
    return {};
  }

  [[nodiscard]] Expected<MachineInstr *> emitReload(MachineIRBuilder &Builder, Register Destination,
                                                    RegisterClassID Class, FrameIndex StackSlot,
                                                    LaneMask Lanes) const override {
    auto Validated = validateSpillReload(Class, Builder.target());
    if (!Validated)
      return std::unexpected(Validated.error());
    if (Lanes.empty())
      return makeUnexpected(ErrorCode::InvalidArgument, "test reload requires lanes");
    auto Size = MemorySize::fixed(8);
    auto Align = Alignment::create(8);
    if (!Size || !Align)
      return makeUnexpected(ErrorCode::InternalError, "test memory shape is invalid");
    MachineMemOperandDesc Memory;
    Memory.Accesses = {MemoryAccessFlag::Load};
    Memory.Size = *Size;
    Memory.Align = *Align;
    Memory.Base = MemoryBase::frameIndex(StackSlot);
    auto MMO = Builder.function().createMemOperand(std::move(Memory));
    if (!MMO)
      return std::unexpected(MMO.error());
    auto Pending = Builder.buildInstr(testOpcode(SpillLoadOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addDef(Destination); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addFrameIndex(StackSlot); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addMemOperand(**MMO); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] Expected<MachineInstr *> emitSpill(MachineIRBuilder &Builder, Register Source,
                                                   RegisterClassID Class, FrameIndex StackSlot,
                                                   LaneMask Lanes) const override {
    auto Validated = validateSpillReload(Class, Builder.target());
    if (!Validated)
      return std::unexpected(Validated.error());
    if (Lanes.empty())
      return makeUnexpected(ErrorCode::InvalidArgument, "test spill requires lanes");
    auto Size = MemorySize::fixed(8);
    auto Align = Alignment::create(8);
    if (!Size || !Align)
      return makeUnexpected(ErrorCode::InternalError, "test memory shape is invalid");
    MachineMemOperandDesc Memory;
    Memory.Accesses = {MemoryAccessFlag::Store};
    Memory.Size = *Size;
    Memory.Align = *Align;
    Memory.Base = MemoryBase::frameIndex(StackSlot);
    auto MMO = Builder.function().createMemOperand(std::move(Memory));
    if (!MMO)
      return std::unexpected(MMO.error());
    auto Pending = Builder.buildInstr(testOpcode(SpillStoreOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addUse(Source); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addFrameIndex(StackSlot); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addMemOperand(**MMO); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] Expected<bool> isRematerializable(const MachineInstr &Instruction,
                                                  Register VirtualReg, const MachineFunction &,
                                                  const TargetDescription &) const override {
    const Register *Defined = Instruction.operandAt(0)->getIf<Register>();
    return Instruction.opcode() == testOpcode(RematerializableDefOpcode) && Defined != nullptr &&
           *Defined == VirtualReg;
  }

  [[nodiscard]] Expected<MachineInstr *>
  rematerialize(MachineIRBuilder &Builder, const MachineInstr &Original, Register Destination,
                Register OriginalRegister, const TargetDescription &Target) const override {
    auto Accepted = isRematerializable(Original, OriginalRegister, Builder.function(), Target);
    if (!Accepted)
      return std::unexpected(Accepted.error());
    if (!*Accepted)
      return makeUnexpected(ErrorCode::InvalidArgument, "test instruction is not rematerializable");
    auto Pending = Builder.buildInstr(testOpcode(RematerializableDefOpcode));
    if (!Pending)
      return std::unexpected(Pending.error());
    if (auto Result = Pending->addDef(Destination); !Result)
      return std::unexpected(Result.error());
    if (auto Result = Pending->addOperand(Original.operandAt(1)->cloneDetached()); !Result)
      return std::unexpected(Result.error());
    return Pending->build();
  }

  [[nodiscard]] std::uint64_t physicalRegisterCost(const MachineFunction &, Register,
                                                   PhysicalRegister, RegisterClassID,
                                                   bool) const noexcept override {
    return RegisterCost;
  }

  [[nodiscard]] std::uint64_t spillCost(const MachineFunction &, const LiveInterval &,
                                        RegisterClassID) const noexcept override {
    return SpillCost;
  }
};

[[nodiscard]] inline Expected<void> runAllocator(MachineFunction &Function,
                                                 const TargetDescription &Target,
                                                 const TargetRegisterAllocationInfo &Info,
                                                 AllocatorKind Kind,
                                                 RegisterAllocationOptions Options = {}) {
  MachineAnalysisManager Analyses;
  auto Registered = registerStage11MachineAnalyses(Analyses);
  if (!Registered)
    return std::unexpected(Registered.error());
  std::unique_ptr<MachineFunctionPass> Pass;
  switch (Kind) {
  case AllocatorKind::Basic:
    Pass = createBasicAllocatorPass(Info, Options);
    break;
  case AllocatorKind::Fast:
    Pass = std::make_unique<FastAllocatorPass>(Info);
    break;
  case AllocatorKind::PBQP:
    Pass = std::make_unique<PBQPAllocatorPass>(Info, Options);
    break;
  }
  MachinePassManager Pipeline;
  auto Added = Pipeline.addPass(std::move(Pass));
  if (!Added)
    return std::unexpected(Added.error());
  return Pipeline.run(Function, Target, Analyses);
}

[[nodiscard]] inline std::size_t countOpcode(const MachineFunction &Function, Opcode OpcodeValue) {
  std::size_t Count = 0;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next())
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next())
      Count += Instruction->opcode() == OpcodeValue ? 1U : 0U;
  return Count;
}

} // namespace rcc::test::ra

#endif // RCC_BACKEND_REGISTER_ALLOCATION_TEST_SUPPORT_H
