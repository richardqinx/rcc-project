// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/MIPrinter.h"

#include "rcc/CodeGen/InlineAsm.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMemOperand.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineRegisterInfo.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMIRTextInfo.h"
#include "rcc/CodeGen/TargetRegisterInfo.h"
#include "rcc/MIR/LLVMConstantSyntax.h"
#include "rcc/MIR/MIDebugSyntax.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc::mir {
namespace {

[[nodiscard]] bool isBareNameCharacter(char Character) noexcept {
  return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
         (Character >= '0' && Character <= '9') || Character == '_' || Character == '-' ||
         Character == '.' || Character == '$';
}

[[nodiscard]] std::string quoteName(std::string_view Name) {
  if (!Name.empty() && std::ranges::all_of(Name, isBareNameCharacter))
    return std::string(Name);

  constexpr std::string_view Hex = "0123456789ABCDEF";
  std::string Result;
  Result.reserve(Name.size() + 2U);
  Result.push_back('"');
  for (const char RawCharacter : Name) {
    const auto Character = static_cast<unsigned char>(RawCharacter);
    if (Character >= 0x20U && Character <= 0x7EU && Character != '"' && Character != '\\') {
      Result.push_back(static_cast<char>(Character));
      continue;
    }
    Result.push_back('\\');
    Result.push_back(Hex[Character >> 4U]);
    Result.push_back(Hex[Character & 0xFU]);
  }
  Result.push_back('"');
  return Result;
}

[[nodiscard]] std::string quoteString(std::string_view Value) {
  std::string Result = quoteName(Value);
  if (!Result.empty() && Result.front() == '"')
    return Result;
  Result.insert(Result.begin(), '"');
  Result.push_back('"');
  return Result;
}

[[nodiscard]] std::string hexadecimalWords(std::span<const std::uint64_t> Words,
                                           std::uint32_t BitWidth) {
  constexpr std::string_view Hex = "0123456789ABCDEF";
  const std::size_t DigitCount = (static_cast<std::size_t>(BitWidth) + 3U) / 4U;
  std::string Result(DigitCount, '0');
  for (std::size_t Digit = 0; Digit < DigitCount; ++Digit) {
    const std::size_t BitOffset = Digit * 4U;
    const std::uint64_t Nibble = (Words[BitOffset / 64U] >> (BitOffset % 64U)) & 0xFU;
    Result[DigitCount - Digit - 1U] = Hex[Nibble];
  }
  return Result;
}

[[nodiscard]] std::string_view integerPredicateName(IntegerPredicate Predicate) noexcept {
  switch (Predicate) {
  case IntegerPredicate::Equal:
    return "eq";
  case IntegerPredicate::NotEqual:
    return "ne";
  case IntegerPredicate::SignedGreaterThan:
    return "sgt";
  case IntegerPredicate::SignedGreaterEqual:
    return "sge";
  case IntegerPredicate::SignedLessThan:
    return "slt";
  case IntegerPredicate::SignedLessEqual:
    return "sle";
  case IntegerPredicate::UnsignedGreaterThan:
    return "ugt";
  case IntegerPredicate::UnsignedGreaterEqual:
    return "uge";
  case IntegerPredicate::UnsignedLessThan:
    return "ult";
  case IntegerPredicate::UnsignedLessEqual:
    return "ule";
  case IntegerPredicate::Count:
    return {};
  }
  return {};
}

[[nodiscard]] std::string_view floatingPredicateName(FloatingPredicate Predicate) noexcept {
  switch (Predicate) {
  case FloatingPredicate::AlwaysFalse:
    return "false";
  case FloatingPredicate::OrderedEqual:
    return "oeq";
  case FloatingPredicate::OrderedGreaterThan:
    return "ogt";
  case FloatingPredicate::OrderedGreaterEqual:
    return "oge";
  case FloatingPredicate::OrderedLessThan:
    return "olt";
  case FloatingPredicate::OrderedLessEqual:
    return "ole";
  case FloatingPredicate::OrderedNotEqual:
    return "one";
  case FloatingPredicate::Ordered:
    return "ord";
  case FloatingPredicate::Unordered:
    return "uno";
  case FloatingPredicate::UnorderedEqual:
    return "ueq";
  case FloatingPredicate::UnorderedGreaterThan:
    return "ugt";
  case FloatingPredicate::UnorderedGreaterEqual:
    return "uge";
  case FloatingPredicate::UnorderedLessThan:
    return "ult";
  case FloatingPredicate::UnorderedLessEqual:
    return "ule";
  case FloatingPredicate::UnorderedNotEqual:
    return "une";
  case FloatingPredicate::AlwaysTrue:
    return "true";
  case FloatingPredicate::Count:
    return {};
  }
  return {};
}

[[nodiscard]] std::string_view atomicOrderingName(AtomicOrdering Ordering) noexcept {
  switch (Ordering) {
  case AtomicOrdering::Unordered:
    return "unordered";
  case AtomicOrdering::Monotonic:
    return "monotonic";
  case AtomicOrdering::Acquire:
    return "acquire";
  case AtomicOrdering::Release:
    return "release";
  case AtomicOrdering::AcquireRelease:
    return "acq_rel";
  case AtomicOrdering::SequentiallyConsistent:
    return "seq_cst";
  case AtomicOrdering::NotAtomic:
    return {};
  }
  return {};
}

[[nodiscard]] Expected<std::string> llvmMIRScalarLowLevelType(LLT Type) {
  if (Type.isToken())
    return "s0";
  if (Type.isAnyScalar())
    return "s" + std::to_string(Type.scalarBitWidth());
  if (Type.isInteger())
    return "i" + std::to_string(Type.scalarBitWidth());
  if (Type.isPointer()) {
    const auto AddressSpace = Type.addressSpace();
    if (!AddressSpace) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "pointer low-level type has no address space");
    }
    return "p" + std::to_string(*AddressSpace);
  }
  if (!Type.isFloat()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "invalid scalar low-level type cannot be printed as LLVM MIR");
  }

  const auto Semantics = Type.floatSemantics();
  if (!Semantics) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "floating low-level type has no floating-point semantics");
  }
  switch (*Semantics) {
  case FloatSemantics::IEEEHalf:
    return "f16";
  case FloatSemantics::BFloat:
    return "bf16";
  case FloatSemantics::IEEESingle:
    return "f32";
  case FloatSemantics::IEEEDouble:
    return "f64";
  case FloatSemantics::IEEEQuad:
    return "f128";
  case FloatSemantics::PPCDoubleDouble:
  case FloatSemantics::PPCDoubleDoubleLegacy:
  case FloatSemantics::Float8E5M2:
  case FloatSemantics::Float8E5M2FNUZ:
  case FloatSemantics::Float8E4M3:
  case FloatSemantics::Float8E4M3FN:
  case FloatSemantics::Float8E4M3FNUZ:
  case FloatSemantics::Float8E4M3B11FNUZ:
  case FloatSemantics::Float8E3M4:
  case FloatSemantics::FloatTF32:
  case FloatSemantics::Float8E8M0FNU:
  case FloatSemantics::Float6E3M2FN:
  case FloatSemantics::Float6E2M3FN:
  case FloatSemantics::Float4E2M1FN:
  case FloatSemantics::X87DoubleExtended:
  case FloatSemantics::Count:
    return makeUnexpected(ErrorCode::Unsupported,
                          "fixed LLVM MIR cannot spell this floating low-level type");
  }
  return makeUnexpected(ErrorCode::InternalError, "floating low-level type is unhandled");
}

