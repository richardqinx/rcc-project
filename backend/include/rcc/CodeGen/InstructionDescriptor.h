// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_INSTRUCTIONDESCRIPTOR_H
#define RCC_CODEGEN_INSTRUCTIONDESCRIPTOR_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rcc {

class MachineInstr;
class MachineRegisterInfo;

class OperandKindSet final {
public:
  static_assert(static_cast<std::uint32_t>(MachineOperand::Kind::Count) <= 32);

  constexpr OperandKindSet() noexcept = default;

  OperandKindSet(std::initializer_list<MachineOperand::Kind> Kinds) noexcept;

  [[nodiscard]] constexpr bool empty() const noexcept {
    return Mask == 0;
  }

  [[nodiscard]] constexpr bool contains(MachineOperand::Kind Kind) const noexcept {
    const auto Index = static_cast<std::uint32_t>(Kind);
    if (Index >= static_cast<std::uint32_t>(MachineOperand::Kind::Count))
      return false;
    return (Mask & (std::uint32_t{1} << Index)) != 0;
  }

  constexpr void add(MachineOperand::Kind Kind) noexcept {
    const auto Index = static_cast<std::uint32_t>(Kind);
    if (Index < static_cast<std::uint32_t>(MachineOperand::Kind::Count))
      Mask |= std::uint32_t{1} << Index;
  }

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(OperandKindSet Left, OperandKindSet Right) noexcept = default;

private:
  std::uint32_t Mask = 0;
};

enum class RegisterOperandRole : std::uint8_t {
  None,
  Def,
  Use,
  DefUse,
};

enum class MemoryOperandRole : std::uint8_t {
  None,
  Address,
  Value,
  Size,
  Ordering,
};

enum class InstructionFlag : std::uint8_t {
  Terminator,
  Branch,
  IndirectBranch,
  Call,
  Return,
  Barrier,
  MayLoad,
  MayStore,
  HasSideEffects,
  Pseudo,
  Meta,
  AllowedPreLegalizer,
  ProducesCode,
  Commutable,
  Rematerializable,
  Predicable,
  NotDuplicable,
  Convergent,
  UnmodeledSideEffects,
  Phi,
  Generic,
  TerminalEmission,
  Count,
};

class InstructionFlags final {
public:
  static_assert(static_cast<std::uint32_t>(InstructionFlag::Count) <= 32);

  constexpr InstructionFlags() noexcept = default;

  InstructionFlags(std::initializer_list<InstructionFlag> Flags) noexcept;

  [[nodiscard]] constexpr bool has(InstructionFlag Flag) const noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index >= static_cast<std::uint32_t>(InstructionFlag::Count))
      return false;
    return (Mask & (std::uint32_t{1} << Index)) != 0;
  }

  constexpr void set(InstructionFlag Flag) noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index < static_cast<std::uint32_t>(InstructionFlag::Count))
      Mask |= std::uint32_t{1} << Index;
  }

  constexpr void clear(InstructionFlag Flag) noexcept {
    const auto Index = static_cast<std::uint32_t>(Flag);
    if (Index < static_cast<std::uint32_t>(InstructionFlag::Count))
      Mask &= ~(std::uint32_t{1} << Index);
  }

  [[nodiscard]] constexpr std::uint32_t rawValue() const noexcept {
    return Mask;
  }

  friend constexpr bool operator==(InstructionFlags Left,
                                   InstructionFlags Right) noexcept = default;

private:
  std::uint32_t Mask = 0;
};

enum class PseudoOwner : std::uint8_t {
  None,
  Legalizer,
  RegisterBankSelect,
  InstructionSelect,
  PhiElimination,
  TwoAddressRewrite,
  RegisterAllocator,
  PrePEIExpansion,
  PrologueEpilogueInsertion,
  PostRAExpansion,
  Finalization,
  AsmPrinter,
};

enum class PipelineStage : std::uint8_t {
  MachineIRInput,
  Legalized,
  RegBankSelected,
  Selected,
  SSAEliminated,
  TiedOpsRewritten,
  RegisterAllocated,
  FrameFinalized,
  PostRAFinalized,
  Emission,
};

