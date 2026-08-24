// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEOUTLINER_H
#define RCC_CODEGEN_MACHINEOUTLINER_H

#include "rcc/CodeGen/MachinePassManager.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rcc {

class MachineBasicBlock;
class MachineFunction;
class MachineInstr;
class TargetDescription;

enum class MachineOutlinerInstrType : std::uint8_t {
  Legal,
  LegalTerminator,
  Illegal,
  Invisible,
};

struct MachineOutlinerFunctionProperties final {
  bool LinkOnceODR = false;
  bool HasSection = false;
  bool HasComdat = false;
  bool HasSectionPrefix = false;
};

using MachineOutlinerFunctionPropertyMap =
    std::unordered_map<std::string, MachineOutlinerFunctionProperties>;

struct MachineOutlinerOptions final {
  MachineOutlinerFunctionPropertyMap FunctionProperties;
};

struct MachineOutlinerCandidate final {
  MachineFunction *Function = nullptr;
  MachineBasicBlock *Block = nullptr;
  std::vector<MachineInstr *> Instructions;
  std::vector<PhysicalRegister> LiveIns;
  std::vector<PhysicalRegister> DefinedRegisters;
  std::uint32_t CallConstructionID = 0;
  std::uint32_t CallOverhead = 0;
  PhysicalRegister SaveRegister;

  [[nodiscard]] MachineInstr &front() const noexcept;
  [[nodiscard]] MachineInstr &back() const noexcept;
  [[nodiscard]] bool containsRegister(PhysicalRegister Reg,
                                      const TargetDescription &Target) const noexcept;
  [[nodiscard]] bool isRegisterLiveAfter(PhysicalRegister Reg,
                                         const TargetDescription &Target) const noexcept;
  [[nodiscard]] bool isRegisterAvailable(PhysicalRegister Reg,
                                         const TargetDescription &Target) const noexcept;
};

class TargetMachineOutlinerInfo {
public:
  virtual ~TargetMachineOutlinerInfo();

  [[nodiscard]] virtual bool isFunctionSafeToOutlineFrom(
      const MachineFunction &Function,
      const MachineOutlinerFunctionProperties &Properties) const noexcept = 0;
  [[nodiscard]] virtual MachineOutlinerInstrType
  getOutliningType(const MachineInstr &Instruction,
                   const MachineOutlinerFunctionProperties &Properties,
                   const TargetDescription &Target) const noexcept = 0;
  [[nodiscard]] virtual Expected<bool> analyzeCandidate(MachineOutlinerCandidate &Candidate,
                                                        const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<std::uint32_t>
  instructionSize(const MachineInstr &Instruction, const MachineFunction &Function,
                  const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual std::uint32_t
  frameOverhead(const MachineOutlinerCandidate &Candidate) const noexcept = 0;
  [[nodiscard]] virtual Expected<void>
  insertOutlinedCall(MachineOutlinerCandidate &Candidate, std::string OutlinedFunctionName,
                     const TargetDescription &Target) const = 0;
  [[nodiscard]] virtual Expected<void>
  buildOutlinedFrame(MachineFunction &Function, MachineBasicBlock &Block,
                     const MachineOutlinerCandidate &Candidate,
                     const TargetDescription &Target) const = 0;
};

[[nodiscard]] std::unique_ptr<CompilationUnitPass>
createMachineOutlinerPass(std::unique_ptr<TargetMachineOutlinerInfo> TargetInfo,
                          MachineOutlinerOptions Options = {});

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEOUTLINER_H
