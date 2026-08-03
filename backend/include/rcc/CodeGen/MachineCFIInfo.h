// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINECFIINFO_H
#define RCC_CODEGEN_MACHINECFIINFO_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rcc {

class DwarfRegisterNumber final {
public:
  constexpr DwarfRegisterNumber() noexcept = default;

  [[nodiscard]] static constexpr DwarfRegisterNumber fromValue(std::uint32_t Value) noexcept {
    return DwarfRegisterNumber(Value);
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept {
    return Value;
  }

  friend constexpr bool operator==(DwarfRegisterNumber Left,
                                   DwarfRegisterNumber Right) noexcept = default;

private:
  explicit constexpr DwarfRegisterNumber(std::uint32_t Value) noexcept : Value(Value) {}

  std::uint32_t Value = 0;
};

enum class MachineCFIOperation : std::uint8_t {
  SameValue,
  RememberState,
  RestoreState,
  Offset,
  LLVMDefAddressSpaceCFA,
  DefCFARegister,
  DefCFAOffset,
  DefCFA,
  RelativeOffset,
  AdjustCFAOffset,
  Escape,
  Restore,
  Undefined,
  Register,
  WindowSave,
  NegateReturnAddressState,
  NegateReturnAddressStateWithPC,
  GNUArgsSize,
  Label,
  ValueOffset,
  LLVMRegisterPair,
  LLVMVectorRegisters,
  LLVMVectorOffset,
  LLVMVectorRegisterMask,
  Count,
};

struct MachineCFINoOperands final {
  friend constexpr bool operator==(MachineCFINoOperands Left,
                                   MachineCFINoOperands Right) noexcept = default;
};

struct MachineCFIRegisterRule final {
  DwarfRegisterNumber Register;

  friend constexpr bool operator==(MachineCFIRegisterRule Left,
                                   MachineCFIRegisterRule Right) noexcept = default;
};

struct MachineCFIOffsetRule final {
  std::int64_t Offset = 0;

  friend constexpr bool operator==(MachineCFIOffsetRule Left,
                                   MachineCFIOffsetRule Right) noexcept = default;
};

struct MachineCFIRegisterOffsetRule final {
  DwarfRegisterNumber Register;
  std::int64_t Offset = 0;

  friend constexpr bool operator==(MachineCFIRegisterOffsetRule Left,
                                   MachineCFIRegisterOffsetRule Right) noexcept = default;
};

struct MachineCFIRegisterRegisterRule final {
  DwarfRegisterNumber Register;
  DwarfRegisterNumber SourceRegister;

  friend constexpr bool operator==(MachineCFIRegisterRegisterRule Left,
                                   MachineCFIRegisterRegisterRule Right) noexcept = default;
};

struct MachineCFIAddressSpaceCFARule final {
  DwarfRegisterNumber Register;
  std::int64_t Offset = 0;
  std::uint32_t AddressSpace = 0;

  friend constexpr bool operator==(MachineCFIAddressSpaceCFARule Left,
                                   MachineCFIAddressSpaceCFARule Right) noexcept = default;
};

struct MachineCFIEscapeRule final {
  std::vector<std::uint8_t> Bytes;

  friend bool operator==(const MachineCFIEscapeRule &Left,
                         const MachineCFIEscapeRule &Right) = default;
};

struct MachineCFILabelRule final {
  std::string Symbol;

  friend bool operator==(const MachineCFILabelRule &Left,
                         const MachineCFILabelRule &Right) = default;
};

struct MachineCFIRegisterPairRule final {
  DwarfRegisterNumber Register;
  DwarfRegisterNumber FirstRegister;
  std::uint32_t FirstSizeInBits = 0;
  DwarfRegisterNumber SecondRegister;
  std::uint32_t SecondSizeInBits = 0;

  friend constexpr bool operator==(MachineCFIRegisterPairRule Left,
                                   MachineCFIRegisterPairRule Right) noexcept = default;
};

struct MachineCFIVectorRegisterLane final {
  DwarfRegisterNumber Register;
  std::uint32_t Lane = 0;
  std::uint32_t SizeInBits = 0;

  friend constexpr bool operator==(MachineCFIVectorRegisterLane Left,
                                   MachineCFIVectorRegisterLane Right) noexcept = default;
};

struct MachineCFIVectorRegistersRule final {
  DwarfRegisterNumber Register;
  std::vector<MachineCFIVectorRegisterLane> VectorRegisters;

  friend bool operator==(const MachineCFIVectorRegistersRule &Left,
                         const MachineCFIVectorRegistersRule &Right) = default;
};

struct MachineCFIVectorOffsetRule final {
  DwarfRegisterNumber Register;
  std::uint32_t RegisterSizeInBits = 0;
  DwarfRegisterNumber MaskRegister;
  std::uint32_t MaskRegisterSizeInBits = 0;
  std::int64_t Offset = 0;

  friend constexpr bool operator==(MachineCFIVectorOffsetRule Left,
                                   MachineCFIVectorOffsetRule Right) noexcept = default;
};

struct MachineCFIVectorRegisterMaskRule final {
  DwarfRegisterNumber Register;
  DwarfRegisterNumber SpillRegister;
  std::uint32_t SpillRegisterLaneSizeInBits = 0;
  DwarfRegisterNumber MaskRegister;
  std::uint32_t MaskRegisterSizeInBits = 0;

