// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/GenericOpcodes.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
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

[[nodiscard]] std::vector<rcc::InstructionDescriptor> copyDescriptors() {
  const auto Source = rcc::genericInstructionDescriptors();
  return {Source.begin(), Source.end()};
}

[[nodiscard]] rcc::MachineOperand registerOperand(rcc::Register Reg, bool IsDef) {
  rcc::RegisterOperandFlags Flags;
  Flags.set(IsDef ? rcc::RegisterOperandFlag::Define : rcc::RegisterOperandFlag::Use);
  auto Operand = rcc::MachineOperand::registerOperand(Reg, Flags);
  if (!Operand)
    std::terminate();
  return std::move(*Operand);
}

[[nodiscard]] rcc::Register createVirtualRegister(rcc::MachineFunction &Function, rcc::LLT Type) {
  auto Reg = Function.registerInfo().createVirtualRegister(Type);
  if (!Reg)
    std::terminate();
  return *Reg;
}

[[nodiscard]] rcc::MachineInstr &createInstruction(rcc::MachineFunction &Function,
                                                   rcc::GenericOpcode OpcodeValue,
                                                   std::vector<rcc::MachineOperand> Operands) {
  auto Instruction = Function.createInstruction(rcc::opcode(OpcodeValue));
  if (!Instruction)
    std::terminate();
  for (rcc::MachineOperand &Operand : Operands) {
    if (!(*Instruction)->addOperand(std::move(Operand)))
      std::terminate();
  }
  return **Instruction;
}

[[nodiscard]] rcc::MachineMemOperandDesc
memoryDescription(rcc::MemoryAccessFlags Accesses, std::uint64_t Bytes,
                  std::uint32_t AddressSpace = 0,
                  rcc::AtomicOperation AtomicKind = rcc::AtomicOperation::None) {
  rcc::MachineMemOperandDesc Description;
  Description.Accesses = Accesses;
  Description.AtomicKind = AtomicKind;
  if (AtomicKind == rcc::AtomicOperation::ReadModifyWrite) {
    Description.SuccessOrdering = rcc::AtomicOrdering::AcquireRelease;
  } else if (AtomicKind == rcc::AtomicOperation::CompareExchange) {
    Description.SuccessOrdering = rcc::AtomicOrdering::AcquireRelease;
    Description.FailureOrdering = rcc::AtomicOrdering::Acquire;
  }
  Description.Size = *rcc::MemorySize::fixed(Bytes);
  Description.Align = *rcc::Alignment::create(std::bit_floor(Bytes));
  Description.AddressSpace = AddressSpace;
  Description.Base = rcc::MemoryBase::stack();
  return Description;
}

void addMemOperand(rcc::MachineFunction &Function, rcc::MachineInstr &Instruction,
                   rcc::MachineMemOperandDesc Description) {
  auto Memory = Function.createMemOperand(std::move(Description));
  if (!Memory || !Instruction.addMemOperand(**Memory))
    std::terminate();
}

void testCompleteRegistry(TestContext &Context) {
  static_assert(rcc::CorePseudoOpcodeCount == 52);
  static_assert(rcc::GenericOpcodeCount == 278);

  const auto Descriptors = rcc::genericInstructionDescriptors();
  Context.expect(Descriptors.size() == 330,
                 "the target-independent registry must contain 52 core and 278 generic opcodes");

  std::unordered_set<std::string> Names;
  std::unordered_set<rcc::Opcode> Opcodes;
  bool ClassesAreCorrect = true;
  for (const rcc::InstructionDescriptor &Descriptor : Descriptors) {
    ClassesAreCorrect &= Descriptor.InstructionOpcode.isValid() && !Descriptor.Name.empty() &&
                         Names.insert(Descriptor.Name).second &&
                         Opcodes.insert(Descriptor.InstructionOpcode).second;
    if (rcc::isGenericOpcode(Descriptor.InstructionOpcode)) {
      ClassesAreCorrect &= Descriptor.Flags.has(rcc::InstructionFlag::Generic) &&
                           !Descriptor.Flags.has(rcc::InstructionFlag::Pseudo);
    } else {
      ClassesAreCorrect &= rcc::isCorePseudoOpcode(Descriptor.InstructionOpcode) &&
                           Descriptor.Flags.has(rcc::InstructionFlag::Pseudo) &&
                           !Descriptor.Flags.has(rcc::InstructionFlag::Generic);
    }
  }
  Context.expect(ClassesAreCorrect,
                 "every target-independent opcode must have one valid name, value, and class");
  Context.expect(rcc::getGenericOpcodeName(rcc::GenericOpcode::PHI) == "PHI" &&
                     rcc::getGenericOpcodeName(rcc::GenericOpcode::G_ASSERT_SEXT) ==
                         "G_ASSERT_SEXT" &&
                     rcc::getGenericOpcodeName(rcc::GenericOpcode::G_UBFX) == "G_UBFX" &&
                     rcc::getGenericOpcodeName(rcc::GenericOpcode::Invalid).empty(),
                 "opcode names must cover both ranges without assigning marker names");

  auto Set = rcc::InstructionSet::create(copyDescriptors());
  Context.expect(Set.has_value(), "the complete target-independent descriptor set must validate");
}

