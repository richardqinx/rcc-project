// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_MACHINEBASICBLOCKATTRIBUTES_H
#define RCC_CODEGEN_MACHINEBASICBLOCKATTRIBUTES_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rcc {

enum class MachineBasicBlockAttribute : std::uint8_t {
  MachineBlockAddressTaken,
  IRBlockAddressTaken,
  LandingPad,
  InlineAsmBrIndirectTarget,
  EHFuncletEntry,
  EHScopeEntry,
  Count,
};

[[nodiscard]] std::string_view
getMachineBasicBlockAttributeName(MachineBasicBlockAttribute Attribute) noexcept;

enum class MachineBasicBlockSectionKind : std::uint8_t {
  Numbered,
  Exception,
  Cold,
};

class MachineBasicBlockSectionID final {
public:
  constexpr MachineBasicBlockSectionID() noexcept = default;

  [[nodiscard]] static constexpr MachineBasicBlockSectionID
  numbered(std::uint32_t Number) noexcept {
    return MachineBasicBlockSectionID(MachineBasicBlockSectionKind::Numbered, Number);
  }

  [[nodiscard]] static constexpr MachineBasicBlockSectionID exception() noexcept {
    return MachineBasicBlockSectionID(MachineBasicBlockSectionKind::Exception, 0);
  }

  [[nodiscard]] static constexpr MachineBasicBlockSectionID cold() noexcept {
    return MachineBasicBlockSectionID(MachineBasicBlockSectionKind::Cold, 0);
  }

  [[nodiscard]] constexpr MachineBasicBlockSectionKind kind() const noexcept {
    return Kind;
  }

  [[nodiscard]] constexpr std::uint32_t number() const noexcept {
    return Number;
  }

  [[nodiscard]] constexpr bool isDefault() const noexcept {
    return Kind == MachineBasicBlockSectionKind::Numbered && Number == 0;
  }

  friend constexpr bool operator==(MachineBasicBlockSectionID Left,
                                   MachineBasicBlockSectionID Right) noexcept = default;

private:
  constexpr MachineBasicBlockSectionID(MachineBasicBlockSectionKind Kind,
                                       std::uint32_t Number) noexcept
      : Kind(Kind), Number(Number) {}

  MachineBasicBlockSectionKind Kind = MachineBasicBlockSectionKind::Numbered;
  std::uint32_t Number = 0;
};

struct MachineBasicBlockProfileID final {
  std::uint32_t BaseID = 0;
  std::uint32_t CloneID = 0;

  friend constexpr bool operator==(MachineBasicBlockProfileID Left,
                                   MachineBasicBlockProfileID Right) noexcept = default;
};

class MachineBasicBlockAttributes final {
public:
  MachineBasicBlockAttributes() = default;

  [[nodiscard]] static Expected<MachineBasicBlockAttributes> fromRawFlags(std::uint8_t RawFlags);

  [[nodiscard]] bool has(MachineBasicBlockAttribute Attribute) const noexcept {
    if (!isValidAttribute(Attribute))
      return false;
    return (Flags & flagFor(Attribute)) != 0;
  }

  void set(MachineBasicBlockAttribute Attribute) noexcept {
    if (isValidAttribute(Attribute))
      Flags |= flagFor(Attribute);
  }

  void clear(MachineBasicBlockAttribute Attribute) noexcept {
    if (isValidAttribute(Attribute))
      Flags &= static_cast<std::uint8_t>(~flagFor(Attribute));
  }

  [[nodiscard]] std::uint8_t rawFlags() const noexcept {
    return Flags;
  }

  [[nodiscard]] std::optional<std::string_view> name() const noexcept {
    if (!Name)
      return std::nullopt;
    return *Name;
  }

  [[nodiscard]] Expected<void> setName(std::string NewName);

  void clearName() noexcept {
    Name.reset();
  }

  [[nodiscard]] std::optional<std::string_view> irBlockName() const noexcept {
    if (!IRBlockName)
      return std::nullopt;
    return *IRBlockName;
  }

  [[nodiscard]] Expected<void> setIRBlockName(std::string NewName);

  void clearIRBlockName() noexcept {
    IRBlockName.reset();
  }

  [[nodiscard]] Alignment alignment() const noexcept {
    return BlockAlignment;
  }

  void setAlignment(Alignment NewAlignment) noexcept {
    BlockAlignment = NewAlignment;
  }

  void clearAlignment() noexcept {
    BlockAlignment = Alignment{};
  }

  [[nodiscard]] MachineBasicBlockSectionID sectionID() const noexcept {
    return SectionID;
  }

  void setSectionID(MachineBasicBlockSectionID NewSectionID) noexcept {
    SectionID = NewSectionID;
  }

  [[nodiscard]] const std::optional<MachineBasicBlockProfileID> &profileID() const noexcept {
    return ProfileID;
  }

  void setProfileID(MachineBasicBlockProfileID NewProfileID) noexcept {
    ProfileID = NewProfileID;
  }

  void clearProfileID() noexcept {
    ProfileID.reset();
  }

  [[nodiscard]] std::uint32_t callFrameSize() const noexcept {
    return CallFrameSize;
  }

  void setCallFrameSize(std::uint32_t Size) noexcept {
    CallFrameSize = Size;
  }

  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const MachineBasicBlockAttributes &Left,
                         const MachineBasicBlockAttributes &Right) = default;

private:
  static constexpr std::uint8_t AttributeCount =
      static_cast<std::uint8_t>(MachineBasicBlockAttribute::Count);
  static_assert(AttributeCount < 8);

  static constexpr std::uint8_t ValidFlags =
      static_cast<std::uint8_t>((std::uint8_t{1} << AttributeCount) - 1);

  [[nodiscard]] static constexpr bool
  isValidAttribute(MachineBasicBlockAttribute Attribute) noexcept {
    return static_cast<std::uint8_t>(Attribute) < AttributeCount;
  }

  [[nodiscard]] static constexpr std::uint8_t
  flagFor(MachineBasicBlockAttribute Attribute) noexcept {
    return static_cast<std::uint8_t>(std::uint8_t{1} << static_cast<std::uint8_t>(Attribute));
  }

  std::uint8_t Flags = 0;
  std::optional<std::string> Name;
  std::optional<std::string> IRBlockName;
  Alignment BlockAlignment;
  MachineBasicBlockSectionID SectionID;
  std::optional<MachineBasicBlockProfileID> ProfileID;
  std::uint32_t CallFrameSize = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_MACHINEBASICBLOCKATTRIBUTES_H