[[nodiscard]] Expected<std::string> llvmMIRLowLevelType(LLT Type) {
  if (!Type.isValid()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "invalid low-level type cannot be printed as LLVM MIR");
  }
  if (!Type.isVector())
    return llvmMIRScalarLowLevelType(Type);

  const auto ElementType = Type.scalarElementType();
  if (!ElementType) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "vector low-level type has no scalar element type");
  }
  auto Scalar = llvmMIRScalarLowLevelType(*ElementType);
  if (!Scalar)
    return std::unexpected(Scalar.error());

  std::string Result = "<";
  if (Type.isScalableVector())
    Result.append("vscale x ");
  Result.append(std::to_string(Type.minimumElementCount()));
  Result.append(" x ");
  Result.append(*Scalar);
  Result.push_back('>');
  return Result;
}

class BodyPrinter final {
public:
  BodyPrinter(const MachineFunction &Function, const TargetDescription &Target,
              const MIParserContext &Context, MIPrinterOptions Options)
      : Function(Function), Target(Target), Context(Context), Options(Options) {}

  [[nodiscard]] Expected<std::string> run();

private:
  [[nodiscard]] Expected<void> initialize();
  [[nodiscard]] Expected<void> printBlock(const MachineBasicBlock &Block);
  [[nodiscard]] Expected<void> printInstruction(const MachineInstr &Instruction);
  [[nodiscard]] Expected<void> printOperand(const MachineOperand &Operand, bool IsExplicitDef,
                                            bool PrintVirtualType = true);
  [[nodiscard]] Expected<void> printInlineAsmExtraInfo(const MachineOperand &Operand);
  [[nodiscard]] Expected<std::size_t>
  printInlineAsmOperandFlag(const MachineOperand &Operand);
  [[nodiscard]] Expected<void> printRegisterOperand(const MachineOperand &Operand,
                                                    bool IsExplicitDef, bool PrintVirtualType);
  [[nodiscard]] Expected<void> printTargetFlags(std::uint32_t Flags);
  [[nodiscard]] Expected<void> printMetadataReference(MachineMetadataReference Reference);
  [[nodiscard]] Expected<void> printDILocation(const MachineDILocation &Location,
                                               std::size_t FrameIndex);
  [[nodiscard]] Expected<void> printCFIInstruction(CFIInstructionIndex Index);
  [[nodiscard]] Expected<void> printMemoryOperand(const MachineMemOperand &Operand);
  [[nodiscard]] Expected<void> printMemoryBase(const MemoryBase &Base, std::int64_t Offset);

  [[nodiscard]] Expected<std::uint32_t> blockID(const MachineBasicBlock &Block) const;
  [[nodiscard]] Expected<std::uint32_t> virtualRegisterID(Register Reg) const;
  [[nodiscard]] Expected<std::uint32_t> frameID(FrameIndex Index) const;
  [[nodiscard]] Expected<std::uint32_t> constantPoolID(ConstantPoolIndex Index) const;
  [[nodiscard]] Expected<std::uint32_t> jumpTableID(JumpTableIndex Index) const;
  [[nodiscard]] Expected<std::uint32_t> metadataID(MachineMetadataReference Reference) const;
  [[nodiscard]] Expected<void> appendPhysicalRegister(PhysicalRegister Register);
  void appendBlockReference(const MachineBasicBlock &Block);
  void appendOffset(std::int64_t Offset);

  const MachineFunction &Function;
  const TargetDescription &Target;
  const MIParserContext &Context;
  MIPrinterOptions Options;
  std::string Output;
  std::unordered_map<const MachineBasicBlock *, std::uint32_t> BlockIDs;
};

Expected<void> BodyPrinter::initialize() {
  std::uint64_t NextID = 0;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (NextID > std::numeric_limits<std::uint32_t>::max()) {
      return makeUnexpected(ErrorCode::InvalidArgument,
                            "machine function has too many blocks to print");
    }
    BlockIDs.emplace(Block, static_cast<std::uint32_t>(NextID));
    ++NextID;
  }
  return {};
}

Expected<std::uint32_t> BodyPrinter::blockID(const MachineBasicBlock &Block) const {
  const auto Found = BlockIDs.find(&Block);
  if (Found == BlockIDs.end())
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine operand references an unlinked basic block");
  return Found->second;
}

Expected<std::uint32_t> BodyPrinter::virtualRegisterID(Register Reg) const {
  if (!Context.VirtualRegisters.empty()) {
    const auto Found =
        std::ranges::find_if(Context.VirtualRegisters,
                             [Reg](const MIVirtualRegisterSlot &Slot) { return Slot.Reg == Reg; });
    if (Found == Context.VirtualRegisters.end()) {
      return makeUnexpected(ErrorCode::VerificationError, "virtual register has no MIR text slot");
    }
    return Found->TextID;
  }
  const auto VirtualIndex = Reg.virtualIndex();
  if (!VirtualIndex)
    return makeUnexpected(ErrorCode::VerificationError, "virtual register has no numeric index");
  return *VirtualIndex;
}