struct OperandDescriptor final {
  OperandKindSet Kinds;
  RegisterOperandRole RegisterRole = RegisterOperandRole::None;
  std::optional<RegisterClassID> RegClass;
  std::optional<RegisterBankID> RegBank;
  std::optional<ImmediatePredicateID> ImmediatePredicate;
  MemoryOperandRole MemoryRole = MemoryOperandRole::None;
  std::optional<std::size_t> DefaultTiedOperand;
  bool EarlyClobber = false;
  bool Optional = false;
  bool VariadicGroup = false;
  bool AllowSubReg = false;
  std::optional<NamedOperandID> Name;
  std::optional<TargetOperandTypeID> TargetOperandType;
  std::uint32_t AllowedTargetFlags = 0;
};

struct ImplicitRegisterDescriptor final {
  PhysicalRegister Reg;
  bool EarlyClobber = false;
  bool Renamable = false;

  friend bool operator==(const ImplicitRegisterDescriptor &Left,
                         const ImplicitRegisterDescriptor &Right) = default;
};

struct InstructionDescriptor final {
  Opcode InstructionOpcode;
  std::string Name;
  std::vector<OperandDescriptor> Operands;
  std::size_t NumExplicitDefs = 0;
  bool Variadic = false;
  std::vector<ImplicitRegisterDescriptor> ImplicitDefs;
  std::vector<ImplicitRegisterDescriptor> ImplicitUses;
  bool AllowExtraImplicitOperands = false;
  InstructionFlags Flags;
  SchedulingClassID SchedulingClass;
  EncodingCategoryID EncodingCategory;
  AssemblyFormatID AssemblyFormat;
  bool RequiresMemoryOperands = false;
  PseudoOwner Owner = PseudoOwner::None;
  PipelineStage Deadline = PipelineStage::Emission;
};

struct ImmediatePredicate final {
  ImmediatePredicateID ID;
  std::string Name;
  std::function<bool(const MachineOperand &)> Predicate;
};

class InstructionSet final {
public:
  [[nodiscard]] static Expected<InstructionSet>
  create(std::vector<InstructionDescriptor> Descriptors,
         std::vector<ImmediatePredicate> ImmediatePredicates = {});

  [[nodiscard]] const InstructionDescriptor *descriptor(Opcode InstructionOpcode) const noexcept;
  [[nodiscard]] const InstructionDescriptor *descriptor(std::string_view Name) const noexcept;
  [[nodiscard]] std::span<const InstructionDescriptor> descriptors() const noexcept {
    return Descriptors;
  }

  [[nodiscard]] Expected<void> validateExplicitOperands(Opcode InstructionOpcode,
                                                        std::span<const MachineOperand> Operands,
                                                        const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateInstruction(const MachineInstr &Instruction,
                                                   const MachineRegisterInfo &RegInfo) const;

  [[nodiscard]] Expected<std::vector<MachineOperand>>
  createDefaultImplicitOperands(const InstructionDescriptor &Descriptor) const;

private:
  InstructionSet(std::vector<InstructionDescriptor> Descriptors,
                 std::vector<ImmediatePredicate> ImmediatePredicates)
      : Descriptors(std::move(Descriptors)), ImmediatePredicates(std::move(ImmediatePredicates)) {}

  [[nodiscard]] const ImmediatePredicate *
  immediatePredicate(ImmediatePredicateID ID) const noexcept;
  [[nodiscard]] Expected<void> validateExplicitOperands(const InstructionDescriptor &Descriptor,
                                                        std::span<const MachineOperand> Operands,
                                                        const MachineRegisterInfo &RegInfo) const;
  [[nodiscard]] Expected<void> validateOperand(const OperandDescriptor &Descriptor,
                                               const MachineOperand &Operand,
                                               const MachineRegisterInfo &RegInfo) const;

  std::vector<InstructionDescriptor> Descriptors;
  std::vector<ImmediatePredicate> ImmediatePredicates;
  std::unordered_map<Opcode, std::size_t> OpcodeToIndex;
  std::unordered_map<std::string, std::size_t> NameToIndex;
  std::unordered_map<ImmediatePredicateID, std::size_t> PredicateToIndex;
};

} // namespace rcc

#endif // RCC_CODEGEN_INSTRUCTIONDESCRIPTOR_H