void testCoreOpcodeContracts(TestContext &Context) {
  struct Contract final {
    rcc::GenericOpcode OpcodeValue;
    std::size_t DescriptorOperands;
    std::size_t ExplicitDefs;
    bool Variadic;
    std::size_t MinimumRepetitions;
    bool Meta;
    bool TerminalEmission;
    rcc::PseudoOwner Owner;
    rcc::PipelineStage Deadline;
  };

#define RCC_META(Name, Operands, Defs, Variadic, Minimum)                                          \
  Contract{rcc::GenericOpcode::Name,                                                               \
           Operands,                                                                               \
           Defs,                                                                                   \
           Variadic,                                                                               \
           Minimum,                                                                                \
           true,                                                                                   \
           false,                                                                                  \
           rcc::PseudoOwner::None,                                                                 \
           rcc::PipelineStage::Emission}
#define RCC_PSEUDO(Name, Operands, Defs, Variadic, Minimum, Owner, Deadline)                       \
  Contract{rcc::GenericOpcode::Name,                                                               \
           Operands,                                                                               \
           Defs,                                                                                   \
           Variadic,                                                                               \
           Minimum,                                                                                \
           false,                                                                                  \
           false,                                                                                  \
           rcc::PseudoOwner::Owner,                                                                \
           rcc::PipelineStage::Deadline}
#define RCC_TERMINAL(Name, Operands, Defs, Variadic, Minimum)                                      \
  Contract {                                                                                       \
    rcc::GenericOpcode::Name, Operands, Defs, Variadic, Minimum, false, true,                      \
        rcc::PseudoOwner::AsmPrinter, rcc::PipelineStage::Emission                                 \
  }

  const std::array Contracts{
      RCC_PSEUDO(PHI, 3, 1, true, 1, PhiElimination, SSAEliminated),
      RCC_TERMINAL(INLINEASM, 1, 0, true, 0),
      RCC_TERMINAL(INLINEASM_BR, 1, 0, true, 0),
      RCC_META(CFI_INSTRUCTION, 1, 0, false, 0),
      RCC_META(EH_LABEL, 1, 0, false, 0),
      RCC_META(GC_LABEL, 1, 0, false, 0),
      RCC_META(ANNOTATION_LABEL, 1, 0, false, 0),
      RCC_META(KILL, 1, 0, true, 0),
      RCC_PSEUDO(EXTRACT_SUBREG, 3, 1, false, 0, PostRAExpansion, PostRAFinalized),
      RCC_PSEUDO(INSERT_SUBREG, 4, 1, false, 0, TwoAddressRewrite, TiedOpsRewritten),
      RCC_META(IMPLICIT_DEF, 1, 1, false, 0),
      RCC_PSEUDO(INIT_UNDEF, 1, 1, false, 0, RegisterAllocator, RegisterAllocated),
      RCC_PSEUDO(SUBREG_TO_REG, 3, 1, false, 0, PostRAExpansion, PostRAFinalized),
      RCC_PSEUDO(COPY_TO_REGCLASS, 3, 1, false, 0, RegisterAllocator, RegisterAllocated),
      RCC_META(DBG_VALUE, 1, 0, true, 0),
      RCC_META(DBG_VALUE_LIST, 1, 0, true, 0),
      RCC_META(DBG_INSTR_REF, 1, 0, true, 0),
      RCC_META(DBG_PHI, 1, 0, true, 0),
      RCC_META(DBG_LABEL, 1, 0, false, 0),
      RCC_PSEUDO(REG_SEQUENCE, 3, 1, true, 1, TwoAddressRewrite, TiedOpsRewritten),
      RCC_PSEUDO(COPY, 2, 1, false, 0, PostRAExpansion, PostRAFinalized),
      RCC_PSEUDO(COPY_LANEMASK, 3, 1, false, 0, PostRAExpansion, PostRAFinalized),
      RCC_META(BUNDLE, 1, 0, true, 0),
      RCC_META(LIFETIME_START, 1, 0, false, 0),
      RCC_META(LIFETIME_END, 1, 0, false, 0),
      RCC_META(PSEUDO_PROBE, 4, 0, false, 0),
      RCC_META(ARITH_FENCE, 2, 1, false, 0),
      RCC_TERMINAL(STACKMAP, 3, 0, true, 0),
      RCC_TERMINAL(FENTRY_CALL, 0, 0, false, 0),
      RCC_TERMINAL(PATCHPOINT, 7, 1, true, 0),
      RCC_PSEUDO(LOAD_STACK_GUARD, 1, 1, false, 0, InstructionSelect, Selected),
      RCC_PSEUDO(PREALLOCATED_SETUP, 1, 0, false, 0, InstructionSelect, Selected),
      RCC_PSEUDO(PREALLOCATED_ARG, 3, 1, false, 0, InstructionSelect, Selected),
      RCC_TERMINAL(STATEPOINT, 1, 0, true, 0),
      RCC_META(LOCAL_ESCAPE, 2, 0, false, 0),
      RCC_TERMINAL(FAULTING_OP, 2, 1, true, 0),
      RCC_TERMINAL(PATCHABLE_OP, 1, 0, true, 0),
      RCC_TERMINAL(PATCHABLE_FUNCTION_ENTER, 0, 0, false, 0),
      RCC_TERMINAL(PATCHABLE_RET, 1, 0, true, 0),
      RCC_TERMINAL(PATCHABLE_FUNCTION_EXIT, 0, 0, false, 0),
      RCC_TERMINAL(PATCHABLE_TAIL_CALL, 1, 0, true, 0),
      RCC_TERMINAL(PATCHABLE_EVENT_CALL, 2, 0, false, 0),
      RCC_TERMINAL(PATCHABLE_TYPED_EVENT_CALL, 3, 0, false, 0),
      RCC_PSEUDO(ICALL_BRANCH_FUNNEL, 1, 0, true, 0, PostRAExpansion, PostRAFinalized),
      RCC_META(FAKE_USE, 1, 0, true, 0),
      RCC_META(MEMBARRIER, 0, 0, false, 0),
      RCC_META(JUMP_TABLE_DEBUG_INFO, 1, 0, false, 0),
      RCC_META(RELOC_NONE, 1, 0, false, 0),
      RCC_META(CONVERGENCECTRL_ENTRY, 1, 1, false, 0),
      RCC_META(CONVERGENCECTRL_ANCHOR, 1, 1, false, 0),
      RCC_META(CONVERGENCECTRL_LOOP, 2, 1, false, 0),
      RCC_META(CONVERGENCECTRL_GLUE, 1, 0, false, 0),
  };
#undef RCC_TERMINAL
#undef RCC_PSEUDO
#undef RCC_META

  static_assert(Contracts.size() == rcc::CorePseudoOpcodeCount);
  const auto Descriptors = rcc::genericInstructionDescriptors();
  bool Matches = true;
  for (const Contract &Expected : Contracts) {
    const auto Found = std::ranges::find_if(
        Descriptors, [&Expected](const rcc::InstructionDescriptor &Descriptor) {
          return Descriptor.InstructionOpcode == rcc::opcode(Expected.OpcodeValue);
        });
    if (Found == Descriptors.end()) {
      Matches = false;
      continue;
    }
    Matches &=
        Found->Operands.size() == Expected.DescriptorOperands &&
        Found->NumExplicitDefs == Expected.ExplicitDefs && Found->Variadic == Expected.Variadic &&
        Found->MinimumVariadicRepetitions == Expected.MinimumRepetitions &&
        Found->Flags.has(rcc::InstructionFlag::Meta) == Expected.Meta &&
        Found->Flags.has(rcc::InstructionFlag::TerminalEmission) == Expected.TerminalEmission &&
        Found->Owner == Expected.Owner && Found->Deadline == Expected.Deadline;
  }
  Context.expect(Matches,
                 "all 52 core opcodes must retain their reviewed shape and lifecycle contracts");

  const auto InlineAsm = std::ranges::find_if(Descriptors, [](const auto &Descriptor) {
    return Descriptor.InstructionOpcode == rcc::opcode(rcc::GenericOpcode::INLINEASM);
  });
  const auto InlineAsmBr = std::ranges::find_if(Descriptors, [](const auto &Descriptor) {
    return Descriptor.InstructionOpcode == rcc::opcode(rcc::GenericOpcode::INLINEASM_BR);
  });
  const auto Copy = std::ranges::find_if(Descriptors, [](const auto &Descriptor) {
    return Descriptor.InstructionOpcode == rcc::opcode(rcc::GenericOpcode::COPY);
  });
  Context.expect(InlineAsm != Descriptors.end() && InlineAsmBr != Descriptors.end() &&
                     !InlineAsm->Flags.has(rcc::InstructionFlag::HasSideEffects) &&
                     InlineAsmBr->Flags.has(rcc::InstructionFlag::HasSideEffects),
                 "INLINEASM_BR must be intrinsically side-effecting while INLINEASM uses its "
                 "operand flags");
  Context.expect(Copy != Descriptors.end() && Copy->AllowExtraImplicitOperands,
                 "COPY must allow dynamic implicit super-register operands");
}

