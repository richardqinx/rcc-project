// SPDX-License-Identifier: GPL-3.0-or-later

// NOLINTNEXTLINE(llvm-header-guard)
#ifndef RCC_LIB_MIR_MIRPARSERUTILITIES_H
#define RCC_LIB_MIR_MIRPARSERUTILITIES_H

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace rcc::mir::parser_detail {

[[nodiscard]] inline std::string_view trim(std::string_view Text) noexcept {
  while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r' ||
                           Text.front() == '\n')) {
    Text.remove_prefix(1);
  }
  while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r' ||
                           Text.back() == '\n')) {
    Text.remove_suffix(1);
  }
  return Text;
}

[[nodiscard]] inline bool parseUnsigned(std::string_view Text, std::uint64_t &Value) noexcept {
  Text = trim(Text);
  int Base = 10;
  if (Text.starts_with("0x") || Text.starts_with("0X")) {
    Text.remove_prefix(2);
    Base = 16;
  }
  if (Text.empty())
    return false;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, Base);
  return Error == std::errc{} && End == Text.data() + Text.size();
}

[[nodiscard]] inline bool parseSigned(std::string_view Text, std::int64_t &Value) noexcept {
  Text = trim(Text);
  if (Text.empty())
    return false;
  const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), Value, 10);
  return Error == std::errc{} && End == Text.data() + Text.size();
}

[[nodiscard]] inline bool parseUnsigned32(std::string_view Text, std::uint32_t &Value) noexcept {
  std::uint64_t Wide = 0;
  if (!parseUnsigned(Text, Wide) || Wide > std::numeric_limits<std::uint32_t>::max())
    return false;
  Value = static_cast<std::uint32_t>(Wide);
  return true;
}

[[nodiscard]] inline bool parseBool(std::string_view Text, bool &Value) noexcept {
  if (Text == "true") {
    Value = true;
    return true;
  }
  if (Text == "false") {
    Value = false;
    return true;
  }
  return false;
}

[[nodiscard]] inline std::optional<std::uint32_t>
parsePrefixedID(std::string_view Text, std::string_view Prefix) noexcept {
  Text = trim(Text);
  if (!Text.starts_with(Prefix))
    return std::nullopt;
  Text.remove_prefix(Prefix.size());
  const std::size_t Suffix = Text.find_first_of(". \t");
  if (Suffix != std::string_view::npos)
    Text = Text.substr(0, Suffix);
  std::uint32_t ID = 0;
  if (!parseUnsigned32(Text, ID))
    return std::nullopt;
  return ID;
}

} // namespace rcc::mir::parser_detail

#endif // RCC_LIB_MIR_MIRPARSERUTILITIES_H