  friend constexpr bool operator==(MachineCFIVectorRegisterMaskRule Left,
                                   MachineCFIVectorRegisterMaskRule Right) noexcept = default;
};

class MachineCFIInstruction final {
public:
  using Payload =
      std::variant<MachineCFINoOperands, MachineCFIRegisterRule, MachineCFIOffsetRule,
                   MachineCFIRegisterOffsetRule, MachineCFIRegisterRegisterRule,
                   MachineCFIAddressSpaceCFARule, MachineCFIEscapeRule, MachineCFILabelRule,
                   MachineCFIRegisterPairRule, MachineCFIVectorRegistersRule,
                   MachineCFIVectorOffsetRule, MachineCFIVectorRegisterMaskRule>;

  [[nodiscard]] static MachineCFIInstruction sameValue(DwarfRegisterNumber Register);
  [[nodiscard]] static MachineCFIInstruction rememberState();
  [[nodiscard]] static MachineCFIInstruction restoreState();
  [[nodiscard]] static MachineCFIInstruction offset(DwarfRegisterNumber Register,
                                                    std::int64_t Offset);
  [[nodiscard]] static MachineCFIInstruction llvmDefAddressSpaceCFA(DwarfRegisterNumber Register,
                                                                    std::int64_t Offset,
                                                                    std::uint32_t AddressSpace);
  [[nodiscard]] static MachineCFIInstruction defCFARegister(DwarfRegisterNumber Register);
  [[nodiscard]] static MachineCFIInstruction defCFAOffset(std::int64_t Offset);
  [[nodiscard]] static MachineCFIInstruction defCFA(DwarfRegisterNumber Register,
                                                    std::int64_t Offset);
  [[nodiscard]] static MachineCFIInstruction relativeOffset(DwarfRegisterNumber Register,
                                                            std::int64_t Offset);
  [[nodiscard]] static MachineCFIInstruction adjustCFAOffset(std::int64_t Offset);
  [[nodiscard]] static Expected<MachineCFIInstruction> escape(std::vector<std::uint8_t> Bytes);
  [[nodiscard]] static MachineCFIInstruction restore(DwarfRegisterNumber Register);
  [[nodiscard]] static MachineCFIInstruction undefined(DwarfRegisterNumber Register);
  [[nodiscard]] static MachineCFIInstruction registerRule(DwarfRegisterNumber Register,
                                                          DwarfRegisterNumber SourceRegister);
  [[nodiscard]] static MachineCFIInstruction windowSave();
  [[nodiscard]] static MachineCFIInstruction negateReturnAddressState();
  [[nodiscard]] static MachineCFIInstruction negateReturnAddressStateWithPC();
  [[nodiscard]] static MachineCFIInstruction gnuArgsSize(std::int64_t Size);
  [[nodiscard]] static Expected<MachineCFIInstruction> label(std::string Symbol);
  [[nodiscard]] static MachineCFIInstruction valueOffset(DwarfRegisterNumber Register,
                                                         std::int64_t Offset);
  [[nodiscard]] static MachineCFIInstruction llvmRegisterPair(MachineCFIRegisterPairRule Rule);
  [[nodiscard]] static Expected<MachineCFIInstruction>
  llvmVectorRegisters(MachineCFIVectorRegistersRule Rule);
  [[nodiscard]] static MachineCFIInstruction llvmVectorOffset(MachineCFIVectorOffsetRule Rule);
  [[nodiscard]] static MachineCFIInstruction
  llvmVectorRegisterMask(MachineCFIVectorRegisterMaskRule Rule);

  [[nodiscard]] MachineCFIOperation operation() const noexcept {
    return Operation;
  }

  template <typename PayloadT> [[nodiscard]] const PayloadT *getIf() const noexcept {
    return std::get_if<PayloadT>(&Data);
  }

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const MachineCFIInstruction &Left,
                         const MachineCFIInstruction &Right) = default;

private:
  MachineCFIInstruction(MachineCFIOperation Operation, Payload Data)
      : Operation(Operation), Data(std::move(Data)) {}

  MachineCFIOperation Operation;
  Payload Data;
};

class MachineCFIInfo final {
public:
  MachineCFIInfo() = default;
  MachineCFIInfo(const MachineCFIInfo &) = delete;
  MachineCFIInfo &operator=(const MachineCFIInfo &) = delete;
  MachineCFIInfo(MachineCFIInfo &&) noexcept = default;
  MachineCFIInfo &operator=(MachineCFIInfo &&) noexcept = default;
  ~MachineCFIInfo() = default;

  [[nodiscard]] Expected<CFIInstructionIndex> addInstruction(MachineCFIInstruction Instruction);

  [[nodiscard]] const MachineCFIInstruction *instruction(CFIInstructionIndex Index) const noexcept;

  [[nodiscard]] std::span<const MachineCFIInstruction> instructions() const noexcept {
    return Instructions;
  }

  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<MachineCFIInfo> clone() const;

  friend bool operator==(const MachineCFIInfo &Left, const MachineCFIInfo &Right) {
    return Left.Instructions == Right.Instructions;
  }

private:
  std::vector<MachineCFIInstruction> Instructions;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINECFIINFO_H