void testShapesAndTypes(TestContext &Context) {
  auto Set = rcc::InstructionSet::create(copyDescriptors());
  auto Function =
      rcc::MachineFunction::create("generic", rcc::MachineFunctionStableID::fromValue(1));
  Context.expect(Set.has_value() && Function.has_value(),
                 "generic opcode tests require a descriptor set and machine function");
  if (!Set || !Function)
    return;

  auto I32 = rcc::LLT::makeInteger(32);
  auto I64 = rcc::LLT::makeInteger(64);
  auto I1 = rcc::LLT::makeInteger(1);
  auto S16 = rcc::LLT::makeAnyScalar(16);
  auto F32 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEESingle);
  auto F64 = rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  auto P64 = rcc::LLT::makePointer(0, 64);
  auto V2I32 = I32 ? rcc::LLT::makeFixedVector(2, *I32) : rcc::Expected<rcc::LLT>{};
  auto V4I32 = I32 ? rcc::LLT::makeFixedVector(4, *I32) : rcc::Expected<rcc::LLT>{};
  auto V2P64 = P64 ? rcc::LLT::makeFixedVector(2, *P64) : rcc::Expected<rcc::LLT>{};
  if (!I1 || !I32 || !I64 || !S16 || !F32 || !F64 || !P64 || !V2I32 || !V4I32 || !V2P64)
    std::terminate();
  auto Dst32 = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Src32A = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Src32B = (*Function)->registerInfo().createVirtualRegister(*I32);
  auto Wide = (*Function)->registerInfo().createVirtualRegister(*I64);
  auto Address = (*Function)->registerInfo().createVirtualRegister(*P64);
  auto NewAddress = (*Function)->registerInfo().createVirtualRegister(*P64);
  const rcc::Register Cond = createVirtualRegister(**Function, *I1);
  const rcc::Register Float32 = createVirtualRegister(**Function, *F32);
  const rcc::Register Float64 = createVirtualRegister(**Function, *F64);
  const rcc::Register Scalar16 = createVirtualRegister(**Function, *S16);
  const rcc::Register Vec2A = createVirtualRegister(**Function, *V2I32);
  const rcc::Register Vec2B = createVirtualRegister(**Function, *V2I32);
  const rcc::Register Vec4 = createVirtualRegister(**Function, *V4I32);
  const rcc::Register PointerVector = createVirtualRegister(**Function, *V2P64);
  if (!Dst32 || !Src32A || !Src32B || !Wide || !Address || !NewAddress)
    std::terminate();

  std::vector<rcc::MachineOperand> AddOperands;
  AddOperands.push_back(registerOperand(*Dst32, true));
  AddOperands.push_back(registerOperand(*Src32A, false));
  AddOperands.push_back(registerOperand(*Src32B, false));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ADD), AddOperands,
                                               (*Function)->registerInfo())
                     .has_value(),
                 "same-typed integer operands must satisfy G_ADD");
  AddOperands.back() = registerOperand(*Wide, false);
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ADD), AddOperands,
                                                (*Function)->registerInfo()),
                 "G_ADD must reject a source from a different LLT equality group");

  auto ZeroImmediate = rcc::IntegerImmediate::fromUnsigned(64, 0);
  std::vector<rcc::MachineOperand> PointerConstant;
  PointerConstant.push_back(registerOperand(*Address, true));
  if (ZeroImmediate)
    PointerConstant.push_back(rcc::MachineOperand::integerImmediate(std::move(*ZeroImmediate)));
  Context.expect(PointerConstant.size() == 2 &&
                     Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_CONSTANT),
                                                   PointerConstant, (*Function)->registerInfo())
                         .has_value(),
                 "G_CONSTANT must accept a scalar pointer destination");

  auto Block0 = (*Function)->createBasicBlock();
  auto Block1 = (*Function)->createBasicBlock();
  if (!Block0 || !Block1 || !(*Function)->appendBlock(**Block0) ||
      !(*Function)->appendBlock(**Block1)) {
    std::terminate();
  }
  std::vector<rcc::MachineOperand> PhiOperands;
  PhiOperands.push_back(registerOperand(*Dst32, true));
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_PHI), PhiOperands,
                                                (*Function)->registerInfo()),
                 "G_PHI must require at least one value and predecessor pair");
  PhiOperands.push_back(registerOperand(*Src32A, false));
  PhiOperands.push_back(rcc::MachineOperand::basicBlock(**Block0));
  PhiOperands.push_back(registerOperand(*Src32B, false));
  PhiOperands.push_back(rcc::MachineOperand::basicBlock(**Block1));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_PHI), PhiOperands,
                                               (*Function)->registerInfo())
                     .has_value(),
                 "G_PHI must accept repeated value and predecessor pairs");
  PhiOperands.pop_back();
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_PHI), PhiOperands,
                                                (*Function)->registerInfo()),
                 "G_PHI must reject an incomplete variadic operand pair");

  std::vector<rcc::MachineOperand> IndexedLoad;
  IndexedLoad.push_back(registerOperand(*Dst32, true));
  IndexedLoad.push_back(registerOperand(*NewAddress, true));
  IndexedLoad.push_back(registerOperand(*Address, false));
  IndexedLoad.push_back(registerOperand(*Wide, false));
  IndexedLoad.push_back(rcc::MachineOperand::immediate(1));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_INDEXED_LOAD),
                                               IndexedLoad, (*Function)->registerInfo())
                     .has_value(),
                 "indexed loads must keep address and integer offset type groups independent");

  std::vector<rcc::MachineOperand> UnmergeOperands;
  UnmergeOperands.push_back(registerOperand(*Dst32, true));
  UnmergeOperands.push_back(registerOperand(*Wide, false));
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_UNMERGE_VALUES),
                                                UnmergeOperands, (*Function)->registerInfo()),
                 "G_UNMERGE_VALUES must require at least two definitions");
  UnmergeOperands.insert(UnmergeOperands.begin() + 1, registerOperand(*Src32B, true));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_UNMERGE_VALUES),
                                               UnmergeOperands, (*Function)->registerInfo())
                     .has_value(),
                 "two homogeneous halves must exactly cover a G_UNMERGE_VALUES source");

  std::vector<rcc::MachineOperand> MergeOperands;
  MergeOperands.push_back(registerOperand(*Wide, true));
  MergeOperands.push_back(registerOperand(*Src32A, false));
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_MERGE_VALUES),
                                                MergeOperands, (*Function)->registerInfo()),
                 "G_MERGE_VALUES must require at least two sources");
  MergeOperands.push_back(registerOperand(*Src32B, false));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_MERGE_VALUES),
                                               MergeOperands, (*Function)->registerInfo())
                     .has_value(),
                 "two homogeneous sources must exactly fill a G_MERGE_VALUES result");

  std::vector<rcc::MachineOperand> ConcatOperands;
  ConcatOperands.push_back(registerOperand(Vec4, true));
  ConcatOperands.push_back(registerOperand(Vec2A, false));
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_CONCAT_VECTORS),
                                                ConcatOperands, (*Function)->registerInfo()),
                 "G_CONCAT_VECTORS must require at least two source vectors");
  ConcatOperands.push_back(registerOperand(Vec2B, false));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_CONCAT_VECTORS),
                                               ConcatOperands, (*Function)->registerInfo())
                     .has_value(),
                 "two vector sources must exactly tile a G_CONCAT_VECTORS result");

  std::vector<rcc::MachineOperand> FrameIndexOperands;
  FrameIndexOperands.push_back(registerOperand(PointerVector, true));
  FrameIndexOperands.push_back(rcc::MachineOperand::frameIndex(rcc::FrameIndex::fromValue(0)));
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_FRAME_INDEX),
                                                FrameIndexOperands, (*Function)->registerInfo()),
                 "G_FRAME_INDEX must reject a vector-of-pointers result");

  std::vector<rcc::MachineOperand> PtrAuthOperands;
  PtrAuthOperands.push_back(registerOperand(*NewAddress, true));
  PtrAuthOperands.push_back(registerOperand(*Address, false));
  PtrAuthOperands.push_back(rcc::MachineOperand::immediate(0));
  PtrAuthOperands.push_back(registerOperand(*Wide, false));
  PtrAuthOperands.push_back(rcc::MachineOperand::immediate(7));
  Context.expect(
      Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_PTRAUTH_GLOBAL_VALUE),
                                    PtrAuthOperands, (*Function)->registerInfo())
          .has_value(),
      "G_PTRAUTH_GLOBAL_VALUE must accept pointer and discriminator registers");
  auto Symbol = rcc::MachineOperand::globalSymbol("global");
  if (!Symbol)
    std::terminate();
  PtrAuthOperands[1] = std::move(*Symbol);
  Context.expect(
      !Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_PTRAUTH_GLOBAL_VALUE),
                                     PtrAuthOperands, (*Function)->registerInfo()),
      "G_PTRAUTH_GLOBAL_VALUE must reject a symbol in place of its address register");

  std::vector<rcc::MachineOperand> ExtensionOperands;
  ExtensionOperands.push_back(registerOperand(*Wide, true));
  ExtensionOperands.push_back(registerOperand(*Src32A, false));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ZEXT),
                                               ExtensionOperands, (*Function)->registerInfo())
                     .has_value(),
                 "G_ZEXT must accept a wider destination with matching shape");
  ExtensionOperands[0] = registerOperand(*Dst32, true);
  ExtensionOperands[1] = registerOperand(*Wide, false);
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ZEXT),
                                                ExtensionOperands, (*Function)->registerInfo()),
                 "G_ZEXT must reject a narrower destination");

  auto Predicate = rcc::MachineOperand::integerPredicate(rcc::IntegerPredicate::Equal);
  if (!Predicate)
    std::terminate();
  std::vector<rcc::MachineOperand> CompareOperands;
  CompareOperands.push_back(registerOperand(Cond, true));
  CompareOperands.push_back(std::move(*Predicate));
  CompareOperands.push_back(registerOperand(*Src32A, false));
  CompareOperands.push_back(registerOperand(*Src32B, false));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ICMP),
                                               CompareOperands, (*Function)->registerInfo())
                     .has_value(),
                 "G_ICMP must accept an i1 result and homogeneous integer sources");
  CompareOperands[0] = registerOperand(*Dst32, true);
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ICMP),
                                               CompareOperands, (*Function)->registerInfo())
                     .has_value(),
                 "G_ICMP must accept a legalized integer result container");
  CompareOperands[2] = registerOperand(*Address, false);
  CompareOperands[3] = registerOperand(*NewAddress, false);
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ICMP),
                                               CompareOperands, (*Function)->registerInfo())
                     .has_value(),
                 "G_ICMP must accept homogeneous pointer sources");
  CompareOperands[3] = registerOperand(*Src32B, false);
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_ICMP),
                                                CompareOperands, (*Function)->registerInfo()),
                 "G_ICMP must reject mixed pointer and integer sources");

  std::vector<rcc::MachineOperand> FloatingLoad;
  FloatingLoad.push_back(registerOperand(Float64, true));
  FloatingLoad.push_back(registerOperand(*Address, false));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_FPEXTLOAD),
                                               FloatingLoad, (*Function)->registerInfo())
                     .has_value(),
                 "G_FPEXTLOAD must accept a scalar floating result and pointer address");
  FloatingLoad[0] = registerOperand(*Wide, true);
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_FPEXTLOAD),
                                                FloatingLoad, (*Function)->registerInfo()),
                 "G_FPEXTLOAD must reject an integer result");

  const std::array<std::uint64_t, 1> FloatWords{0};
  auto FloatBits = rcc::FloatingPointBits::create(rcc::FloatSemantics::IEEESingle, FloatWords);
  if (!FloatBits)
    std::terminate();
  std::vector<rcc::MachineOperand> FConstantOperands;
  FConstantOperands.push_back(registerOperand(Float32, true));
  FConstantOperands.push_back(rcc::MachineOperand::floatingImmediate(*FloatBits));
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_FCONSTANT),
                                               FConstantOperands, (*Function)->registerInfo())
                     .has_value(),
                 "G_FCONSTANT payload semantics must match its scalar destination");
  FConstantOperands[0] = registerOperand(Float64, true);
  Context.expect(!Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_FCONSTANT),
                                                FConstantOperands, (*Function)->registerInfo()),
                 "G_FCONSTANT must reject mismatched floating semantics");

  const std::array<std::uint64_t, 1> BFloatWords{0};
  auto BFloatBits = rcc::FloatingPointBits::create(rcc::FloatSemantics::BFloat, BFloatWords);
  if (!BFloatBits)
    std::terminate();
  FConstantOperands[0] = registerOperand(Scalar16, true);
  FConstantOperands[1] = rcc::MachineOperand::floatingImmediate(*BFloatBits);
  Context.expect(Set->validateExplicitOperands(rcc::opcode(rcc::GenericOpcode::G_FCONSTANT),
                                               FConstantOperands, (*Function)->registerInfo())
                     .has_value(),
                 "G_FCONSTANT must accept a bit-width-compatible LLVM scalar LLT");
}

