// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_REGISTER_H
#define RCC_CODEGEN_REGISTER_H

#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>

namespace rcc {

class PhysicalRegister final {
public:
  constexpr PhysicalRegister() noexcept = default;

  [[nodiscard]] static Expected<PhysicalRegister> create(std::uint64_t Number);

  [[nodiscard]] constexpr bool isValid() const noexcept {
    return Number != 0;
  }

  [[nodiscard]] constexpr std::uint32_t number() const noexcept {
    return Number;
  }

  friend constexpr bool operator==(PhysicalRegister Left,
                                   PhysicalRegister Right) noexcept = default;

private:
  explicit constexpr PhysicalRegister(std::uint32_t Number) noexcept : Number(Number) {}

  std::uint32_t Number = 0;

  friend class Register;
};

class Register final {
public:
  constexpr Register() noexcept = default;
  explicit constexpr Register(std::uint32_t OpaqueValue) noexcept : OpaqueValue(OpaqueValue) {}

  [[nodiscard]] static Expected<Register> makePhysical(std::uint64_t Number);
  [[nodiscard]] static Expected<Register> makeVirtual(std::uint64_t Index);

  [[nodiscard]] static constexpr Register fromPhysical(PhysicalRegister PhysReg) noexcept {
    return Register(PhysReg.Number);
  }

  [[nodiscard]] constexpr bool isNoRegister() const noexcept {
    return OpaqueValue == 0;
  }

  [[nodiscard]] constexpr bool isPhysical() const noexcept {
    return OpaqueValue != 0 && (OpaqueValue & VirtualBit) == 0;
  }

  [[nodiscard]] constexpr bool isVirtual() const noexcept {
    return (OpaqueValue & VirtualBit) != 0;
  }

  [[nodiscard]] std::optional<PhysicalRegister> physicalRegister() const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> virtualIndex() const noexcept;

  [[nodiscard]] constexpr std::uint32_t opaqueValue() const noexcept {
    return OpaqueValue;
  }

  [[nodiscard]] std::string toString() const;

  friend constexpr bool operator==(Register Left, Register Right) noexcept = default;

private:
  static constexpr std::uint32_t VirtualBit = std::uint32_t{1} << 31;
  static constexpr std::uint32_t IndexMask = VirtualBit - 1;

  std::uint32_t OpaqueValue = 0;
};

static_assert(sizeof(PhysicalRegister) == sizeof(std::uint32_t));
static_assert(sizeof(Register) == sizeof(std::uint32_t));
static_assert(std::is_trivially_copyable_v<PhysicalRegister>);
static_assert(std::is_trivially_copyable_v<Register>);

} // namespace rcc

template <> struct std::hash<rcc::PhysicalRegister> {
  [[nodiscard]] std::size_t operator()(rcc::PhysicalRegister PhysReg) const noexcept {
    return std::hash<std::uint32_t>{}(PhysReg.number());
  }
};

template <> struct std::hash<rcc::Register> {
  [[nodiscard]] std::size_t operator()(rcc::Register Reg) const noexcept {
    return std::hash<std::uint32_t>{}(Reg.opaqueValue());
  }
};

#endif // RCC_CODEGEN_REGISTER_H
