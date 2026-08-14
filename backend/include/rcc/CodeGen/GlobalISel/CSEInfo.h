// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GLOBALISEL_CSEINFO_H
#define RCC_CODEGEN_GLOBALISEL_CSEINFO_H

#include "rcc/ADT/APInt.h"
#include "rcc/CodeGen/CSEConfigBase.h"
#include "rcc/CodeGen/CommandFlags.h"
#include "rcc/CodeGen/GlobalISel/GISelChangeObserver.h"
#include "rcc/CodeGen/GlobalISel/GISelWorkList.h"
#include "rcc/CodeGen/LowLevelType.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class MachineIRBuilderResult;

class CSEConfigFull final : public CSEConfigBase {
public:
  [[nodiscard]] bool shouldCSEOpcode(Opcode InstructionOpcode) const noexcept override;
};

class CSEConfigConstantOnly final : public CSEConfigBase {
public:
  [[nodiscard]] bool shouldCSEOpcode(Opcode InstructionOpcode) const noexcept override;
};

[[nodiscard]] std::unique_ptr<CSEConfigBase> getStandardCSEConfigForOpt(OptimizationLevel Level);

class GISelCSEInfo final : public GISelChangeObserver {
public:
  GISelCSEInfo() = default;
  GISelCSEInfo(const GISelCSEInfo &) = delete;
  GISelCSEInfo &operator=(const GISelCSEInfo &) = delete;
  ~GISelCSEInfo() override = default;

  void setCSEConfig(std::unique_ptr<CSEConfigBase> Config) noexcept;
  [[nodiscard]] bool shouldCSE(Opcode InstructionOpcode) const noexcept;
  [[nodiscard]] Expected<void> analyze(MachineFunction &Function);
  [[nodiscard]] Expected<void> verify();
  void releaseMemory() noexcept;

  void erasingInstr(MachineInstr &Instruction) override;
  void createdInstr(MachineInstr &Instruction) override;
  void changingInstr(MachineInstr &Instruction) override;
  void changedInstr(MachineInstr &Instruction) override;

private:
  struct CSEKey final {
    const MachineBasicBlock *Block = nullptr;
    Opcode InstructionOpcode;
    std::vector<std::uint64_t> Profile;

    friend bool operator==(const CSEKey &, const CSEKey &) = default;
  };

  struct CSEKeyHash final {
    [[nodiscard]] std::size_t operator()(const CSEKey &Key) const noexcept;
  };

  [[nodiscard]] Expected<void> appendRegisterProfile(std::vector<std::uint64_t> &Profile,
                                                     Register Reg, bool IsDefinition) const;
  [[nodiscard]] Expected<void> appendResultProfile(std::vector<std::uint64_t> &Profile,
                                                   const MachineIRBuilderResult &Result) const;
  [[nodiscard]] Expected<CSEKey> profileInstruction(const MachineInstr &Instruction) const;
  [[nodiscard]] Expected<CSEKey> profileBuilderInstruction(
      Opcode InstructionOpcode, std::span<const MachineIRBuilderResult> Destinations,
      std::span<const Register> Sources, std::span<const std::int64_t> Immediates,
      MachineInstrFlags Flags, const MachineBasicBlock &Block) const;
  [[nodiscard]] Expected<CSEKey> profileConstant(const MachineIRBuilderResult &Destination,
                                                 const APInt &Value,
                                                 const MachineBasicBlock &Block) const;

  void recordNewInstruction(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> handleRecordedInstruction(MachineInstr &Instruction);
  [[nodiscard]] Expected<void> handleRecordedInstructions();
  void handleRemoveInstruction(MachineInstr &Instruction) noexcept;
  [[nodiscard]] Expected<void> insertInstruction(MachineInstr &Instruction);

  [[nodiscard]] Expected<MachineInstr *> getMachineInstrIfExists(
      Opcode InstructionOpcode, std::span<const MachineIRBuilderResult> Destinations,
      std::span<const Register> Sources, std::span<const std::int64_t> Immediates,
      MachineInstrFlags Flags, MachineBasicBlock &Block);
  [[nodiscard]] Expected<MachineInstr *>
  getConstantIfExists(const MachineIRBuilderResult &Destination, const APInt &Value,
                      MachineBasicBlock &Block);
  void countOpcodeHit(Opcode InstructionOpcode) noexcept;

  MachineFunction *Function = nullptr;
  std::unique_ptr<CSEConfigBase> Config;
  std::unordered_map<CSEKey, MachineInstr *, CSEKeyHash> CSEMap;
  std::unordered_map<const MachineInstr *, CSEKey> InstructionMapping;
  GISelWorkList<8> TemporaryInstructions;
  std::unordered_map<std::uint32_t, std::size_t> OpcodeHitTable;
  bool HandlingRecordedInstructions = false;

  friend class CSEMIRBuilder;
};

} // namespace rcc

#endif // RCC_CODEGEN_GLOBALISEL_CSEINFO_H
