// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_LIB_TARGET_RISCV_RISCVIMMEDIATEMATERIALIZATION_H
#define RCC_LIB_TARGET_RISCV_RISCVIMMEDIATEMATERIALIZATION_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rcc::riscv {

enum class InstructionOpcode : std::uint32_t;
class RISCVSubtargetFeatures;

enum class ImmediateMaterializationOperandKind : std::uint8_t {
  Immediate,
  RegisterImmediate,
};

struct ImmediateMaterializationInstruction final {
  InstructionOpcode Opcode;
  std::int64_t Immediate = 0;
  ImmediateMaterializationOperandKind OperandKind =
      ImmediateMaterializationOperandKind::RegisterImmediate;
};

using ImmediateMaterializationSequence = std::vector<ImmediateMaterializationInstruction>;

inline constexpr std::size_t DefaultRV64IntegerBuildCost = 5;

[[nodiscard]] ImmediateMaterializationSequence
generateRV64IntegerMaterialization(std::int64_t Value, const RISCVSubtargetFeatures &Features);

[[nodiscard]] std::size_t estimateRV64IntegerBuildCost(std::int64_t Value,
                                                       const RISCVSubtargetFeatures &Features);

[[nodiscard]] bool
shouldUseRV64IntegerConstantPool(std::int64_t Value, const RISCVSubtargetFeatures &Features,
                                 std::size_t MaximumBuildCost = DefaultRV64IntegerBuildCost);

} // namespace rcc::riscv

#endif // RCC_LIB_TARGET_RISCV_RISCVIMMEDIATEMATERIALIZATION_H
