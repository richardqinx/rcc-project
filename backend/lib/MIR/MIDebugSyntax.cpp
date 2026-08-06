// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/MIDebugSyntax.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

namespace rcc::mir {
namespace {

[[nodiscard]] bool parseIndex(std::string_view Text, std::uint64_t &Value) noexcept {
  if (Text.empty())
    return false;
  const auto [End, Error] =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  return Error == std::errc{} && End == Text.data() + Text.size();
}

} // namespace

std::optional<std::uint64_t>
parseDIExpressionOpcodeName(std::string_view Name) noexcept {
  const auto NumberedOpcode = [Name](std::string_view Prefix,
                                     std::uint64_t Base) -> std::optional<std::uint64_t> {
    if (!Name.starts_with(Prefix))
      return std::nullopt;
    std::uint64_t Index = 0;
    if (!parseIndex(Name.substr(Prefix.size()), Index) || Index > 31)
      return std::nullopt;
    return Base + Index;
  };
  if (auto Opcode = NumberedOpcode("DW_OP_lit", 0x30U))
    return Opcode;
  if (auto Opcode = NumberedOpcode("DW_OP_reg", 0x50U))
    return Opcode;
  if (auto Opcode = NumberedOpcode("DW_OP_breg", 0x70U))
    return Opcode;

  constexpr auto NamedOpcodes = std::to_array<std::pair<std::string_view, std::uint64_t>>({
      {"DW_OP_addr", 0x03},
      {"DW_OP_deref", 0x06},
      {"DW_OP_const1u", 0x08},
      {"DW_OP_const1s", 0x09},
      {"DW_OP_const2u", 0x0A},
      {"DW_OP_const2s", 0x0B},
      {"DW_OP_const4u", 0x0C},
      {"DW_OP_const4s", 0x0D},
      {"DW_OP_const8u", 0x0E},
      {"DW_OP_const8s", 0x0F},
      {"DW_OP_constu", 0x10},
      {"DW_OP_consts", 0x11},
      {"DW_OP_dup", 0x12},
      {"DW_OP_drop", 0x13},
      {"DW_OP_over", 0x14},
      {"DW_OP_pick", 0x15},
      {"DW_OP_swap", 0x16},
      {"DW_OP_rot", 0x17},
      {"DW_OP_xderef", 0x18},
      {"DW_OP_abs", 0x19},
      {"DW_OP_and", 0x1A},
      {"DW_OP_div", 0x1B},
      {"DW_OP_minus", 0x1C},
      {"DW_OP_mod", 0x1D},
      {"DW_OP_mul", 0x1E},
      {"DW_OP_neg", 0x1F},
      {"DW_OP_not", 0x20},
      {"DW_OP_or", 0x21},
      {"DW_OP_plus", 0x22},
      {"DW_OP_plus_uconst", 0x23},
      {"DW_OP_shl", 0x24},
      {"DW_OP_shr", 0x25},
      {"DW_OP_shra", 0x26},
      {"DW_OP_xor", 0x27},
      {"DW_OP_bra", 0x28},
      {"DW_OP_eq", 0x29},
      {"DW_OP_ge", 0x2A},
      {"DW_OP_gt", 0x2B},
      {"DW_OP_le", 0x2C},
      {"DW_OP_lt", 0x2D},
      {"DW_OP_ne", 0x2E},
      {"DW_OP_skip", 0x2F},
      {"DW_OP_regx", 0x90},
      {"DW_OP_fbreg", 0x91},
      {"DW_OP_bregx", 0x92},
      {"DW_OP_piece", 0x93},
      {"DW_OP_deref_size", 0x94},
      {"DW_OP_xderef_size", 0x95},
      {"DW_OP_nop", 0x96},
      {"DW_OP_push_object_address", 0x97},
      {"DW_OP_call2", 0x98},
      {"DW_OP_call4", 0x99},
      {"DW_OP_call_ref", 0x9A},
      {"DW_OP_form_tls_address", 0x9B},
      {"DW_OP_call_frame_cfa", 0x9C},
      {"DW_OP_bit_piece", 0x9D},
      {"DW_OP_implicit_value", 0x9E},
      {"DW_OP_stack_value", 0x9F},
      {"DW_OP_implicit_pointer", 0xA0},
      {"DW_OP_addrx", 0xA1},
      {"DW_OP_constx", 0xA2},
      {"DW_OP_entry_value", 0xA3},
      {"DW_OP_const_type", 0xA4},
      {"DW_OP_regval_type", 0xA5},
      {"DW_OP_deref_type", 0xA6},
      {"DW_OP_xderef_type", 0xA7},
      {"DW_OP_convert", 0xA8},
      {"DW_OP_reinterpret", 0xA9},
      {"DW_OP_LLVM_fragment", 0x1000},
      {"DW_OP_LLVM_convert", 0x1001},
      {"DW_OP_LLVM_tag_offset", 0x1002},
      {"DW_OP_LLVM_entry_value", 0x1003},
      {"DW_OP_LLVM_implicit_pointer", 0x1004},
      {"DW_OP_LLVM_arg", 0x1005},
  });
  const auto Found =
      std::ranges::find_if(NamedOpcodes, [Name](const auto &Entry) { return Entry.first == Name; });
  if (Found == NamedOpcodes.end())
    return std::nullopt;
  return Found->second;
}

} // namespace rcc::mir