Expected<std::uint32_t> BodyPrinter::frameID(FrameIndex Index) const {
  const auto InStack = std::ranges::find_if(
      Context.StackObjects, [Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
  if (InStack != Context.StackObjects.end())
    return InStack->TextID;
  const auto InFixed =
      std::ranges::find_if(Context.FixedStackObjects,
                           [Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
  if (InFixed != Context.FixedStackObjects.end())
    return InFixed->TextID;
  return makeUnexpected(ErrorCode::VerificationError, "frame index has no MIR text slot");
}

Expected<std::uint32_t> BodyPrinter::constantPoolID(ConstantPoolIndex Index) const {
  const auto Found =
      std::ranges::find_if(Context.ConstantPoolItems,
                           [Index](const MIConstantPoolSlot &Slot) { return Slot.Index == Index; });
  if (Found == Context.ConstantPoolItems.end()) {
    return makeUnexpected(ErrorCode::VerificationError, "constant-pool index has no MIR text slot");
  }
  return Found->TextID;
}

Expected<std::uint32_t> BodyPrinter::jumpTableID(JumpTableIndex Index) const {
  const auto Found = std::ranges::find_if(
      Context.JumpTables, [Index](const MIJumpTableSlot &Slot) { return Slot.Index == Index; });
  if (Found == Context.JumpTables.end())
    return makeUnexpected(ErrorCode::VerificationError, "jump-table index has no MIR text slot");
  return Found->TextID;
}

Expected<std::uint32_t> BodyPrinter::metadataID(MachineMetadataReference Reference) const {
  const auto Found =
      std::ranges::find_if(Context.Metadata, [Reference](const MIMetadataSlot &Slot) {
        return Slot.Reference == Reference;
      });
  if (Found != Context.Metadata.end())
    return Found->TextID;
  if (Reference.scope() == MachineMetadataScope::MachineFunction)
    return Reference.slot();
  return makeUnexpected(ErrorCode::VerificationError,
                        "embedded metadata reference has no MIR text slot");
}

Expected<void> BodyPrinter::appendPhysicalRegister(PhysicalRegister Register) {
  const auto Name = Target.registers().physicalRegisterMIRName(Register);
  if (!Name)
    return makeUnexpected(ErrorCode::VerificationError, "physical register has no MIR name");
  Output.push_back('$');
  Output.append(*Name);
  return {};
}

void BodyPrinter::appendBlockReference(const MachineBasicBlock &Block) {
  Output.append("%bb.");
  Output.append(std::to_string(BlockIDs.at(&Block)));
}

void BodyPrinter::appendOffset(std::int64_t Offset) {
  if (Offset == 0)
    return;
  if (Offset > 0) {
    Output.append(" + ");
    Output.append(std::to_string(Offset));
    return;
  }
  Output.append(" - ");
  const std::uint64_t Magnitude = Offset == std::numeric_limits<std::int64_t>::min()
                                      ? std::uint64_t{1} << 63U
                                      : static_cast<std::uint64_t>(-Offset);
  Output.append(std::to_string(Magnitude));
}

Expected<void> BodyPrinter::printTargetFlags(std::uint32_t Flags) {
  if (Flags == 0)
    return {};
  auto Names = Target.mirText().operandFlagNames(Flags);
  if (!Names)
    return std::unexpected(Names.error());
  Output.append("target-flags(");
  for (std::size_t Index = 0; Index < Names->size(); ++Index) {
    if (Index != 0)
      Output.append(", ");
    Output.append((*Names)[Index]);
  }
  Output.append(") ");
  return {};
}

Expected<void> BodyPrinter::printMetadataReference(MachineMetadataReference Reference) {
  auto ID = metadataID(Reference);
  if (!ID)
    return std::unexpected(ID.error());
  Output.push_back('!');
  Output.append(std::to_string(*ID));
  return {};
}

Expected<void> BodyPrinter::printDILocation(const MachineDILocation &Location,
                                            std::size_t FrameIndex) {
  const auto Frames = Location.inlineChain();
  if (FrameIndex >= Frames.size()) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline DILocation has an invalid frame index");
  }
  const MachineDILocationFrame &Frame = Frames[FrameIndex];
  Output.append("!DILocation(line: ");
  Output.append(std::to_string(Frame.Line));
  if (Options.Mode == MIPrintMode::Complete || Frame.Column != 0) {
    Output.append(", column: ");
    Output.append(std::to_string(Frame.Column));
  }
  Output.append(", scope: ");
  if (auto Printed = printMetadataReference(Frame.Scope); !Printed)
    return Printed;
  if (FrameIndex + 1U < Frames.size()) {
    Output.append(", inlinedAt: ");
    if (auto Printed = printDILocation(Location, FrameIndex + 1U); !Printed)
      return Printed;
  } else if (Location.terminalInlinedAt()) {
    Output.append(", inlinedAt: ");
    if (auto Printed = printMetadataReference(*Location.terminalInlinedAt()); !Printed) {
      return Printed;
    }
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.IsImplicitCode) {
    Output.append(", isImplicitCode: ");
    Output.append(Frame.IsImplicitCode ? "true" : "false");
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.AtomGroup != 0) {
    Output.append(", atomGroup: ");
    Output.append(std::to_string(Frame.AtomGroup));
  }
  if (Options.Mode == MIPrintMode::Complete || Frame.AtomRank != 0) {
    Output.append(", atomRank: ");
    Output.append(std::to_string(Frame.AtomRank));
  }
  Output.push_back(')');
  return {};
}

Expected<void> BodyPrinter::printRegisterOperand(const MachineOperand &Operand,
                                                 bool IsExplicitDef, bool PrintVirtualType) {
  const RegisterOperandFlags Flags = Operand.registerFlags();
  if (Flags.has(RegisterOperandFlag::Implicit) && Flags.has(RegisterOperandFlag::Define)) {
    Output.append("implicit-def ");
  } else {
    if (!IsExplicitDef && Flags.has(RegisterOperandFlag::Define))
      Output.append("def ");
    if (Flags.has(RegisterOperandFlag::Implicit))
      Output.append("implicit ");
  }
  if (Flags.has(RegisterOperandFlag::Dead))
    Output.append("dead ");
  if (Flags.has(RegisterOperandFlag::Kill))
    Output.append("killed ");
  if (Flags.has(RegisterOperandFlag::Undef))
    Output.append("undef ");
  if (Flags.has(RegisterOperandFlag::InternalRead))
    Output.append("internal ");
  if (Flags.has(RegisterOperandFlag::EarlyClobber))
    Output.append("early-clobber ");
  if (Flags.has(RegisterOperandFlag::Debug))
    Output.append("debug-use ");
  if (Flags.has(RegisterOperandFlag::Renamable))
    Output.append("renamable ");

  const auto RegisterValue = Operand.registerValue();
  if (!RegisterValue) {
    return makeUnexpected(ErrorCode::VerificationError, "register operand has no register value");
  }
  const Register Reg = *RegisterValue;
  if (Reg.isNoRegister()) {
    Output.append("$noreg");
  } else if (Reg.isPhysical()) {
    const auto Physical = Reg.physicalRegister();
    if (!Physical) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "physical register has no physical-register value");
    }
    auto Printed = appendPhysicalRegister(*Physical);
    if (!Printed)
      return Printed;
  } else {
    auto Info = Function.registerInfo().virtualRegisterInfo(Reg);
    if (!Info)
      return std::unexpected(Info.error());
    const VirtualRegisterInfo &RegisterInfo = Info->get();
    Output.push_back('%');
    if (RegisterInfo.Name) {
      Output.append(*RegisterInfo.Name);
    } else {
      auto VirtualIndex = virtualRegisterID(Reg);
      if (!VirtualIndex)
        return std::unexpected(VirtualIndex.error());
      Output.append(std::to_string(*VirtualIndex));
    }
    if (Operand.subReg().value() != 0) {
      const SubRegIndexDesc *SubReg = Target.registers().subRegIndex(Operand.subReg());
      if (SubReg == nullptr)
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register has an unknown subregister");
      Output.push_back('.');
      Output.append(SubReg->Name);
    }

    const bool PrintRegisterConstraint =
        IsExplicitDef || Function.registerInfo().getUniqueVRegDef(Reg) == nullptr;
    if (PrintRegisterConstraint && RegisterInfo.RegClass) {
      const auto Name = Target.registers().registerClassMIRName(*RegisterInfo.RegClass);
      if (!Name)
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register class has no MIR name");
      Output.push_back(':');
      Output.append(*Name);
    } else if (PrintRegisterConstraint && RegisterInfo.Bank) {
      const auto Name = Target.registers().registerBankMIRName(*RegisterInfo.Bank);
      if (!Name)
        return makeUnexpected(ErrorCode::VerificationError,
                              "virtual register bank has no MIR name");
      Output.push_back(':');
      Output.append(*Name);
    } else if (PrintRegisterConstraint && RegisterInfo.Type) {
      Output.append(":_");
    }
    if (PrintVirtualType && RegisterInfo.Type) {
      auto Type = llvmMIRLowLevelType(*RegisterInfo.Type);
      if (!Type)
        return std::unexpected(Type.error());
      Output.push_back('(');
      Output.append(*Type);
      Output.push_back(')');
    }
  }

  if (Operand.tiedOperandIndex() && Operand.isUse()) {
    Output.append("(tied-def ");
    Output.append(std::to_string(*Operand.tiedOperandIndex()));
    Output.push_back(')');
  }
  return {};
}

Expected<void> BodyPrinter::printCFIInstruction(CFIInstructionIndex Index) {
  const MachineCFIInstruction *Instruction = Function.cfiInfo().instruction(Index);
  if (Instruction == nullptr)
    return makeUnexpected(ErrorCode::VerificationError,
                          "CFI operand references an unknown instruction");
  const auto PrintRegister = [this](DwarfRegisterNumber Dwarf) -> Expected<void> {
    const auto Register = Target.mirText().physicalRegister(Dwarf.value());
    if (!Register) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "CFI instruction has no canonical physical register");
    }
    return appendPhysicalRegister(*Register);
  };
  const auto PrintRegisterOffset =
      [this, &PrintRegister](std::string_view Name,
                             const MachineCFIRegisterOffsetRule &Rule) -> Expected<void> {
    Output.append(Name);
    Output.push_back(' ');
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.Offset));
    return {};
  };

  switch (Instruction->operation()) {
  case MachineCFIOperation::SameValue: {
    Output.append("same_value ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  }
  case MachineCFIOperation::RememberState:
    Output.append("remember_state");
    return {};
  case MachineCFIOperation::RestoreState:
    Output.append("restore_state");
    return {};
  case MachineCFIOperation::Offset:
    return PrintRegisterOffset("offset", *Instruction->getIf<MachineCFIRegisterOffsetRule>());
  case MachineCFIOperation::LLVMDefAddressSpaceCFA: {
    const auto &Rule = *Instruction->getIf<MachineCFIAddressSpaceCFARule>();
    Output.append("llvm_def_aspace_cfa ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.Offset));
    Output.append(", ");
    Output.append(std::to_string(Rule.AddressSpace));
    return {};
  }
  case MachineCFIOperation::DefCFARegister:
    Output.append("def_cfa_register ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  case MachineCFIOperation::DefCFAOffset:
    Output.append("def_cfa_offset ");
    Output.append(std::to_string(Instruction->getIf<MachineCFIOffsetRule>()->Offset));
    return {};
  case MachineCFIOperation::DefCFA:
    return PrintRegisterOffset("def_cfa", *Instruction->getIf<MachineCFIRegisterOffsetRule>());
  case MachineCFIOperation::RelativeOffset:
    return PrintRegisterOffset("rel_offset", *Instruction->getIf<MachineCFIRegisterOffsetRule>());
  case MachineCFIOperation::AdjustCFAOffset:
    Output.append("adjust_cfa_offset ");
    Output.append(std::to_string(Instruction->getIf<MachineCFIOffsetRule>()->Offset));
    return {};
  case MachineCFIOperation::Escape: {
    Output.append("escape ");
    constexpr std::string_view Hex = "0123456789ABCDEF";
    const auto &Bytes = Instruction->getIf<MachineCFIEscapeRule>()->Bytes;
    for (std::size_t ByteIndex = 0; ByteIndex < Bytes.size(); ++ByteIndex) {
      if (ByteIndex != 0)
        Output.append(", ");
      Output.append("0x");
      Output.push_back(Hex[Bytes[ByteIndex] >> 4U]);
      Output.push_back(Hex[Bytes[ByteIndex] & 0xFU]);
    }
    return {};
  }
  case MachineCFIOperation::Restore:
    Output.append("restore ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  case MachineCFIOperation::Undefined:
    Output.append("undefined ");
    return PrintRegister(Instruction->getIf<MachineCFIRegisterRule>()->Register);
  case MachineCFIOperation::Register: {
    const auto &Rule = *Instruction->getIf<MachineCFIRegisterRegisterRule>();
    Output.append("register ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    return PrintRegister(Rule.SourceRegister);
  }
  case MachineCFIOperation::WindowSave:
    Output.append("window_save");
    return {};
  case MachineCFIOperation::NegateReturnAddressState:
    Output.append("negate_ra_sign_state");
    return {};
  case MachineCFIOperation::NegateReturnAddressStateWithPC:
    Output.append("negate_ra_sign_state_with_pc");
    return {};
  case MachineCFIOperation::GNUArgsSize:
  case MachineCFIOperation::Label:
  case MachineCFIOperation::ValueOffset:
    return makeUnexpected(
        ErrorCode::Unsupported,
        "CFI operation has no textual representation in the fixed LLVM MIR baseline");
  case MachineCFIOperation::LLVMRegisterPair: {
    const auto &Rule = *Instruction->getIf<MachineCFIRegisterPairRule>();
    Output.append("llvm_register_pair ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.FirstRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.FirstSizeInBits));
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.SecondRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.SecondSizeInBits));
    return {};
  }
  case MachineCFIOperation::LLVMVectorRegisters: {
    const auto &Rule = *Instruction->getIf<MachineCFIVectorRegistersRule>();
    Output.append("llvm_vector_registers ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    for (const MachineCFIVectorRegisterLane &Lane : Rule.VectorRegisters) {
      Output.append(", ");
      if (auto Printed = PrintRegister(Lane.Register); !Printed)
        return Printed;
      Output.append(", ");
      Output.append(std::to_string(Lane.Lane));
      Output.append(", ");
      Output.append(std::to_string(Lane.SizeInBits));
    }
    return {};
  }
  case MachineCFIOperation::LLVMVectorOffset: {
    const auto &Rule = *Instruction->getIf<MachineCFIVectorOffsetRule>();
    Output.append("llvm_vector_offset ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.RegisterSizeInBits));
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.MaskRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.MaskRegisterSizeInBits));
    Output.append(", ");
    Output.append(std::to_string(Rule.Offset));
    return {};
  }
  case MachineCFIOperation::LLVMVectorRegisterMask: {
    const auto &Rule = *Instruction->getIf<MachineCFIVectorRegisterMaskRule>();
    Output.append("llvm_vector_register_mask ");
    if (auto Printed = PrintRegister(Rule.Register); !Printed)
      return Printed;
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.SpillRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.SpillRegisterLaneSizeInBits));
    Output.append(", ");
    if (auto Printed = PrintRegister(Rule.MaskRegister); !Printed)
      return Printed;
    Output.append(", ");
    Output.append(std::to_string(Rule.MaskRegisterSizeInBits));
    return {};
  }
  case MachineCFIOperation::Count:
    return makeUnexpected(ErrorCode::VerificationError, "CFI instruction has an invalid operation");
  }
  return makeUnexpected(ErrorCode::InternalError, "CFI printer did not handle an operation");
}

Expected<void> BodyPrinter::printOperand(const MachineOperand &Operand, bool IsExplicitDef,
                                         bool PrintVirtualType) {
  if (Operand.isRegister())
    return printRegisterOperand(Operand, IsExplicitDef, PrintVirtualType);
  if (auto Flags = printTargetFlags(Operand.targetFlags()); !Flags)
    return Flags;

  switch (Operand.kind()) {
  case MachineOperand::Kind::Register:
    return makeUnexpected(ErrorCode::InternalError,
                          "register operand reached the non-register printer");
  case MachineOperand::Kind::Immediate:
    Output.append(std::to_string(*Operand.getIf<std::int64_t>()));
    return {};
  case MachineOperand::Kind::IntegerImmediate: {
    const IntegerImmediate &Value = Operand.getIf<IntegerImmediateOperand>()->Value;
    Output.push_back('i');
    Output.append(std::to_string(Value.bitWidth()));
    Output.push_back(' ');
    if (Value.bitWidth() == 1U)
      Output.append(Value.isZero() ? "false" : "true");
    else
      Output.append(Value.toSignedDecimal());
    return {};
  }
  case MachineOperand::Kind::FloatingImmediate: {
    const FloatingPointBits &Bits = Operand.getIf<FloatingImmediateOperand>()->Bits;
    auto Constant = GenericConstantData::floating(Bits);
    if (!Constant)
      return std::unexpected(Constant.error());
    auto Text = printLLVMConstant(*Constant);
    if (!Text)
      return std::unexpected(Text.error());
    Output.append(*Text);
    return {};
  }
  case MachineOperand::Kind::BasicBlock: {
    const MachineBasicBlock *Block = Operand.getIf<BasicBlockOperand>()->Block;
    if (Block == nullptr || !BlockIDs.contains(Block)) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "basic-block operand references an unlinked block");
    }
    appendBlockReference(*Block);
    return {};
  }
  case MachineOperand::Kind::FrameIndex: {
    const auto &Frame = *Operand.getIf<FrameIndexOperand>();
    auto ID = frameID(Frame.Index);
    if (!ID)
      return std::unexpected(ID.error());
    const auto Stack = std::ranges::find_if(
        Context.StackObjects,
        [Index = Frame.Index](const MIFrameObjectSlot &Slot) { return Slot.Index == Index; });
    if (Stack != Context.StackObjects.end()) {
      Output.append("%stack.");
      Output.append(std::to_string(*ID));
      if (Stack->Name) {
        Output.push_back('.');
        Output.append(quoteName(*Stack->Name));
      }
    } else {
      Output.append("%fixed-stack.");
      Output.append(std::to_string(*ID));
    }
    appendOffset(Frame.Offset);
    return {};
  }
  case MachineOperand::Kind::ConstantPoolIndex: {
    const auto &Constant = *Operand.getIf<ConstantPoolOperand>();
    auto ID = constantPoolID(Constant.Index);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%const.");
    Output.append(std::to_string(*ID));
    appendOffset(Constant.Offset);
    return {};
  }
  case MachineOperand::Kind::JumpTableIndex: {
    auto ID = jumpTableID(Operand.getIf<JumpTableOperand>()->Index);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%jump-table.");
    Output.append(std::to_string(*ID));
    return {};
  }
  case MachineOperand::Kind::GlobalSymbol: {
    const auto &Global = *Operand.getIf<GlobalSymbolOperand>();
    Output.push_back('@');
    Output.append(quoteName(Global.Name));
    appendOffset(Global.Offset);
    return {};
  }
  case MachineOperand::Kind::ExternalSymbol: {
    const auto &External = *Operand.getIf<ExternalSymbolOperand>();
    Output.push_back('&');
    Output.append(quoteName(External.Name));
    appendOffset(External.Offset);
    return {};
  }
  case MachineOperand::Kind::BlockAddress: {
    const auto &Address = *Operand.getIf<BlockAddressOperand>();
    Output.append("blockaddress(@");
    Output.append(quoteName(Address.FunctionName));
    Output.append(", %ir-block.");
    Output.append(quoteName(Address.BlockName));
    Output.push_back(')');
    appendOffset(Address.Offset);
    return {};
  }
  case MachineOperand::Kind::RegisterMask: {
    const RegisterMask &Mask = *Operand.getIf<RegisterMaskOperand>()->Mask;
    if (const auto Name = Target.mirText().registerMaskName(Mask)) {
      Output.append(*Name);
      return {};
    }
    Output.append("CustomRegMask(");
    bool First = true;
    for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
      if (!Mask.preserves(Register.Reg))
        continue;
      if (!First)
        Output.append(", ");
      First = false;
      if (auto Printed = appendPhysicalRegister(Register.Reg); !Printed)
        return Printed;
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::LiveOutRegisterMask: {
    const RegisterMask &Mask = *Operand.getIf<LiveOutRegisterMaskOperand>()->Mask;
    Output.append("liveout(");
    bool First = true;
    for (const PhysicalRegisterDesc &Register : Target.registers().physicalRegisters()) {
      if (!Mask.preserves(Register.Reg))
        continue;
      if (!First)
        Output.append(", ");
      First = false;
      if (auto Printed = appendPhysicalRegister(Register.Reg); !Printed)
        return Printed;
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::Metadata: {
    const MachineMetadataValue &Value = Operand.getIf<MetadataOperand>()->Value;
    if (const auto *Reference = Value.reference())
      return printMetadataReference(*Reference);
    const MachineDIExpression *Expression = Value.expression();
    if (Expression == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "metadata operand has no printable value");
    }
    Output.append(formatDIExpression(Expression->elements()));
    return {};
  }
  case MachineOperand::Kind::TargetIndex: {
    const auto &TargetIndexValue = *Operand.getIf<TargetIndexOperand>();
    const auto Name = Target.mirText().targetIndexName(TargetIndexValue.Index);
    if (!Name)
      return makeUnexpected(ErrorCode::VerificationError, "target index has no MIR name");
    Output.append("target-index(");
    Output.append(*Name);
    Output.push_back(')');
    appendOffset(TargetIndexValue.Offset);
    return {};
  }
  case MachineOperand::Kind::MCSymbol: {
    const auto &Symbol = *Operand.getIf<MCSymbolOperand>();
    Output.append("<mcsymbol ");
    Output.append(quoteName(Symbol.Name));
    Output.push_back('>');
    appendOffset(Symbol.Offset);
    return {};
  }
  case MachineOperand::Kind::CFIIndex:
    return printCFIInstruction(Operand.getIf<CFIIndexOperand>()->Index);
  case MachineOperand::Kind::Intrinsic: {
    const auto Name = Target.mirText().intrinsicName(Operand.getIf<IntrinsicOperand>()->ID);
    if (!Name)
      return makeUnexpected(ErrorCode::VerificationError, "intrinsic has no MIR name");
    Output.append("intrinsic(@");
    Output.append(*Name);
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::Predicate: {
    const PredicateOperand &Predicate = *Operand.getIf<PredicateOperand>();
    if (const auto *Integer = Predicate.integerPredicate()) {
      Output.append("intpred(");
      Output.append(integerPredicateName(*Integer));
    } else {
      Output.append("floatpred(");
      Output.append(floatingPredicateName(*Predicate.floatingPredicate()));
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::ShuffleMask: {
    Output.append("shufflemask(");
    const auto &Elements = Operand.getIf<ShuffleMaskOperand>()->Elements;
    for (std::size_t Index = 0; Index < Elements.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      if (Elements[Index] == ShuffleMaskOperand::Undef)
        Output.append("undef");
      else
        Output.append(std::to_string(Elements[Index]));
    }
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::DebugInstrRef: {
    const auto &Reference = *Operand.getIf<DebugInstrRefOperand>();
    Output.append("dbg-instr-ref(");
    Output.append(std::to_string(Reference.InstructionNumber));
    Output.append(", ");
    Output.append(std::to_string(Reference.OperandNumber));
    Output.push_back(')');
    return {};
  }
  case MachineOperand::Kind::LaneMask:
    Output.append("lanemask(0x");
    Output.append(
        hexadecimalWords(std::array{Operand.getIf<LaneMaskOperand>()->Mask.rawValue()}, 64));
    Output.push_back(')');
    return {};
  case MachineOperand::Kind::Count:
    return makeUnexpected(ErrorCode::VerificationError, "machine operand has an invalid kind");
  }
  return makeUnexpected(ErrorCode::InternalError, "machine operand printer did not handle a kind");
}

Expected<void> BodyPrinter::printInlineAsmExtraInfo(const MachineOperand &Operand) {
  const auto *Value = Operand.getIf<std::int64_t>();
  if (Value == nullptr || Operand.targetFlags() != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly extra info must be an unflagged immediate");
  }
  const InlineAsmExtraInfo ExtraInfo(static_cast<std::uint32_t>(*Value));
  const std::vector<std::string_view> Names = ExtraInfo.names();
  for (std::size_t Index = 0; Index < Names.size(); ++Index) {
    if (Index != 0)
      Output.push_back(' ');
    Output.append(Names[Index]);
  }
  return {};
}

Expected<std::size_t> BodyPrinter::printInlineAsmOperandFlag(const MachineOperand &Operand) {
  const auto *Value = Operand.getIf<std::int64_t>();
  if (Value == nullptr || *Value < 0 ||
      static_cast<std::uint64_t>(*Value) > std::numeric_limits<std::uint32_t>::max() ||
      Operand.targetFlags() != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly operand flag must be an unflagged 32-bit immediate");
  }

  const InlineAsmOperandFlag Flag(static_cast<std::uint32_t>(*Value));
  const std::string_view KindName = InlineAsmOperandFlag::kindName(Flag.kind());
  if (KindName.empty() || Flag.operandCount() == 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly operand flag has an invalid kind or operand count");
  }
  Output.append(KindName);

  if (const auto MatchingOperand = Flag.matchingOperand()) {
    Output.append(" tiedto:$");
    Output.append(std::to_string(*MatchingOperand));
    return Flag.operandCount();
  }
  if (Flag.kind() == InlineAsmOperandKind::Memory ||
      Flag.kind() == InlineAsmOperandKind::Function) {
    const auto Constraint = InlineAsmOperandFlag::memoryConstraintName(Flag.memoryConstraint());
    if (Constraint) {
      Output.push_back(':');
      Output.append(*Constraint);
    }
    return Flag.operandCount();
  }
  if (Flag.isRegisterKind()) {
    if (const auto RegisterClass = Flag.registerClass()) {
      const auto Name =
          Target.registers().registerClassMIRName(RegisterClassID::fromValue(*RegisterClass));
      if (!Name) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "inline assembly operand flag references an unknown register class");
      }
      Output.push_back(':');
      Output.append(*Name);
    }
  }
  return Flag.operandCount();
}

Expected<void> BodyPrinter::printMemoryBase(const MemoryBase &Base, std::int64_t Offset) {
  switch (Base.kind()) {
  case MemoryBase::Kind::Unknown:
    Output.append("unknown-address");
    break;
  case MemoryBase::Kind::FrameIndex: {
    const auto Frame = Base.frameIndex();
    if (!Frame) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "frame-index memory base has no frame index");
    }
    const FrameIndex Index = *Frame;
    auto ID = frameID(Index);
    if (!ID)
      return std::unexpected(ID.error());
    const auto Stack =
        std::ranges::find_if(Context.StackObjects, [Index](const MIFrameObjectSlot &Slot) {
          return Slot.Index == Index;
        });
    if (Stack != Context.StackObjects.end()) {
      Output.append("%stack.");
      Output.append(std::to_string(*ID));
      if (Stack->Name) {
        Output.push_back('.');
        Output.append(quoteName(*Stack->Name));
      }
    } else {
      Output.append("%fixed-stack.");
      Output.append(std::to_string(*ID));
    }
    break;
  }
  case MemoryBase::Kind::ConstantPool: {
    const auto Constant = Base.constantPoolIndex();
    if (!Constant) {
      return makeUnexpected(ErrorCode::VerificationError, "constant-pool memory base has no index");
    }
    auto ID = constantPoolID(*Constant);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%const.");
    Output.append(std::to_string(*ID));
    break;
  }
  case MemoryBase::Kind::JumpTable: {
    const auto JumpTable = Base.jumpTableIndex();
    if (!JumpTable) {
      return makeUnexpected(ErrorCode::VerificationError, "jump-table memory base has no index");
    }
    auto ID = jumpTableID(*JumpTable);
    if (!ID)
      return std::unexpected(ID.error());
    Output.append("%jump-table.");
    Output.append(std::to_string(*ID));
    break;
  }
  case MemoryBase::Kind::GlobalSymbol: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError, "global memory base has no symbol name");
    }
    Output.push_back('@');
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::ExternalSymbol: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "external memory base has no symbol name");
    }
    Output.push_back('&');
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::IRValue: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError, "IR-value memory base has no value name");
    }
    Output.append("%ir.");
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::IRConstant: {
    const GenericConstantData *Constant = Base.constantValue();
    if (Constant == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "IR-constant memory base has no constant value");
    }
    auto Printed = printLLVMConstant(*Constant);
    if (!Printed)
      return std::unexpected(Printed.error());
    Output.push_back('`');
    Output.append(*Printed);
    Output.push_back('`');
    break;
  }
  case MemoryBase::Kind::Stack:
    Output.append("stack");
    break;
  case MemoryBase::Kind::GOT:
    Output.append("got");
    break;
  case MemoryBase::Kind::ConstantPoolPseudo:
    Output.append("constant-pool");
    break;
  case MemoryBase::Kind::JumpTablePseudo:
    Output.append("jump-table");
    break;
  case MemoryBase::Kind::CallEntryGlobal: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "global call-entry memory base has no symbol name");
    }
    Output.append("call-entry @");
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::CallEntryExternal: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "external call-entry memory base has no symbol name");
    }
    Output.append("call-entry &");
    Output.append(quoteName(*Name));
    break;
  }
  case MemoryBase::Kind::Custom: {
    const auto &Name = Base.symbolName();
    if (!Name) {
      return makeUnexpected(ErrorCode::VerificationError, "custom memory base has no name");
    }
    Output.append("custom ");
    Output.append(quoteName(*Name));
    break;
  }
  }
  appendOffset(Offset);
  return {};
}

