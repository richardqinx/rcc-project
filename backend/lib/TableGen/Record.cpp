// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/TableGen/Record.h"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace rcc::targetgen {

[[nodiscard]] std::string lowerASCII(std::string_view Value) {
  std::string Result(Value);
  for (char &Character : Result) {
    if (Character >= 'A' && Character <= 'Z')
      Character = static_cast<char>(Character - 'A' + 'a');
  }
  return Result;
}

[[nodiscard]] bool isIdentifier(std::string_view Value) noexcept {
  if (Value.empty())
    return false;
  const auto IsHead = [](unsigned char Character) {
    return std::isalpha(Character) != 0 || Character == '_';
  };
  const auto IsTail = [](unsigned char Character) {
    return std::isalnum(Character) != 0 || Character == '_';
  };
  if (!IsHead(static_cast<unsigned char>(Value.front())))
    return false;
  return std::ranges::all_of(Value.substr(1), [IsTail](char Character) {
    return IsTail(static_cast<unsigned char>(Character));
  });
}

[[nodiscard]] bool isAssemblyMnemonic(std::string_view Value) noexcept {
  if (Value.empty())
    return false;
  if (std::islower(static_cast<unsigned char>(Value.front())) == 0)
    return false;
  if (std::islower(static_cast<unsigned char>(Value.back())) == 0 &&
      std::isdigit(static_cast<unsigned char>(Value.back())) == 0)
    return false;
  return std::ranges::all_of(Value, [](char Character) {
    const unsigned char Code = static_cast<unsigned char>(Character);
    return std::islower(Code) != 0 || std::isdigit(Code) != 0 || Character == '.' ||
           Character == '_';
  });
}

[[nodiscard]] std::string upperIdentifier(std::string_view Value) {
  std::string Result;
  Result.reserve(Value.size());
  for (const char Character : Value) {
    if (Character >= 'a' && Character <= 'z')
      Result.push_back(static_cast<char>(Character - 'a' + 'A'));
    else if ((Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9'))
      Result.push_back(Character);
    else
      Result.push_back('_');
  }
  return Result;
}

void appendQuoted(std::ostream &Output, std::string_view Value) {
  Output << '"';
  for (const char Character : Value) {
    switch (Character) {
    case '\\':
      Output << "\\\\";
      break;
    case '"':
      Output << "\\\"";
      break;
    case '\n':
      Output << "\\n";
      break;
    case '\r':
      Output << "\\r";
      break;
    case '\t':
      Output << "\\t";
      break;
    default:
      Output << Character;
      break;
    }
  }
  Output << '"';
}

} // namespace rcc::targetgen
