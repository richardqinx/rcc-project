// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVTARGET_H
#define RCC_TARGET_RISCV_RISCVTARGET_H

#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/Support/Error.h"
#include "rcc/Target/RISCV/RISCVInstructions.h"
#include "rcc/Target/RISCV/RISCVRegisters.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace rcc::riscv {

struct SchedulingClassInfo final {
  SchedulingClassID ID;
  std::string_view Name;
  std::uint32_t MicroOps = 0;
  std::uint32_t Latency = 0;
};

struct EncodingCategoryInfo final {
  EncodingCategoryID ID;
  std::string_view Name;
  std::uint32_t SizeInBytes = 0;
};

struct AssemblyFormatInfo final {
  AssemblyFormatID ID;
  std::string_view Name;
};

struct AssemblyInstructionInfo final {
  Opcode InstructionOpcode;
  std::string_view Mnemonic;
  AssemblyFormatID Format;
};

struct CallingConventionInfo final {
  CallingConvention ID;
  std::string_view Name;
  RegisterClassID IntegerRegisterClass;
  RegisterClassID FloatingRegisterClass;
  std::span<const PhysicalRegister> IntegerArgumentRegisters;
  std::span<const PhysicalRegister> FloatingArgumentRegisters;
  std::span<const PhysicalRegister> IntegerReturnRegisters;
  std::span<const PhysicalRegister> FloatingReturnRegisters;
  std::string_view PreservedMaskName;
  std::uint32_t StackAlignmentBytes = 0;
  std::uint32_t StackArgumentAlignmentBytes = 0;
  std::uint32_t ShadowSpaceBytes = 0;
  bool SupportsVarArgs = false;
  bool SupportsTailCalls = false;
};

[[nodiscard]] Expected<PhysicalRegister> physicalRegister(PhysicalRegisterID ID);
[[nodiscard]] Expected<TargetRegisterInfo> createRegisterInfo();
[[nodiscard]] Expected<TargetMIRTextInfo> createMIRTextInfo();
[[nodiscard]] Expected<TargetDescription> createTargetDescription();
[[nodiscard]] std::shared_ptr<const RegisterMask> createCallPreservedMask();
[[nodiscard]] std::span<const SchedulingClassInfo> schedulingClasses() noexcept;
[[nodiscard]] std::span<const EncodingCategoryInfo> encodingCategories() noexcept;
[[nodiscard]] std::span<const AssemblyFormatInfo> assemblyFormats() noexcept;
[[nodiscard]] std::span<const AssemblyInstructionInfo> assemblyInstructions() noexcept;
[[nodiscard]] const AssemblyInstructionInfo *assemblyInstruction(Opcode InstructionOpcode) noexcept;
[[nodiscard]] std::span<const CallingConventionInfo> callingConventions() noexcept;
[[nodiscard]] const CallingConventionInfo *callingConvention(CallingConvention ID) noexcept;

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVTARGET_H
