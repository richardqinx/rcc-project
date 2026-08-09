// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_MC_ELFOBJECTWRITER_H
#define RCC_MC_ELFOBJECTWRITER_H

#include "rcc/MC/MCObjectWriter.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rcc::mc {

struct ELFExtraSection final {
  std::string Name;
  std::uint32_t Type = 1;
  std::uint64_t Flags = 0;
  std::uint64_t Alignment = 1;
  std::uint64_t EntrySize = 0;
  std::vector<std::uint8_t> Contents;
};

struct ELFObjectTargetInfo final {
  std::uint16_t Machine = 0;
  std::uint8_t OSABI = 0;
  std::uint8_t ABIVersion = 0;
  std::uint32_t Flags = 0;
  std::vector<ELFExtraSection> ExtraSections;
};

class ELF64LEObjectWriter final : public MCObjectWriter {
public:
  explicit ELF64LEObjectWriter(ELFObjectTargetInfo Target) : Target(std::move(Target)) {}

  [[nodiscard]] Expected<std::vector<std::uint8_t>>
  writeObject(const MCAssembledObject &Object) const override;

private:
  ELFObjectTargetInfo Target;
};

} // namespace rcc::mc

#endif // RCC_MC_ELFOBJECTWRITER_H