Expected<void> BodyPrinter::printMemoryOperand(const MachineMemOperand &Operand) {
  const MachineMemOperandDesc &Description = Operand.description();
  Output.push_back('(');
  if (Description.Accesses.has(MemoryAccessFlag::Volatile))
    Output.append("volatile ");
  if (Description.Accesses.has(MemoryAccessFlag::NonTemporal))
    Output.append("non-temporal ");
  if (Description.Accesses.has(MemoryAccessFlag::Dereferenceable))
    Output.append("dereferenceable ");
  if (Description.Accesses.has(MemoryAccessFlag::Invariant))
    Output.append("invariant ");
  if (Description.TargetFlags != 0) {
    auto Names = Target.mirText().machineMemOperandFlagNames(Description.TargetFlags);
    if (!Names)
      return std::unexpected(Names.error());
    for (std::string_view Name : *Names) {
      Output.append(quoteName(Name));
      Output.push_back(' ');
    }
  }

  if (Operand.isLoad())
    Output.append("load");
  else
    Output.append("store");
  if (Operand.isLoad() && Operand.isStore())
    Output.append(" store");

  if (Description.SyncScope.value() != 0) {
    const auto Scope =
        std::ranges::find_if(Context.SyncScopes, [&Description](const MISyncScopeSlot &Slot) {
          return Slot.ID == Description.SyncScope;
        });
    if (Scope == Context.SyncScopes.end()) {
      return makeUnexpected(ErrorCode::VerificationError, "synchronization scope has no MIR name");
    }
    Output.append(" syncscope(");
    Output.append(quoteString(Scope->Name));
    Output.push_back(')');
  }
  if (Description.SuccessOrdering != AtomicOrdering::NotAtomic) {
    Output.push_back(' ');
    Output.append(atomicOrderingName(Description.SuccessOrdering));
  }
  if (Description.FailureOrdering != AtomicOrdering::NotAtomic) {
    Output.push_back(' ');
    Output.append(atomicOrderingName(Description.FailureOrdering));
  }

  Output.push_back(' ');
  if (Description.MemoryType) {
    auto Type = llvmMIRLowLevelType(*Description.MemoryType);
    if (!Type)
      return std::unexpected(Type.error());
    Output.push_back('(');
    Output.append(*Type);
    Output.push_back(')');
  } else {
    switch (Description.Size.kind()) {
    case MemorySize::Kind::Unknown:
      Output.append("unknown-size");
      break;
    case MemorySize::Kind::Fixed:
      Output.append(std::to_string(Description.Size.minimumBytes()));
      break;
    case MemorySize::Kind::Scalable:
      Output.append("(<vscale x ");
      Output.append(std::to_string(Description.Size.minimumBytes()));
      Output.append(" x s8>)");
      break;
    }
  }

  const std::string_view Preposition =
      Operand.isLoad() && Operand.isStore() ? " on " : (Operand.isLoad() ? " from " : " into ");
  if (Description.Base.kind() != MemoryBase::Kind::Unknown || Description.Offset != 0) {
    Output.append(Preposition);
    if (auto Printed = printMemoryBase(Description.Base, Description.Offset); !Printed) {
      return Printed;
    }
  }

  if (!Description.Size.isKnown() ||
      (Description.Size.minimumBytes() != 0 &&
       Description.Align.bytes() != Description.Size.minimumBytes())) {
    Output.append(", align ");
    Output.append(std::to_string(Description.Align.bytes()));
  }
  if (Description.BaseAlign && *Description.BaseAlign != Description.Align) {
    Output.append(", basealign ");
    Output.append(std::to_string(Description.BaseAlign->bytes()));
  }
  if (Description.AddressSpace != 0) {
    Output.append(", addrspace ");
    Output.append(std::to_string(Description.AddressSpace));
  }

  const auto PrintMetadata =
      [this](std::string_view Name,
             const std::optional<MachineMetadataReference> &Reference) -> Expected<void> {
    if (!Reference)
      return {};
    Output.append(", ");
    Output.append(Name);
    Output.push_back(' ');
    return printMetadataReference(*Reference);
  };
  if (auto Printed = PrintMetadata("!tbaa", Description.TBAA); !Printed)
    return Printed;
  if (auto Printed = PrintMetadata("!alias.scope", Description.AliasScope); !Printed) {
    return Printed;
  }
  if (auto Printed = PrintMetadata("!noalias", Description.NoAlias); !Printed)
    return Printed;
  if (auto Printed = PrintMetadata("!noalias.addrspace", Description.NoAliasAddressSpace);
      !Printed) {
    return Printed;
  }
  if (auto Printed = PrintMetadata("!range", Description.RangeMetadata); !Printed) {
    return Printed;
  }
  Output.push_back(')');
  return {};
}

