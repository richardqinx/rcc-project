// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGISTERMASK_H
#define RCC_CODEGEN_REGISTERMASK_H

#include "rcc/CodeGen/Register.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace rcc {

class RegisterMask final {
public:
  explicit RegisterMask(std::vector<std::uint64_t> PreservedWords)
      : PreservedWords(std::move(PreservedWords)) {}

  [[nodiscard]] bool preserves(PhysicalRegister PhysReg) const noexcept;

  [[nodiscard]] bool clobbers(PhysicalRegister PhysReg) const noexcept {
    return !preserves(PhysReg);
  }

  [[nodiscard]] std::span<const std::uint64_t> words() const noexcept {
    return PreservedWords;
  }

  friend bool operator==(const RegisterMask &Left, const RegisterMask &Right) = default;

private:
  std::vector<std::uint64_t> PreservedWords;
};

} // namespace rcc

#endif // RCC_CODEGEN_REGISTERMASK_H
