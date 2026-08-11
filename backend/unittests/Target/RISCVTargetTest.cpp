// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVTarget.h"

#include "rcc/CodeGen/GenericOpcodes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ranges>
#include <string_view>

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

[[nodiscard]] rcc::PhysicalRegister registerID(rcc::riscv::PhysicalRegisterID ID) {
  const auto Register = rcc::riscv::physicalRegister(ID);
  if (!Register)
    std::terminate();
  return *Register;
}

void testRegisterFacts(TestContext &Context) {
  auto Registers = rcc::riscv::createRegisterInfo();
  Context.expect(Registers.has_value(), "RV64GC register description must be valid");
  if (!Registers)
    return;

  const rcc::PhysicalRegister X0 = registerID(rcc::riscv::PhysicalRegisterID::X0);
  const rcc::PhysicalRegister X2 = registerID(rcc::riscv::PhysicalRegisterID::X2);
  const rcc::PhysicalRegister X5 = registerID(rcc::riscv::PhysicalRegisterID::X5);
  const rcc::PhysicalRegister X8 = registerID(rcc::riscv::PhysicalRegisterID::X8);
  const rcc::PhysicalRegister F31D = registerID(rcc::riscv::PhysicalRegisterID::F31_D);
  const rcc::PhysicalRegister F31F = registerID(rcc::riscv::PhysicalRegisterID::F31_F);
  const rcc::PhysicalRegister FFLAGS = registerID(rcc::riscv::PhysicalRegisterID::FFLAGS);
  const rcc::PhysicalRegister FRM = registerID(rcc::riscv::PhysicalRegisterID::FRM);

  Context.expect(
      Registers->physicalRegisters().size() == 98 && Registers->registerUnits().size() == 66 &&
          Registers->registerClasses().size() == 10 && Registers->registerBanks().size() == 3,
      "RV64GC must expose scalar, compressed, restricted, and status register classes");
  Context.expect(Registers->physicalRegister("x0") != nullptr &&
                     Registers->physicalRegister("zero") == nullptr &&
                     Registers->physicalRegisterByAlternativeName("ZERO") != nullptr &&
                     Registers->physicalRegisterByAlternativeName("zero")->Reg == X0 &&
                     Registers->physicalRegisterByAlternativeName("Sp") != nullptr &&
                     Registers->physicalRegisterByAlternativeName("sp")->Reg == X2 &&
                     Registers->physicalRegisterMIRName(X8) == "x8",
                 "canonical MIR names and case-insensitive ABI aliases must use separate lookups");
  Context.expect(
      Registers->isReservedRegister(X0) && Registers->isReservedRegister(X2) &&
          !Registers->isReservedRegister(X5),
      "zero, stack, global, and thread register constraints must be recorded statically");
  Context.expect(Registers->physicalRegister(F31D) != nullptr &&
                     Registers->physicalRegister(F31D)->Encoding == 31 &&
                     Registers->physicalRegister("f31_d") != nullptr &&
                     Registers->physicalRegister("f31_d")->Reg == F31D &&
                     Registers->physicalRegister("f31_f") != nullptr &&
                     Registers->physicalRegister("f31_f")->Reg == F31F &&
                     Registers->physicalRegisterMIRName(F31D) == "f31_d" &&
                     Registers->physicalRegisterMIRName(F31F) == "f31_f" &&
                     Registers->regsOverlap(F31D, F31F) &&
                     Registers->subRegister(F31D, rcc::SubRegIndex::fromValue(1)) == F31F,
                 "FPR32 and FPR64 MIR views must retain encoding, overlap, and subregister facts");
  Context.expect(Registers->physicalRegister("fflags") != nullptr &&
                     Registers->physicalRegister("fflags")->Reg == FFLAGS &&
                     Registers->isReservedRegister(FFLAGS) &&
                     Registers->registerClass("fflags") != nullptr &&
                     !Registers->registerClass("fflags")->Allocatable,
                 "FFLAGS must be a reserved nonallocatable status register");
  Context.expect(Registers->physicalRegister("frm") != nullptr &&
                     Registers->physicalRegister("frm")->Reg == FRM &&
                     Registers->isReservedRegister(FRM) &&
                     Registers->registerClass("frm") != nullptr &&
                     !Registers->registerClass("frm")->Allocatable,
                 "FRM must be a reserved nonallocatable status register");

  const auto *GPR = Registers->registerClass("gpr");
  Context.expect(
      GPR != nullptr && std::ranges::find(GPR->AllocationOrder, X0) == GPR->AllocationOrder.end() &&
          std::ranges::find(GPR->AllocationOrder, X2) == GPR->AllocationOrder.end() &&
          std::ranges::find(GPR->AllocationOrder, X5) != GPR->AllocationOrder.end(),
      "the general-register allocation order must exclude statically reserved registers");
  Context.expect(Registers->registerClass("gpr_nx0") != nullptr &&
                     Registers->registerClass("gpr_nx0_nx2") != nullptr,
                 "compressed instructions must have explicit nonzero register classes");
}