void testInstructionMemorySemantics(TestContext &Context) {
  auto Set = rcc::InstructionSet::create(copyDescriptors());
  auto Function =
      rcc::MachineFunction::create("generic_memory", rcc::MachineFunctionStableID::fromValue(2));
  Context.expect(Set && Function, "generic memory tests require an instruction set and function");
  if (!Set || !Function)
    return;

  const rcc::LLT I1 = *rcc::LLT::makeInteger(1);
  const rcc::LLT I32 = *rcc::LLT::makeInteger(32);
  const rcc::LLT I64 = *rcc::LLT::makeInteger(64);
  const rcc::LLT F64 = *rcc::LLT::makeFloat(rcc::FloatSemantics::IEEEDouble);
  const rcc::LLT P0 = *rcc::LLT::makePointer(0, 64);
  const rcc::LLT P1 = *rcc::LLT::makePointer(1, 64);
  const rcc::Register Dst32 = createVirtualRegister(**Function, I32);
  const rcc::Register Dst64 = createVirtualRegister(**Function, I64);
  const rcc::Register Src32 = createVirtualRegister(**Function, I32);
  const rcc::Register Src64 = createVirtualRegister(**Function, I64);
  const rcc::Register Float64 = createVirtualRegister(**Function, F64);
  const rcc::Register Status = createVirtualRegister(**Function, I1);
  const rcc::Register Address0 = createVirtualRegister(**Function, P0);
  const rcc::Register Address1 = createVirtualRegister(**Function, P1);

  std::vector<rcc::MachineOperand> LoadOperands;
  LoadOperands.push_back(registerOperand(Dst64, true));
  LoadOperands.push_back(registerOperand(Address0, false));
  rcc::MachineInstr &Load =
      createInstruction(**Function, rcc::GenericOpcode::G_LOAD, std::move(LoadOperands));
  addMemOperand(**Function, Load, memoryDescription({rcc::MemoryAccessFlag::Load}, 8));
  Context.expect(Set->validateInstruction(Load, (*Function)->registerInfo()).has_value(),
                 "G_LOAD must accept one matching load MMO");

  std::vector<rcc::MachineOperand> OversizedLoadOperands;
  OversizedLoadOperands.push_back(registerOperand(Dst32, true));
  OversizedLoadOperands.push_back(registerOperand(Address0, false));
  rcc::MachineInstr &OversizedLoad =
      createInstruction(**Function, rcc::GenericOpcode::G_LOAD, std::move(OversizedLoadOperands));
  addMemOperand(**Function, OversizedLoad, memoryDescription({rcc::MemoryAccessFlag::Load}, 8));
  Context.expect(!Set->validateInstruction(OversizedLoad, (*Function)->registerInfo()),
                 "G_LOAD must reject an MMO larger than its result LLT");

  std::vector<rcc::MachineOperand> WrongAddressSpaceOperands;
  WrongAddressSpaceOperands.push_back(registerOperand(Dst64, true));
  WrongAddressSpaceOperands.push_back(registerOperand(Address1, false));
  rcc::MachineInstr &WrongAddressSpace = createInstruction(**Function, rcc::GenericOpcode::G_LOAD,
                                                           std::move(WrongAddressSpaceOperands));
  addMemOperand(**Function, WrongAddressSpace,
                memoryDescription({rcc::MemoryAccessFlag::Load}, 8, 0));
  Context.expect(!Set->validateInstruction(WrongAddressSpace, (*Function)->registerInfo()),
                 "generic memory access must match its pointer and MMO address spaces");

  std::vector<rcc::MachineOperand> ExtLoadOperands;
  ExtLoadOperands.push_back(registerOperand(Dst64, true));
  ExtLoadOperands.push_back(registerOperand(Address0, false));
  rcc::MachineInstr &ExtLoad =
      createInstruction(**Function, rcc::GenericOpcode::G_ZEXTLOAD, std::move(ExtLoadOperands));
  addMemOperand(**Function, ExtLoad, memoryDescription({rcc::MemoryAccessFlag::Load}, 4));
  Context.expect(Set->validateInstruction(ExtLoad, (*Function)->registerInfo()).has_value(),
                 "G_ZEXTLOAD must accept a strictly narrower memory access");

  std::vector<rcc::MachineOperand> NonExtendingLoadOperands;
  NonExtendingLoadOperands.push_back(registerOperand(Dst64, true));
  NonExtendingLoadOperands.push_back(registerOperand(Address0, false));
  rcc::MachineInstr &NonExtendingLoad = createInstruction(
      **Function, rcc::GenericOpcode::G_ZEXTLOAD, std::move(NonExtendingLoadOperands));
  addMemOperand(**Function, NonExtendingLoad, memoryDescription({rcc::MemoryAccessFlag::Load}, 8));
  Context.expect(!Set->validateInstruction(NonExtendingLoad, (*Function)->registerInfo()),
                 "G_ZEXTLOAD must reject a non-narrowing MMO");

  std::vector<rcc::MachineOperand> CompareExchangeOperands;
  CompareExchangeOperands.push_back(registerOperand(Dst64, true));
  CompareExchangeOperands.push_back(registerOperand(Status, true));
  CompareExchangeOperands.push_back(registerOperand(Address0, false));
  CompareExchangeOperands.push_back(registerOperand(Src64, false));
  CompareExchangeOperands.push_back(registerOperand(Src64, false));
  rcc::MachineInstr &CompareExchange =
      createInstruction(**Function, rcc::GenericOpcode::G_ATOMIC_CMPXCHG_WITH_SUCCESS,
                        std::move(CompareExchangeOperands));
  addMemOperand(**Function, CompareExchange,
                memoryDescription({rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store}, 8, 0,
                                  rcc::AtomicOperation::CompareExchange));
  Context.expect(Set->validateInstruction(CompareExchange, (*Function)->registerInfo()).has_value(),
                 "generic compare-exchange must accept a matching atomic MMO");

  std::vector<rcc::MachineOperand> WrongAtomicOperands;
  WrongAtomicOperands.push_back(registerOperand(Dst64, true));
  WrongAtomicOperands.push_back(registerOperand(Address0, false));
  WrongAtomicOperands.push_back(registerOperand(Src64, false));
  rcc::MachineInstr &WrongAtomic = createInstruction(
      **Function, rcc::GenericOpcode::G_ATOMICRMW_ADD, std::move(WrongAtomicOperands));
  addMemOperand(**Function, WrongAtomic,
                memoryDescription({rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store}, 8, 0,
                                  rcc::AtomicOperation::CompareExchange));
  Context.expect(!Set->validateInstruction(WrongAtomic, (*Function)->registerInfo()),
                 "generic atomic RMW must reject a compare-exchange MMO");

  std::vector<rcc::MachineOperand> NarrowAtomicOperands;
  NarrowAtomicOperands.push_back(registerOperand(Dst64, true));
  NarrowAtomicOperands.push_back(registerOperand(Address0, false));
  NarrowAtomicOperands.push_back(registerOperand(Src64, false));
  rcc::MachineInstr &NarrowAtomic = createInstruction(
      **Function, rcc::GenericOpcode::G_ATOMICRMW_ADD, std::move(NarrowAtomicOperands));
  addMemOperand(**Function, NarrowAtomic,
                memoryDescription({rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store}, 1, 0,
                                  rcc::AtomicOperation::ReadModifyWrite));
  Context.expect(Set->validateInstruction(NarrowAtomic, (*Function)->registerInfo()).has_value(),
                 "generic atomic RMW must accept an MMO narrower than its register container");

  std::vector<rcc::MachineOperand> OversizedAtomicOperands;
  OversizedAtomicOperands.push_back(registerOperand(Dst32, true));
  OversizedAtomicOperands.push_back(registerOperand(Address0, false));
  OversizedAtomicOperands.push_back(registerOperand(Src32, false));
  rcc::MachineInstr &OversizedAtomic = createInstruction(
      **Function, rcc::GenericOpcode::G_ATOMICRMW_ADD, std::move(OversizedAtomicOperands));
  addMemOperand(**Function, OversizedAtomic,
                memoryDescription({rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store}, 8, 0,
                                  rcc::AtomicOperation::ReadModifyWrite));
  Context.expect(!Set->validateInstruction(OversizedAtomic, (*Function)->registerInfo()),
                 "generic atomic RMW must reject an MMO wider than its register container");

  std::vector<rcc::MachineOperand> FloatingAtomicOperands;
  FloatingAtomicOperands.push_back(registerOperand(Float64, true));
  FloatingAtomicOperands.push_back(registerOperand(Address0, false));
  FloatingAtomicOperands.push_back(registerOperand(Float64, false));
  rcc::MachineInstr &FloatingAtomic = createInstruction(
      **Function, rcc::GenericOpcode::G_ATOMICRMW_FADD, std::move(FloatingAtomicOperands));
  addMemOperand(**Function, FloatingAtomic,
                memoryDescription({rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store}, 8, 0,
                                  rcc::AtomicOperation::ReadModifyWrite));
  Context.expect(Set->validateInstruction(FloatingAtomic, (*Function)->registerInfo()).has_value(),
                 "floating atomic RMW must accept a scalar floating value and RMW MMO");

  std::vector<rcc::MachineOperand> WrongFloatingAtomicOperands;
  WrongFloatingAtomicOperands.push_back(registerOperand(Dst64, true));
  WrongFloatingAtomicOperands.push_back(registerOperand(Address0, false));
  WrongFloatingAtomicOperands.push_back(registerOperand(Src64, false));
  rcc::MachineInstr &WrongFloatingAtomic = createInstruction(
      **Function, rcc::GenericOpcode::G_ATOMICRMW_FADD, std::move(WrongFloatingAtomicOperands));
  addMemOperand(**Function, WrongFloatingAtomic,
                memoryDescription({rcc::MemoryAccessFlag::Load, rcc::MemoryAccessFlag::Store}, 8, 0,
                                  rcc::AtomicOperation::ReadModifyWrite));
  Context.expect(!Set->validateInstruction(WrongFloatingAtomic, (*Function)->registerInfo()),
                 "floating atomic RMW must reject integer value registers");

  std::vector<rcc::MachineOperand> PrefetchOperands;
  PrefetchOperands.push_back(registerOperand(Address0, false));
  PrefetchOperands.push_back(rcc::MachineOperand::immediate(0));
  PrefetchOperands.push_back(rcc::MachineOperand::immediate(3));
  PrefetchOperands.push_back(rcc::MachineOperand::immediate(1));
  rcc::MachineInstr &Prefetch =
      createInstruction(**Function, rcc::GenericOpcode::G_PREFETCH, std::move(PrefetchOperands));
  Context.expect(!Set->validateInstruction(Prefetch, (*Function)->registerInfo()),
                 "G_PREFETCH must require an MMO");
  addMemOperand(**Function, Prefetch, memoryDescription({rcc::MemoryAccessFlag::Load}, 1));
  Context.expect(Set->validateInstruction(Prefetch, (*Function)->registerInfo()).has_value(),
                 "read-prefetch selector must match a load-only MMO");

  std::vector<rcc::MachineOperand> VarArgOperands;
  VarArgOperands.push_back(registerOperand(Dst64, true));
  VarArgOperands.push_back(registerOperand(Address0, false));
  VarArgOperands.push_back(rcc::MachineOperand::immediate(8));
  rcc::MachineInstr &VarArg =
      createInstruction(**Function, rcc::GenericOpcode::G_VAARG, std::move(VarArgOperands));
  Context.expect(Set->validateInstruction(VarArg, (*Function)->registerInfo()).has_value(),
                 "G_VAARG must accept LLVM's canonical form without MMOs");
  addMemOperand(**Function, VarArg, memoryDescription({rcc::MemoryAccessFlag::Load}, 8));
  addMemOperand(**Function, VarArg, memoryDescription({rcc::MemoryAccessFlag::Store}, 8));
  Context.expect(!Set->validateInstruction(VarArg, (*Function)->registerInfo()),
                 "G_VAARG must reject MMOs before legalization");

  std::vector<rcc::MachineOperand> MemoryTransferOperands;
  MemoryTransferOperands.push_back(registerOperand(Address0, false));
  MemoryTransferOperands.push_back(registerOperand(Address1, false));
  MemoryTransferOperands.push_back(registerOperand(Src64, false));
  MemoryTransferOperands.push_back(rcc::MachineOperand::immediate(0));
  rcc::MachineInstr &MemoryTransfer = createInstruction(**Function, rcc::GenericOpcode::G_MEMCPY,
                                                        std::move(MemoryTransferOperands));
  addMemOperand(**Function, MemoryTransfer,
                memoryDescription({rcc::MemoryAccessFlag::Store}, 8, 0));
  addMemOperand(**Function, MemoryTransfer, memoryDescription({rcc::MemoryAccessFlag::Load}, 8, 1));
  Context.expect(Set->validateInstruction(MemoryTransfer, (*Function)->registerInfo()).has_value(),
                 "G_MEMCPY must accept equal-sized store/load MMOs in canonical order");

  std::vector<rcc::MachineOperand> ReversedTransferOperands;
  ReversedTransferOperands.push_back(registerOperand(Address0, false));
  ReversedTransferOperands.push_back(registerOperand(Address1, false));
  ReversedTransferOperands.push_back(registerOperand(Src64, false));
  ReversedTransferOperands.push_back(rcc::MachineOperand::immediate(0));
  rcc::MachineInstr &ReversedTransfer = createInstruction(**Function, rcc::GenericOpcode::G_MEMCPY,
                                                          std::move(ReversedTransferOperands));
  addMemOperand(**Function, ReversedTransfer,
                memoryDescription({rcc::MemoryAccessFlag::Load}, 8, 0));
  addMemOperand(**Function, ReversedTransfer,
                memoryDescription({rcc::MemoryAccessFlag::Store}, 8, 1));
  Context.expect(!Set->validateInstruction(ReversedTransfer, (*Function)->registerInfo()),
                 "G_MEMCPY must reject reversed MMO access directions");
}