Expected<void> BodyPrinter::printInstruction(const MachineInstr &Instruction) {
  const InstructionDescriptor *Descriptor = Target.instructions().descriptor(Instruction.opcode());
  if (Descriptor == nullptr) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine instruction has no target descriptor");
  }
  std::size_t ExplicitDefs = 0;
  while (ExplicitDefs < Instruction.operandCount()) {
    const MachineOperand *Operand = Instruction.operandAt(ExplicitDefs);
    if (Operand == nullptr || !Operand->isRegister() || Operand->isImplicit() ||
        !Operand->isDef() || Operand->isUse())
      break;
    ++ExplicitDefs;
  }

  Output.append("  ");
  for (std::size_t Index = 0; Index < ExplicitDefs; ++Index) {
    if (Index != 0)
      Output.append(", ");
    if (auto Printed = printOperand(*Instruction.operandAt(Index), true); !Printed) {
      return Printed;
    }
  }
  if (ExplicitDefs != 0)
    Output.append(" = ");

  const MachineInstrFlags Flags = Instruction.flags();
  constexpr std::array FlagSpellings{
      std::pair{MachineInstrFlag::FrameSetup, std::string_view{"frame-setup"}},
      std::pair{MachineInstrFlag::FrameDestroy, std::string_view{"frame-destroy"}},
      std::pair{MachineInstrFlag::FastMathNoNaNs, std::string_view{"nnan"}},
      std::pair{MachineInstrFlag::FastMathNoInfs, std::string_view{"ninf"}},
      std::pair{MachineInstrFlag::FastMathNoSignedZeros, std::string_view{"nsz"}},
      std::pair{MachineInstrFlag::FastMathAllowReciprocal, std::string_view{"arcp"}},
      std::pair{MachineInstrFlag::FastMathAllowContract, std::string_view{"contract"}},
      std::pair{MachineInstrFlag::FastMathApproxFunc, std::string_view{"afn"}},
      std::pair{MachineInstrFlag::FastMathAllowReassoc, std::string_view{"reassoc"}},
      std::pair{MachineInstrFlag::NoUnsignedWrap, std::string_view{"nuw"}},
      std::pair{MachineInstrFlag::NoSignedWrap, std::string_view{"nsw"}},
      std::pair{MachineInstrFlag::NoUSWrap, std::string_view{"nusw"}},
      std::pair{MachineInstrFlag::Exact, std::string_view{"exact"}},
      std::pair{MachineInstrFlag::NoFPExcept, std::string_view{"nofpexcept"}},
      std::pair{MachineInstrFlag::Unpredictable, std::string_view{"unpredictable"}},
      std::pair{MachineInstrFlag::NoConvergent, std::string_view{"noconvergent"}},
      std::pair{MachineInstrFlag::NonNeg, std::string_view{"nneg"}},
      std::pair{MachineInstrFlag::Disjoint, std::string_view{"disjoint"}},
      std::pair{MachineInstrFlag::SameSign, std::string_view{"samesign"}},
      std::pair{MachineInstrFlag::InBounds, std::string_view{"inbounds"}},
  };
  for (const auto &[Flag, Spelling] : FlagSpellings) {
    if (Flags.has(Flag)) {
      Output.append(Spelling);
      Output.push_back(' ');
    }
  }
  Output.append(Descriptor->Name);

  bool HasPrintedTail = false;
  const bool IsInlineAsm = Descriptor->Name == "INLINEASM" || Descriptor->Name == "INLINEASM_BR";
  std::size_t InlineAsmPayloadRemaining = 0;
  for (std::size_t Index = ExplicitDefs; Index < Instruction.operandCount(); ++Index) {
    Output.append(HasPrintedTail ? ", " : " ");
    HasPrintedTail = true;
    const MachineOperand &Operand = *Instruction.operandAt(Index);
    if (IsInlineAsm && Index == InlineAsmExtraInfoOperandIndex) {
      auto Printed = printInlineAsmExtraInfo(Operand);
      if (!Printed)
        return Printed;
      continue;
    }
    if (IsInlineAsm && Index >= InlineAsmFirstConstraintOperandIndex &&
        InlineAsmPayloadRemaining == 0) {
      auto OperandCount = printInlineAsmOperandFlag(Operand);
      if (!OperandCount)
        return std::unexpected(OperandCount.error());
      InlineAsmPayloadRemaining = *OperandCount;
      continue;
    }
    bool PrintVirtualType = true;
    if (Descriptor->Flags.has(InstructionFlag::Generic) &&
        !Descriptor->Flags.has(InstructionFlag::Phi) &&
        Index < Descriptor->Operands.size()) {
      const std::optional<OperandTypeConstraint> &UseConstraint =
          Descriptor->Operands[Index].TypeConstraint;
      if (UseConstraint) {
        for (std::size_t DefIndex = 0; DefIndex < ExplicitDefs; ++DefIndex) {
          if (DefIndex >= Descriptor->Operands.size())
            break;
          const std::optional<OperandTypeConstraint> &DefConstraint =
              Descriptor->Operands[DefIndex].TypeConstraint;
          if (DefConstraint && DefConstraint->EqualityGroup == UseConstraint->EqualityGroup) {
            PrintVirtualType = false;
            break;
          }
        }
      } else if (const Register *UseRegister = Operand.getIf<Register>();
                 UseRegister != nullptr && UseRegister->isVirtual()) {
        const auto UseInfo = Function.registerInfo().virtualRegisterInfo(*UseRegister);
        for (std::size_t DefIndex = 0; DefIndex < ExplicitDefs && UseInfo &&
                                       UseInfo->get().Type;
             ++DefIndex) {
          if (DefIndex >= Descriptor->Operands.size() ||
              Descriptor->Operands[DefIndex].TypeConstraint)
            continue;
          const MachineOperand *DefOperand = Instruction.operandAt(DefIndex);
          const Register *DefRegister =
              DefOperand == nullptr ? nullptr : DefOperand->getIf<Register>();
          if (DefRegister == nullptr || !DefRegister->isVirtual())
            continue;
          const auto DefInfo = Function.registerInfo().virtualRegisterInfo(*DefRegister);
          if (DefInfo && DefInfo->get().Type == UseInfo->get().Type) {
            PrintVirtualType = false;
            break;
          }
        }
      }
    }
    auto Printed = printOperand(Operand, false, PrintVirtualType);
    if (!Printed)
      return Printed;
    if (IsInlineAsm && InlineAsmPayloadRemaining != 0)
      --InlineAsmPayloadRemaining;
  }
  if (InlineAsmPayloadRemaining != 0)
    return makeUnexpected(ErrorCode::VerificationError,
                          "inline assembly operand flag has missing payload operands");

  const auto BeginAttachment = [this, &HasPrintedTail](std::string_view Name) {
    Output.append(HasPrintedTail ? ", " : " ");
    HasPrintedTail = true;
    Output.append(Name);
    Output.push_back(' ');
  };
  if (Instruction.preInstrSymbol()) {
    BeginAttachment("pre-instr-symbol");
    Output.append("<mcsymbol ");
    Output.append(quoteName(*Instruction.preInstrSymbol()));
    Output.push_back('>');
  }
  if (Instruction.postInstrSymbol()) {
    BeginAttachment("post-instr-symbol");
    Output.append("<mcsymbol ");
    Output.append(quoteName(*Instruction.postInstrSymbol()));
    Output.push_back('>');
  }
  if (Instruction.heapAllocMarker()) {
    BeginAttachment("heap-alloc-marker");
    if (auto Printed = printMetadataReference(*Instruction.heapAllocMarker()); !Printed) {
      return Printed;
    }
  }
  if (Instruction.pcSections()) {
    BeginAttachment("pcsections");
    if (auto Printed = printMetadataReference(*Instruction.pcSections()); !Printed) {
      return Printed;
    }
  }
  if (Instruction.mmra()) {
    BeginAttachment("mmra");
    if (auto Printed = printMetadataReference(*Instruction.mmra()); !Printed)
      return Printed;
  }
  if (Instruction.cfiType()) {
    BeginAttachment("cfi-type");
    Output.append(std::to_string(*Instruction.cfiType()));
  }
  if (Instruction.deactivationSymbol()) {
    BeginAttachment("deactivation-symbol");
    Output.push_back('@');
    Output.append(quoteName(*Instruction.deactivationSymbol()));
  }
  if (Instruction.debugInstrNumber() != 0) {
    BeginAttachment("debug-instr-number");
    Output.append(std::to_string(Instruction.debugInstrNumber()));
  }
  if (Instruction.debugLocation()) {
    BeginAttachment("debug-location");
    if (const MachineMetadataReference *Reference = Instruction.debugLocation()->reference()) {
      if (auto Printed = printMetadataReference(*Reference); !Printed)
        return Printed;
    } else if (const MachineDILocation *Location = Instruction.debugLocation()->inlineLocation()) {
      if (auto Printed = printDILocation(*Location, 0); !Printed)
        return Printed;
    } else {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine debug location has no printable value");
    }
  }

  if (!Instruction.memOperands().empty()) {
    Output.append(" :: ");
    for (std::size_t Index = 0; Index < Instruction.memOperands().size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      if (auto Printed = printMemoryOperand(*Instruction.memOperands()[Index]); !Printed) {
        return Printed;
      }
    }
  }
  return {};
}

