// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETMIRTEXTINFO_H
#define RCC_CODEGEN_TARGETMIRTEXTINFO_H

#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/CodeGen/TargetFunctionState.h"
#include "rcc/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

class TargetRegisterInfo;

enum class MIRTargetNodeKind : std::uint8_t {
  Null,
  Scalar,
  Sequence,
  Mapping,
  Count,
};

struct MIRTargetMappingEntry;

struct MIRTargetNode final {
  MIRTargetNode();
  MIRTargetNode(const MIRTargetNode &);
  MIRTargetNode &operator=(const MIRTargetNode &);
  MIRTargetNode(MIRTargetNode &&) noexcept;
  MIRTargetNode &operator=(MIRTargetNode &&) noexcept;
  ~MIRTargetNode();

  MIRTargetNodeKind Kind = MIRTargetNodeKind::Null;
  std::string Scalar;
  std::vector<MIRTargetNode> Sequence;
  std::vector<MIRTargetMappingEntry> Mapping;

  [[nodiscard]] static MIRTargetNode null();
  [[nodiscard]] static MIRTargetNode scalar(std::string Value);
  [[nodiscard]] static MIRTargetNode sequence(std::vector<MIRTargetNode> Values);
  [[nodiscard]] static MIRTargetNode mapping(std::vector<MIRTargetMappingEntry> Entries);

  [[nodiscard]] const MIRTargetNode *mappingValue(std::string_view Key) const noexcept;
  [[nodiscard]] Expected<void> verify() const;

  friend bool operator==(const MIRTargetNode &Left, const MIRTargetNode &Right);
};

struct MIRTargetMappingEntry final {
  std::string Key;
  MIRTargetNode Value;

  friend bool operator==(const MIRTargetMappingEntry &Left,
                         const MIRTargetMappingEntry &Right) = default;
};

using MIRTargetFunctionStateParser =
    Expected<TargetFunctionState> (*)(const MIRTargetNode &Node);
using MIRTargetFunctionStatePrinter =
    Expected<MIRTargetNode> (*)(const TargetFunctionState &State);
using MIRTargetConstantParser = Expected<TargetConstant> (*)(std::string_view Text);
using MIRTargetConstantPrinter = Expected<std::string> (*)(const TargetConstant &Constant);

struct MIRRegisterMaskDesc final {
  std::string Name;
  std::shared_ptr<const RegisterMask> Mask;
};

struct MIRTargetIndexDesc final {
  std::string Name;
  TargetIndex Index;
};

struct MIROperandFlagDesc final {
  std::string Name;
  std::uint32_t Value = 0;
};

struct MIRMachineMemOperandFlagDesc final {
  std::string Name;
  std::uint16_t Value = 0;
};

struct MIRVirtualRegisterFlagDesc final {
  std::string Name;
  std::uint8_t Value = 0;
};

struct MIRIntrinsicDesc final {
  std::string Name;
  IntrinsicID ID;
};

struct MIRCFIRegisterMappingDesc final {
  PhysicalRegister Register;
  std::uint32_t DwarfRegister = 0;
  bool IsCanonical = true;
};

struct TargetMIRTextInfoDesc final {
  std::vector<MIRRegisterMaskDesc> RegisterMasks;
  std::vector<MIRTargetIndexDesc> TargetIndices;
  std::uint32_t DirectOperandFlagMask = 0;
  std::vector<MIROperandFlagDesc> DirectOperandFlags;
  std::vector<MIROperandFlagDesc> BitmaskOperandFlags;
  std::vector<MIRMachineMemOperandFlagDesc> MachineMemOperandFlags;
  std::vector<MIRVirtualRegisterFlagDesc> VirtualRegisterFlags;
  std::vector<MIRIntrinsicDesc> Intrinsics;
  std::vector<MIRCFIRegisterMappingDesc> CFIRegisters;
  std::vector<StackID> SupportedStackIDs{
      StackID::defaultStack(),
      StackID::noAllocStack(),
  };
  std::optional<TargetFunctionStateTypeID> FunctionStateType;
  MIRTargetFunctionStateParser ParseFunctionState = nullptr;
  MIRTargetFunctionStatePrinter PrintFunctionState = nullptr;
  MIRTargetConstantParser ParseTargetConstant = nullptr;
  MIRTargetConstantPrinter PrintTargetConstant = nullptr;
};

class TargetMIRTextInfo final {
public:
  TargetMIRTextInfo(const TargetMIRTextInfo &) = delete;
  TargetMIRTextInfo &operator=(const TargetMIRTextInfo &) = delete;
  TargetMIRTextInfo(TargetMIRTextInfo &&) noexcept = default;
  TargetMIRTextInfo &operator=(TargetMIRTextInfo &&) noexcept = default;
  ~TargetMIRTextInfo() = default;

  [[nodiscard]] static Expected<TargetMIRTextInfo> create(TargetMIRTextInfoDesc Description,
                                                          const TargetRegisterInfo &Registers);

  [[nodiscard]] std::span<const MIRRegisterMaskDesc> registerMasks() const noexcept {
    return Description.RegisterMasks;
  }

  [[nodiscard]] std::span<const MIRTargetIndexDesc> targetIndices() const noexcept {
    return Description.TargetIndices;
  }

  [[nodiscard]] std::span<const MIROperandFlagDesc> directOperandFlags() const noexcept {
    return Description.DirectOperandFlags;
  }

  [[nodiscard]] std::span<const MIROperandFlagDesc> bitmaskOperandFlags() const noexcept {
    return Description.BitmaskOperandFlags;
  }