void testSemanticFlags(TestContext &Context) {
  const auto Descriptors = rcc::genericInstructionDescriptors();
  const auto Find = [Descriptors](rcc::GenericOpcode OpcodeValue) {
    const rcc::Opcode Value = rcc::opcode(OpcodeValue);
    const auto Position =
        std::ranges::find_if(Descriptors, [Value](const rcc::InstructionDescriptor &Descriptor) {
          return Descriptor.InstructionOpcode == Value;
        });
    return Position == Descriptors.end() ? nullptr : &*Position;
  };

  const auto *Load = Find(rcc::GenericOpcode::G_LOAD);
  const auto *Atomic = Find(rcc::GenericOpcode::G_ATOMIC_CMPXCHG);
  const auto *VarArg = Find(rcc::GenericOpcode::G_VAARG);
  const auto *Branch = Find(rcc::GenericOpcode::G_BRINDIRECT);
  const auto *Strict = Find(rcc::GenericOpcode::G_STRICT_FMA);
  const auto *InlineAsm = Find(rcc::GenericOpcode::INLINEASM);
  Context.expect(
      Load != nullptr && Load->Flags.has(rcc::InstructionFlag::MayLoad) &&
          Load->RequiresMemoryOperands && Atomic != nullptr &&
          Atomic->Flags.has(rcc::InstructionFlag::MayLoad) &&
          Atomic->Flags.has(rcc::InstructionFlag::MayStore) && Atomic->RequiresMemoryOperands &&
          VarArg != nullptr && VarArg->Flags.has(rcc::InstructionFlag::MayLoad) &&
          VarArg->Flags.has(rcc::InstructionFlag::MayStore) && !VarArg->RequiresMemoryOperands &&
          Branch != nullptr && Branch->Flags.has(rcc::InstructionFlag::Terminator) &&
          Branch->Flags.has(rcc::InstructionFlag::IndirectBranch) &&
          Branch->Flags.has(rcc::InstructionFlag::Barrier) && Strict != nullptr &&
          Strict->Flags.has(rcc::InstructionFlag::HasSideEffects) && InlineAsm != nullptr &&
          InlineAsm->Flags.has(rcc::InstructionFlag::TerminalEmission) &&
          InlineAsm->Owner == rcc::PseudoOwner::AsmPrinter,
      "memory, control-flow, constrained-FP, and terminal flags must be explicit");
}

} // namespace

int main() {
  TestContext Context;
  testCompleteRegistry(Context);
  testCoreOpcodeContracts(Context);
  testShapesAndTypes(Context);
  testInstructionMemorySemantics(Context);
  testSemanticFlags(Context);
  return Context.result();
}