Expected<void> BodyPrinter::printBlock(const MachineBasicBlock &Block) {
  auto ID = blockID(Block);
  if (!ID)
    return std::unexpected(ID.error());
  Output.append("bb.");
  Output.append(std::to_string(*ID));
  const auto &Name = Block.attributes().name();
  if (Name) {
    Output.push_back('.');
    Output.append(quoteName(*Name));
  }

  std::vector<std::string> Attributes;
  const auto AddFlag = [&Block, &Attributes](MachineBasicBlockAttribute Attribute,
                                             std::string_view Spelling) {
    if (Block.attributes().has(Attribute))
      Attributes.emplace_back(Spelling);
  };
  AddFlag(MachineBasicBlockAttribute::MachineBlockAddressTaken, "machine-block-address-taken");
  if (Block.attributes().has(MachineBasicBlockAttribute::IRBlockAddressTaken)) {
    std::string Attribute = "ir-block-address-taken";
    const auto &IRBlockName = Block.attributes().irBlockName();
    if (IRBlockName) {
      Attribute.append(" %ir-block.");
      Attribute.append(quoteName(*IRBlockName));
    }
    Attributes.push_back(std::move(Attribute));
  } else {
    const auto &IRBlockName = Block.attributes().irBlockName();
    if (IRBlockName)
      Attributes.push_back("%ir-block." + quoteName(*IRBlockName));
  }
  AddFlag(MachineBasicBlockAttribute::LandingPad, "landing-pad");
  AddFlag(MachineBasicBlockAttribute::InlineAsmBrIndirectTarget, "inlineasm-br-indirect-target");
  AddFlag(MachineBasicBlockAttribute::EHFuncletEntry, "ehfunclet-entry");
  AddFlag(MachineBasicBlockAttribute::EHScopeEntry, "ehscope-entry");
  if (Block.attributes().alignment().bytes() != 1) {
    Attributes.push_back("align " + std::to_string(Block.attributes().alignment().bytes()));
  }
  const MachineBasicBlockSectionID Section = Block.attributes().sectionID();
  if (!Section.isDefault()) {
    std::string Value;
    if (Section.kind() == MachineBasicBlockSectionKind::Exception)
      Value = "Exception";
    else if (Section.kind() == MachineBasicBlockSectionKind::Cold)
      Value = "Cold";
    else
      Value = std::to_string(Section.number());
    Attributes.push_back("bbsections " + Value);
  }
  const auto &ProfileID = Block.attributes().profileID();
  if (ProfileID) {
    const MachineBasicBlockProfileID Profile = *ProfileID;
    Attributes.push_back("bb_id " + std::to_string(Profile.BaseID) + "." +
                         std::to_string(Profile.CloneID));
  }
  if (Block.attributes().callFrameSize() != 0) {
    Attributes.push_back("call-frame-size " + std::to_string(Block.attributes().callFrameSize()));
  }
  if (!Attributes.empty()) {
    Output.append(" (");
    for (std::size_t Index = 0; Index < Attributes.size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      Output.append(Attributes[Index]);
    }
    Output.push_back(')');
  }
  Output.append(":\n");

  bool HasLineAttributes = false;
  if (!Block.successors().empty()) {
    Output.append("  successors: ");
    for (std::size_t Index = 0; Index < Block.successors().size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      const MachineSuccessor &Successor = Block.successors()[Index];
      if (Successor.Block == nullptr || !BlockIDs.contains(Successor.Block)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "successor references an unlinked block");
      }
      appendBlockReference(*Successor.Block);
      Output.append("(0x");
      Output.append(hexadecimalWords(
          std::array{static_cast<std::uint64_t>(Successor.Probability.rawValue())}, 32));
      Output.push_back(')');
    }
    Output.push_back('\n');
    HasLineAttributes = true;
  }
  if (!Block.liveIns().empty()) {
    Output.append("  liveins: ");
    for (std::size_t Index = 0; Index < Block.liveIns().size(); ++Index) {
      if (Index != 0)
        Output.append(", ");
      const MachineBasicBlockLiveIn &LiveIn = Block.liveIns()[Index];
      if (auto Printed = appendPhysicalRegister(LiveIn.PhysReg); !Printed)
        return Printed;
      if (LiveIn.Lanes != LaneMask::all()) {
        Output.append(": 0x");
        Output.append(hexadecimalWords(std::array{LiveIn.Lanes.rawValue()}, 64));
      }
    }
    Output.push_back('\n');
    HasLineAttributes = true;
  }

  if (HasLineAttributes && Block.firstInstruction() != nullptr)
    Output.push_back('\n');

  bool InBundle = false;
  for (const MachineInstr *Instruction = Block.firstInstruction(); Instruction != nullptr;
       Instruction = Instruction->next()) {
    if (auto Printed = printInstruction(*Instruction); !Printed)
      return Printed;
    const bool HasPredecessor = Instruction->flags().has(MachineInstrFlag::BundledWithPred);
    const bool HasSuccessor = Instruction->flags().has(MachineInstrFlag::BundledWithSucc);
    if (!InBundle && HasSuccessor && !HasPredecessor) {
      Output.append(" {");
      InBundle = true;
    }
    Output.push_back('\n');
    if (InBundle && !HasSuccessor) {
      Output.append("  }\n");
      InBundle = false;
    }
  }
  if (InBundle) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "unterminated instruction bundle cannot be printed");
  }
  return {};
}

Expected<std::string> BodyPrinter::run() {
  auto Verification = MachineVerifier::verify(
      Function, Target,
      MachineVerifierOptions{
          .Mode = MachineVerifierMode::Generic, .Stage = std::nullopt, .VerifyProperties = false});
  if (!Verification)
    return std::unexpected(Verification.error());
  if (auto Initialized = initialize(); !Initialized)
    return std::unexpected(Initialized.error());

  bool First = true;
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (!First)
      Output.push_back('\n');
    First = false;
    if (auto Printed = printBlock(*Block); !Printed)
      return std::unexpected(Printed.error());
  }
  return std::move(Output);
}

} // namespace

Expected<std::string> printMachineFunctionBody(const MachineFunction &Function,
                                               const TargetDescription &Target,
                                               const MIParserContext &Context,
                                               MIPrinterOptions Options) {
  return BodyPrinter(Function, Target, Context, Options).run();
}

} // namespace rcc::mir
