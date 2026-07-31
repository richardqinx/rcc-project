// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/RegisterMask.h"

namespace rcc {

bool RegisterMask::preserves(PhysicalRegister PhysReg) const noexcept {
  if (!PhysReg.isValid())
    return false;

  constexpr std::uint32_t BitsPerWord = 64;
  const std::uint32_t RegisterNumber = PhysReg.number();
  const std::size_t WordIndex = RegisterNumber / BitsPerWord;
  if (WordIndex >= PreservedWords.size())
    return false;

  const std::uint32_t BitIndex = RegisterNumber % BitsPerWord;
  return (PreservedWords[WordIndex] & (std::uint64_t{1} << BitIndex)) != 0;
}

} // namespace rcc