void testABIAndDebugFacts(TestContext &Context) {
  const rcc::PhysicalRegister X1 = registerID(rcc::riscv::PhysicalRegisterID::X1);
  const rcc::PhysicalRegister X10 = registerID(rcc::riscv::PhysicalRegisterID::X10);
  const rcc::PhysicalRegister F8D = registerID(rcc::riscv::PhysicalRegisterID::F8_D);
  const rcc::PhysicalRegister F8F = registerID(rcc::riscv::PhysicalRegisterID::F8_F);
  const rcc::PhysicalRegister F10D = registerID(rcc::riscv::PhysicalRegisterID::F10_D);
  const rcc::PhysicalRegister F10F = registerID(rcc::riscv::PhysicalRegisterID::F10_F);
  const rcc::PhysicalRegister F31D = registerID(rcc::riscv::PhysicalRegisterID::F31_D);
  const rcc::PhysicalRegister F31F = registerID(rcc::riscv::PhysicalRegisterID::F31_F);

  const auto LP64Mask = rcc::riscv::createCallPreservedMask(rcc::riscv::CallingConvention::LP64);
  const auto LP64DMask = rcc::riscv::createCallPreservedMask(rcc::riscv::CallingConvention::LP64D);
  Context.expect(
      LP64DMask != nullptr && LP64DMask->preserves(X1) && LP64DMask->preserves(F8D) &&
          LP64DMask->preserves(F8F) && LP64DMask->clobbers(X10) && LP64DMask->clobbers(F10D) &&
          LP64DMask->clobbers(F10F) && LP64Mask != nullptr && LP64Mask->preserves(X1) &&
          LP64Mask->clobbers(F8D) && LP64Mask->clobbers(F8F) && LP64Mask->clobbers(X10),
      "LP64 and LP64D masks must distinguish integer and floating callee-saved registers");

  auto MIRText = rcc::riscv::createMIRTextInfo();
  Context.expect(MIRText.has_value(), "RV64GC MIR text facts must be valid");
  if (!MIRText)
    return;
  const auto TextLP64Mask = MIRText->registerMask("csr_ilp32_lp64");
  const auto TextLP64DMask = MIRText->registerMask("csr_ilp32d_lp64d");
  Context.expect(MIRText->registerMasks().size() == 2 && TextLP64Mask != nullptr &&
                     LP64Mask != nullptr && *TextLP64Mask == *LP64Mask &&
                     TextLP64DMask != nullptr && LP64DMask != nullptr &&
                     *TextLP64DMask == *LP64DMask,
                 "RV64GC must expose named LP64 and LP64D call-preserved masks");
  Context.expect(MIRText->directOperandFlagMask() == 127 &&
                     MIRText->directOperandFlag("riscv-call") == 1 &&
                     MIRText->directOperandFlag("riscv-hi") == 32 &&
                     MIRText->directOperandFlag("riscv-lo") == 64,
                 "RV64GC MIR target flags must expose canonical call and address spellings");
  Context.expect(MIRText->dwarfRegister(X1) == 1,
                 "the return-address register must retain its DWARF register number");
  Context.expect(MIRText->dwarfRegister(F31D) == 63 && MIRText->dwarfRegister(F31F) == 63 &&
                     MIRText->physicalRegister(63) == F31D,
                 "typed floating views must share one canonical DWARF register mapping");

  const auto Conventions = rcc::riscv::callingConventions();
  const auto *LP64 = rcc::riscv::callingConvention(rcc::riscv::CallingConvention::LP64);
  const auto *LP64D = rcc::riscv::callingConvention(rcc::riscv::CallingConvention::LP64D);
  Context.expect(
      Conventions.size() == 2 && LP64 != nullptr && LP64->Name == "lp64" &&
          LP64->IntegerArgumentRegisters.size() == 8 && LP64->FloatingArgumentRegisters.empty() &&
          LP64->IntegerReturnRegisters.size() == 2 && LP64->FloatingReturnRegisters.empty() &&
          LP64->PreservedMaskName == "CSR_ILP32_LP64" && LP64D != nullptr &&
          LP64D->Name == "lp64d" && LP64D->IntegerArgumentRegisters.size() == 8 &&
          LP64D->FloatingArgumentRegisters.size() == 8 &&
          LP64D->IntegerReturnRegisters.size() == 2 && LP64D->FloatingReturnRegisters.size() == 2 &&
          LP64D->StackAlignmentBytes == 16 && LP64D->StackArgumentAlignmentBytes == 16 &&
          LP64D->SupportsVarArgs && LP64D->SupportsTailCalls &&
          LP64D->PreservedMaskName == "CSR_ILP32D_LP64D",
      "LP64 and LP64D calling-convention tables must expose complete ABI facts");
}

