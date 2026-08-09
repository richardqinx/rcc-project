// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_TARGET_RISCV_RISCVTARGETCONSTANT_H
#define RCC_TARGET_RISCV_RISCVTARGETCONSTANT_H

#include "rcc/CodeGen/TargetConstant.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rcc::riscv {

enum class RISCVSymbolConstantKind : std::uint8_t {
  GlobalValue,
  ExternalSymbol,
};

class RISCVSymbolConstant final {
public:
  RISCVSymbolConstant(RISCVSymbolConstantKind Kind, std::string Symbol, std::int64_t Addend = 0)
      : ConstantKind(Kind), SymbolName(std::move(Symbol)), SymbolAddend(Addend) {}

  [[nodiscard]] static constexpr TargetConstantTypeID stableTypeID() noexcept {
    return TargetConstantTypeID::fromValue(0x5243435256435001ULL);
  }

  [[nodiscard]] static constexpr std::uint32_t schemaVersion() noexcept {
    return 1;
  }

  [[nodiscard]] Expected<void> verify() const;
  [[nodiscard]] Expected<RISCVSymbolConstant> clone() const;
  [[nodiscard]] std::uint64_t stableHash() const noexcept;
  [[nodiscard]] std::string print() const;
  [[nodiscard]] Expected<std::vector<std::byte>> encode() const;
  [[nodiscard]] Expected<TargetConstantEmission> emit() const;
  [[nodiscard]] Alignment requiredAlignment() const noexcept;
  [[nodiscard]] std::optional<TargetRelocationKindID> relocationKind() const noexcept;

  [[nodiscard]] static Expected<RISCVSymbolConstant> parse(std::string_view Text);
  [[nodiscard]] static Expected<RISCVSymbolConstant> decode(std::uint32_t Version,
                                                            std::span<const std::byte> Payload);

  [[nodiscard]] RISCVSymbolConstantKind kind() const noexcept {
    return ConstantKind;
  }

  [[nodiscard]] std::string_view symbol() const noexcept {
    return SymbolName;
  }

  [[nodiscard]] std::int64_t addend() const noexcept {
    return SymbolAddend;
  }

  friend bool operator==(const RISCVSymbolConstant &Left,
                         const RISCVSymbolConstant &Right) = default;

private:
  RISCVSymbolConstantKind ConstantKind;
  std::string SymbolName;
  std::int64_t SymbolAddend = 0;
};

} // namespace rcc::riscv

#endif // RCC_TARGET_RISCV_RISCVTARGETCONSTANT_H