  [[nodiscard]] std::span<const MIRMachineMemOperandFlagDesc>
  machineMemOperandFlags() const noexcept {
    return Description.MachineMemOperandFlags;
  }

  [[nodiscard]] std::span<const MIRVirtualRegisterFlagDesc> virtualRegisterFlags() const noexcept {
    return Description.VirtualRegisterFlags;
  }

  [[nodiscard]] std::span<const MIRIntrinsicDesc> intrinsics() const noexcept {
    return Description.Intrinsics;
  }

  [[nodiscard]] std::shared_ptr<const RegisterMask>
  registerMask(std::string_view Name) const noexcept;
  [[nodiscard]] std::optional<std::string_view>
  registerMaskName(const RegisterMask &Mask) const noexcept;

  [[nodiscard]] std::optional<TargetIndex> targetIndex(std::string_view Name) const noexcept;
  [[nodiscard]] std::optional<std::string_view> targetIndexName(TargetIndex Index) const noexcept;

  [[nodiscard]] std::optional<std::uint32_t>
  directOperandFlag(std::string_view Name) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t>
  bitmaskOperandFlag(std::string_view Name) const noexcept;
  [[nodiscard]] Expected<std::vector<std::string_view>> operandFlagNames(std::uint32_t Flags) const;
  [[nodiscard]] Expected<void> validateOperandFlags(std::uint32_t Flags) const;

  [[nodiscard]] std::optional<std::uint16_t>
  machineMemOperandFlag(std::string_view Name) const noexcept;
  [[nodiscard]] Expected<std::vector<std::string_view>>
  machineMemOperandFlagNames(std::uint16_t Flags) const;
  [[nodiscard]] Expected<void> validateMachineMemOperandFlags(std::uint16_t Flags) const;

  [[nodiscard]] std::optional<std::uint8_t>
  virtualRegisterFlag(std::string_view Name) const noexcept;
  [[nodiscard]] Expected<std::vector<std::string_view>>
  virtualRegisterFlagNames(std::uint8_t Flags) const;
  [[nodiscard]] Expected<void> validateVirtualRegisterFlags(std::uint8_t Flags) const;

  [[nodiscard]] std::optional<IntrinsicID> intrinsic(std::string_view Name) const noexcept;
  [[nodiscard]] std::optional<std::string_view> intrinsicName(IntrinsicID ID) const noexcept;

  [[nodiscard]] std::optional<std::uint32_t>
  dwarfRegister(PhysicalRegister Register) const noexcept;
  [[nodiscard]] std::optional<PhysicalRegister>
  physicalRegister(std::uint32_t DwarfRegister) const noexcept;

  [[nodiscard]] std::span<const StackID> supportedStackIDs() const noexcept {
    return Description.SupportedStackIDs;
  }

  [[nodiscard]] bool supportsStackID(StackID ID) const noexcept {
    return SupportedStackIDs.contains(ID.value());
  }

  [[nodiscard]] std::uint32_t directOperandFlagMask() const noexcept {
    return Description.DirectOperandFlagMask;
  }

  [[nodiscard]] std::uint32_t serializableOperandFlagMask() const noexcept {
    return SerializableOperandFlagMask;
  }

  [[nodiscard]] std::uint16_t serializableMachineMemOperandFlagMask() const noexcept {
    return SerializableMachineMemOperandFlagMask;
  }

  [[nodiscard]] std::uint8_t serializableVirtualRegisterFlagMask() const noexcept {
    return SerializableVirtualRegisterFlagMask;
  }

  [[nodiscard]] std::optional<TargetFunctionStateTypeID> functionStateType() const noexcept {
    return Description.FunctionStateType;
  }

  [[nodiscard]] Expected<TargetFunctionState>
  parseFunctionState(const MIRTargetNode &Node) const;
  [[nodiscard]] Expected<MIRTargetNode>
  printFunctionState(const TargetFunctionState &State) const;
  [[nodiscard]] Expected<TargetConstant> parseTargetConstant(std::string_view Text) const;
  [[nodiscard]] Expected<std::string>
  printTargetConstant(const TargetConstant &Constant) const;

private:
  explicit TargetMIRTextInfo(TargetMIRTextInfoDesc Description)
      : Description(std::move(Description)) {}

  TargetMIRTextInfoDesc Description;
  std::unordered_map<std::string, std::size_t> RegisterMaskNameToIndex;
  std::unordered_map<std::string, std::size_t> TargetIndexNameToIndex;
  std::unordered_map<TargetIndex, std::size_t> TargetIndexToIndex;
  std::unordered_map<std::string, std::size_t> DirectOperandFlagNameToIndex;
  std::unordered_map<std::uint32_t, std::size_t> DirectOperandFlagToIndex;
  std::unordered_map<std::string, std::size_t> BitmaskOperandFlagNameToIndex;
  std::unordered_map<std::string, std::size_t> MachineMemOperandFlagNameToIndex;
  std::unordered_map<std::string, std::size_t> VirtualRegisterFlagNameToIndex;
  std::unordered_map<std::string, std::size_t> IntrinsicNameToIndex;
  std::unordered_map<IntrinsicID, std::size_t> IntrinsicToIndex;
  std::unordered_map<PhysicalRegister, std::uint32_t> RegisterToDwarf;
  std::unordered_map<std::uint32_t, PhysicalRegister> DwarfToRegister;
  std::unordered_set<std::uint32_t> SupportedStackIDs;
  std::uint32_t SerializableOperandFlagMask = 0;
  std::uint16_t SerializableMachineMemOperandFlagMask = 0;
  std::uint8_t SerializableVirtualRegisterFlagMask = 0;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETMIRTEXTINFO_H