void testInstructionFacts(TestContext &Context) {
  constexpr std::size_t TargetInstructionCount =
      static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::WriteFFLAGS) -
      static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::LUI) + 1U;
  auto Target = rcc::riscv::createTargetDescription();
  Context.expect(Target.has_value(), "RV64GC target description must be structurally valid");
  if (!Target) {
    std::cerr << "target construction error: " << Target.error().message() << '\n';
    return;
  }

  const auto *Addi = Target->instructionDescriptor(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADDI)));
  const auto *Load = Target->instructionDescriptor(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::LD)));
  const auto *Branch = Target->instructionDescriptor(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::BEQ)));
  const auto *Floating = Target->instructionDescriptor(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::FADD_S)));
  const auto *Atomic = Target->instructionDescriptor(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::LR_W)));
  const auto *Compressed = Target->instructionDescriptor(rcc::Opcode::fromValue(
      static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::C_ADDI16SP)));
  const auto *GenericAdd = Target->instructionDescriptor(rcc::opcode(rcc::GenericOpcode::G_ADD));
  const auto *PseudoCall = Target->instructionDescriptor(rcc::Opcode::fromValue(
      static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::PseudoCALL)));
  const auto *Assembly = rcc::riscv::assemblyInstruction(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::ADDI)));
  const auto *FenceAssembly = rcc::riscv::assemblyInstruction(
      rcc::Opcode::fromValue(static_cast<std::uint32_t>(rcc::riscv::InstructionOpcode::FENCE_I)));

  constexpr std::array SymbolicLowOpcodes = {
      rcc::riscv::InstructionOpcode::LB,  rcc::riscv::InstructionOpcode::LH,
      rcc::riscv::InstructionOpcode::LW,  rcc::riscv::InstructionOpcode::LBU,
      rcc::riscv::InstructionOpcode::LHU, rcc::riscv::InstructionOpcode::LWU,
      rcc::riscv::InstructionOpcode::SB,  rcc::riscv::InstructionOpcode::SH,
      rcc::riscv::InstructionOpcode::SW,  rcc::riscv::InstructionOpcode::LD,
      rcc::riscv::InstructionOpcode::SD,  rcc::riscv::InstructionOpcode::FLW,
      rcc::riscv::InstructionOpcode::FSW, rcc::riscv::InstructionOpcode::FLD,
      rcc::riscv::InstructionOpcode::FSD,
  };
  const rcc::OperandKindSet SymbolicLowKinds = {
      rcc::MachineOperand::Kind::Immediate,      rcc::MachineOperand::Kind::ConstantPoolIndex,
      rcc::MachineOperand::Kind::JumpTableIndex, rcc::MachineOperand::Kind::GlobalSymbol,
      rcc::MachineOperand::Kind::ExternalSymbol, rcc::MachineOperand::Kind::BlockAddress,
      rcc::MachineOperand::Kind::MCSymbol,
  };
  Context.expect(
      Load != nullptr && Load->Operands.size() == 3 &&
          std::ranges::all_of(SymbolicLowOpcodes,
                              [&](rcc::riscv::InstructionOpcode Opcode) {
                                const auto *Descriptor = Target->instructionDescriptor(
                                    rcc::Opcode::fromValue(static_cast<std::uint32_t>(Opcode)));
                                return Descriptor != nullptr && Descriptor->Operands.size() == 3 &&
                                       Descriptor->Operands[2].Kinds == SymbolicLowKinds &&
                                       Descriptor->Operands[2].ImmediatePredicate ==
                                           Load->Operands[2].ImmediatePredicate &&
                                       Descriptor->Operands[2].AllowedTargetFlags == 68;
                              }),
      "RV64GC loads and stores must accept SIMM12 low-part symbolic operands");

  Context.expect(
      Target->instructions().descriptors().size() ==
              TargetInstructionCount + rcc::genericInstructionDescriptors().size() &&
          Addi != nullptr && Addi->Name == "ADDI" && Addi->NumExplicitDefs == 1 &&
          Addi->Operands.size() == 3 &&
          Addi->Operands[1].Kinds.contains(rcc::MachineOperand::Kind::FrameIndex) &&
          Addi->Operands.back().ImmediatePredicate && Load != nullptr &&
          Load->Flags.has(rcc::InstructionFlag::MayLoad) && !Load->RequiresMemoryOperands &&
          Branch != nullptr && Branch->Flags.has(rcc::InstructionFlag::Terminator) &&
          Branch->Flags.has(rcc::InstructionFlag::Branch) && Floating != nullptr &&
          Floating->Operands.size() == 4 && Floating->Operands.back().ImmediatePredicate &&
          Floating->AllowExtraImplicitOperands && Atomic != nullptr &&
          Atomic->Flags.has(rcc::InstructionFlag::MayLoad) && !Atomic->RequiresMemoryOperands &&
          Compressed != nullptr && Compressed->ImplicitDefs.size() == 1 &&
          Compressed->ImplicitUses.size() == 1 && GenericAdd != nullptr &&
          GenericAdd->Flags.has(rcc::InstructionFlag::Generic) && PseudoCall != nullptr &&
          PseudoCall->Flags.has(rcc::InstructionFlag::Pseudo) &&
          PseudoCall->Flags.has(rcc::InstructionFlag::Call) &&
          rcc::riscv::assemblyInstruction(PseudoCall->InstructionOpcode) == nullptr &&
          Assembly != nullptr && Assembly->Mnemonic == "addi" &&
          Assembly->Format == rcc::AssemblyFormatID::fromValue(static_cast<std::uint32_t>(
                                  rcc::riscv::AssemblyFormat::RD_RS1_IMM)),
      "generated instruction and assembly descriptors must drive the formal RV64GC target");
  Context.expect(
      Load != nullptr &&
          Target->instructions()
              .validateMemOperands(Load->InstructionOpcode,
                                   std::span<const rcc::MachineMemOperand *const>{})
              .has_value() &&
          !Target->instructions().validateMemOperands(
              rcc::opcode(rcc::GenericOpcode::G_LOAD),
              std::span<const rcc::MachineMemOperand *const>{}),
      "target memory operands must be optional while generic memory operands remain required");

  const auto Schedules = rcc::riscv::schedulingClasses();
  const auto Encodings = rcc::riscv::encodingCategories();
  const auto Formats = rcc::riscv::assemblyFormats();
  Context.expect(Schedules.size() == 15 && Encodings.size() == 10 && Formats.size() == 34 &&
                     rcc::riscv::assemblyInstructions().size() == 195 && FenceAssembly != nullptr &&
                     FenceAssembly->Mnemonic == "fence.i" &&
                     std::ranges::any_of(Schedules,
                                         [](const rcc::riscv::SchedulingClassInfo &Class) {
                                           return Class.Name == "load" && Class.MicroOps == 1 &&
                                                  Class.Latency == 3;
                                         }) &&
                     std::ranges::any_of(Encodings,
                                         [](const rcc::riscv::EncodingCategoryInfo &Category) {
                                           return Category.Name == "i" && Category.SizeInBytes == 4;
                                         }) &&
                     std::ranges::any_of(Formats,
                                         [](const rcc::riscv::AssemblyFormatInfo &Format) {
                                           return Format.Name == "rd-rs1-imm";
                                         }),
                 "generated scheduling, encoding, and assembly tables must preserve target facts");
}

} // namespace

int main() {
  TestContext Context;
  testRegisterFacts(Context);
  testABIAndDebugFacts(Context);
  testInstructionFacts(Context);
  return Context.result();
}
