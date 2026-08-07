// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_GENERICOPCODES_H
#define RCC_CODEGEN_GENERICOPCODES_H

#include "rcc/CodeGen/InstructionDescriptor.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rcc {

// Target-independent values are build-local. Target tables start at 1024 and
// are appended to this set when a target description is constructed.
enum class GenericOpcode : std::uint32_t {
  Invalid,
#define RCC_CORE_OPCODE(Name) Name,
#define RCC_GENERIC_OPCODE(Name)
#include "rcc/CodeGen/GenericOpcode.def"
#undef RCC_GENERIC_OPCODE
#undef RCC_CORE_OPCODE
  CORE_PSEUDO_END,

#define RCC_CORE_OPCODE(Name)
#define RCC_GENERIC_OPCODE(Name) Name,
#include "rcc/CodeGen/GenericOpcode.def"
#undef RCC_GENERIC_OPCODE
#undef RCC_CORE_OPCODE
  G_GENERIC_END,
};

[[nodiscard]] constexpr Opcode opcode(GenericOpcode Generic) noexcept {
  return Opcode::fromValue(static_cast<std::uint32_t>(Generic));
}

[[nodiscard]] constexpr bool isGenericOpcode(Opcode Value) noexcept {
  return Value.value() >= static_cast<std::uint32_t>(GenericOpcode::G_ASSERT_SEXT) &&
         Value.value() < static_cast<std::uint32_t>(GenericOpcode::G_GENERIC_END);
}

[[nodiscard]] constexpr bool isCorePseudoOpcode(Opcode Value) noexcept {
  return Value.value() >= static_cast<std::uint32_t>(GenericOpcode::PHI) &&
         Value.value() < static_cast<std::uint32_t>(GenericOpcode::CORE_PSEUDO_END);
}

inline constexpr std::size_t CorePseudoOpcodeCount =
    static_cast<std::size_t>(GenericOpcode::CORE_PSEUDO_END) -
    static_cast<std::size_t>(GenericOpcode::PHI);

inline constexpr std::size_t GenericOpcodeCount =
    static_cast<std::size_t>(GenericOpcode::G_GENERIC_END) -
    static_cast<std::size_t>(GenericOpcode::G_ASSERT_SEXT);

[[nodiscard]] std::string_view getGenericOpcodeName(GenericOpcode OpcodeValue) noexcept;

[[nodiscard]] std::span<const InstructionDescriptor> genericInstructionDescriptors() noexcept;

[[nodiscard]] std::span<const ImmediatePredicate> genericImmediatePredicates() noexcept;

[[nodiscard]] Expected<void>
appendGenericInstructionInfo(std::vector<InstructionDescriptor> &Descriptors,
                             std::vector<ImmediatePredicate> &Predicates);

} // namespace rcc

#endif // RCC_CODEGEN_GENERICOPCODES_H
