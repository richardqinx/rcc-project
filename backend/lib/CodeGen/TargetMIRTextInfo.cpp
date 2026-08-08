// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetMIRTextInfo.h"

#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::unexpected<Error> invalidDescription(std::string Message) {
  return makeUnexpected(ErrorCode::InvalidArgument,
                        "target MIR text description: " + std::move(Message));
}

[[nodiscard]] bool isASCIIAlpha(char Character) noexcept {
  return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z');
}

[[nodiscard]] bool isASCIIDigit(char Character) noexcept {
  return Character >= '0' && Character <= '9';
}

[[nodiscard]] bool isMIRIdentifier(std::string_view Name) noexcept {
  if (Name.empty() || (!isASCIIAlpha(Name.front()) && Name.front() != '_'))
    return false;
  for (const char Character : Name) {
    if (!isASCIIAlpha(Character) && !isASCIIDigit(Character) && Character != '_' &&
        Character != '-' && Character != '.' && Character != '$') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string lowerASCII(std::string_view Name) {
  std::string Result(Name);
  for (char &Character : Result) {
    if (Character >= 'A' && Character <= 'Z')
      Character = static_cast<char>(Character - 'A' + 'a');
  }
  return Result;
}

} // namespace

MIRTargetNode::MIRTargetNode() = default;
MIRTargetNode::MIRTargetNode(const MIRTargetNode &) = default;
MIRTargetNode &MIRTargetNode::operator=(const MIRTargetNode &) = default;
MIRTargetNode::MIRTargetNode(MIRTargetNode &&) noexcept = default;
MIRTargetNode &MIRTargetNode::operator=(MIRTargetNode &&) noexcept = default;
MIRTargetNode::~MIRTargetNode() = default;

MIRTargetNode MIRTargetNode::null() {
  return {};
}

MIRTargetNode MIRTargetNode::scalar(std::string Value) {
  MIRTargetNode Result;
  Result.Kind = MIRTargetNodeKind::Scalar;
  Result.Scalar = std::move(Value);
  return Result;
}

MIRTargetNode MIRTargetNode::sequence(std::vector<MIRTargetNode> Values) {
  MIRTargetNode Result;
  Result.Kind = MIRTargetNodeKind::Sequence;
  Result.Sequence = std::move(Values);
  return Result;
}

MIRTargetNode MIRTargetNode::mapping(std::vector<MIRTargetMappingEntry> Entries) {
  MIRTargetNode Result;
  Result.Kind = MIRTargetNodeKind::Mapping;
  Result.Mapping = std::move(Entries);
  return Result;
}

bool operator==(const MIRTargetNode &Left, const MIRTargetNode &Right) {
  return Left.Kind == Right.Kind && Left.Scalar == Right.Scalar &&
         Left.Sequence == Right.Sequence && Left.Mapping == Right.Mapping;
}

const MIRTargetNode *MIRTargetNode::mappingValue(std::string_view Key) const noexcept {
  if (Kind != MIRTargetNodeKind::Mapping)
    return nullptr;
  for (const MIRTargetMappingEntry &Entry : Mapping) {
    if (Entry.Key == Key)
      return &Entry.Value;
  }
  return nullptr;
}

Expected<void> MIRTargetNode::verify() const {
  if (Kind >= MIRTargetNodeKind::Count)
    return makeUnexpected(ErrorCode::InvalidArgument, "target MIR node kind is invalid");

  const bool HasScalar = !Scalar.empty();
  const bool HasSequence = !Sequence.empty();
  const bool HasMapping = !Mapping.empty();
  if ((Kind != MIRTargetNodeKind::Scalar && HasScalar) ||
      (Kind != MIRTargetNodeKind::Sequence && HasSequence) ||
      (Kind != MIRTargetNodeKind::Mapping && HasMapping)) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target MIR node contains storage for a different node kind");
  }

  if (Kind == MIRTargetNodeKind::Sequence) {
    for (const MIRTargetNode &Element : Sequence) {
      auto ElementResult = Element.verify();
      if (!ElementResult)
        return ElementResult;
    }
  }
  if (Kind == MIRTargetNodeKind::Mapping) {
    std::unordered_set<std::string_view> Keys;
    for (const MIRTargetMappingEntry &Entry : Mapping) {
      if (Entry.Key.empty() || !Keys.insert(Entry.Key).second) {
        return makeUnexpected(ErrorCode::InvalidArgument,
                              "target MIR mapping keys must be nonempty and unique");
      }
      auto ValueResult = Entry.Value.verify();
      if (!ValueResult)
        return ValueResult;
    }
  }
  return {};
}

Expected<TargetMIRTextInfo> TargetMIRTextInfo::create(TargetMIRTextInfoDesc Description,
                                                      const TargetRegisterInfo &Registers) {
  TargetMIRTextInfo Result(std::move(Description));

  for (std::size_t Index = 0; Index < Result.Description.RegisterMasks.size(); ++Index) {
    MIRRegisterMaskDesc &Mask = Result.Description.RegisterMasks[Index];
    Mask.Name = lowerASCII(Mask.Name);
    if (!isMIRIdentifier(Mask.Name) || Mask.Mask == nullptr) {
      return invalidDescription("named register masks require an identifier and nonnull mask");
    }
    auto Validation = Registers.validateRegisterMask(*Mask.Mask);
    if (!Validation)
      return invalidDescription("named register mask contains an unknown physical register");
    if (!Result.RegisterMaskNameToIndex.emplace(Mask.Name, Index).second) {
      return invalidDescription("register mask names must be unique case-insensitively");
    }
    for (std::size_t Previous = 0; Previous < Index; ++Previous) {
      if (*Result.Description.RegisterMasks[Previous].Mask == *Mask.Mask) {
        return invalidDescription("named register mask values must have one canonical name");
      }
    }
  }

  for (std::size_t Index = 0; Index < Result.Description.TargetIndices.size(); ++Index) {
    const MIRTargetIndexDesc &TargetIndex = Result.Description.TargetIndices[Index];
    if (!isMIRIdentifier(TargetIndex.Name)) {
      return invalidDescription("target index names must be MIR identifiers");
    }
    if (!Result.TargetIndexNameToIndex.emplace(TargetIndex.Name, Index).second ||
        !Result.TargetIndexToIndex.emplace(TargetIndex.Index, Index).second) {
      return invalidDescription("target index names and values must be unique");
    }
  }

  if (Result.Description.DirectOperandFlags.empty() !=
      (Result.Description.DirectOperandFlagMask == 0)) {
    return invalidDescription(
        "direct operand flags and the direct operand flag mask must be present together");
  }

  std::unordered_set<std::string> OperandFlagNames;
  for (std::size_t Index = 0; Index < Result.Description.DirectOperandFlags.size(); ++Index) {
    const MIROperandFlagDesc &Flag = Result.Description.DirectOperandFlags[Index];
    if (!isMIRIdentifier(Flag.Name) || Flag.Value == 0 ||
        (Flag.Value & ~Result.Description.DirectOperandFlagMask) != 0) {
      return invalidDescription(
          "direct operand flags require an identifier and a nonzero value inside the direct mask");
    }
    if (!OperandFlagNames.insert(Flag.Name).second ||
        !Result.DirectOperandFlagNameToIndex.emplace(Flag.Name, Index).second ||
        !Result.DirectOperandFlagToIndex.emplace(Flag.Value, Index).second) {
      return invalidDescription("direct operand flag names and values must be unique");
    }
  }

  std::uint32_t BitmaskOperandFlags = 0;
  for (std::size_t Index = 0; Index < Result.Description.BitmaskOperandFlags.size(); ++Index) {
    const MIROperandFlagDesc &Flag = Result.Description.BitmaskOperandFlags[Index];
    if (!isMIRIdentifier(Flag.Name) || !std::has_single_bit(Flag.Value) ||
        (Flag.Value & Result.Description.DirectOperandFlagMask) != 0) {
      return invalidDescription(
          "bitmask operand flags require an identifier and one bit outside the direct mask");
    }
    if (!OperandFlagNames.insert(Flag.Name).second ||
        !Result.BitmaskOperandFlagNameToIndex.emplace(Flag.Name, Index).second ||
        (BitmaskOperandFlags & Flag.Value) != 0) {
      return invalidDescription("operand flag names and bitmask values must be unique");
    }
    BitmaskOperandFlags |= Flag.Value;
  }
  Result.SerializableOperandFlagMask =
      Result.Description.DirectOperandFlagMask | BitmaskOperandFlags;

  for (std::size_t Index = 0; Index < Result.Description.MachineMemOperandFlags.size(); ++Index) {
    const MIRMachineMemOperandFlagDesc &Flag = Result.Description.MachineMemOperandFlags[Index];
    if (!isMIRIdentifier(Flag.Name) || !std::has_single_bit(Flag.Value)) {
      return invalidDescription(
          "machine memory operand flags require an identifier and a single-bit value");
    }
    if (!Result.MachineMemOperandFlagNameToIndex.emplace(Flag.Name, Index).second ||
        (Result.SerializableMachineMemOperandFlagMask & Flag.Value) != 0) {
      return invalidDescription("machine memory operand flag names and values must be unique");
    }
    Result.SerializableMachineMemOperandFlagMask |= Flag.Value;
  }

  for (std::size_t Index = 0; Index < Result.Description.VirtualRegisterFlags.size(); ++Index) {
    const MIRVirtualRegisterFlagDesc &Flag = Result.Description.VirtualRegisterFlags[Index];
    if (!isMIRIdentifier(Flag.Name) || !std::has_single_bit(Flag.Value)) {
      return invalidDescription(
          "virtual register flags require an identifier and a single-bit value");
    }
    if (!Result.VirtualRegisterFlagNameToIndex.emplace(Flag.Name, Index).second ||
        (Result.SerializableVirtualRegisterFlagMask & Flag.Value) != 0) {
      return invalidDescription("virtual register flag names and values must be unique");
    }
    Result.SerializableVirtualRegisterFlagMask |= Flag.Value;
  }

  for (std::size_t Index = 0; Index < Result.Description.Intrinsics.size(); ++Index) {
    const MIRIntrinsicDesc &Intrinsic = Result.Description.Intrinsics[Index];
    if (!isMIRIdentifier(Intrinsic.Name) || !Intrinsic.Name.starts_with("llvm.") ||
        Intrinsic.ID.value() == 0) {
      return invalidDescription("intrinsics require an llvm-prefixed MIR name and a nonzero ID");
    }
    if (!Result.IntrinsicNameToIndex.emplace(Intrinsic.Name, Index).second ||
        !Result.IntrinsicToIndex.emplace(Intrinsic.ID, Index).second) {
      return invalidDescription("intrinsic names and IDs must be unique");
    }
  }

  std::unordered_set<std::uint32_t> ReferencedDwarfRegisters;
  for (const MIRCFIRegisterMappingDesc &Mapping : Result.Description.CFIRegisters) {
    if (!Registers.isKnownPhysicalRegister(Mapping.Register) ||
        !Result.RegisterToDwarf.emplace(Mapping.Register, Mapping.DwarfRegister).second) {
      return invalidDescription("CFI register mappings require unique, known physical registers");
    }
    ReferencedDwarfRegisters.insert(Mapping.DwarfRegister);
    if (Mapping.IsCanonical &&
        !Result.DwarfToRegister.emplace(Mapping.DwarfRegister, Mapping.Register).second) {
      return invalidDescription(
          "each DWARF register must have exactly one canonical physical register");
    }
  }
  if (ReferencedDwarfRegisters.size() != Result.DwarfToRegister.size()) {
    return invalidDescription(
        "every referenced DWARF register requires a canonical physical register");
  }

  for (const StackID ID : Result.Description.SupportedStackIDs) {
    if (!ID.isValid() || !Result.SupportedStackIDs.insert(ID.value()).second)
      return invalidDescription("supported stack IDs must be valid and unique");
  }
  if (!Result.SupportedStackIDs.contains(StackID::defaultStack().value()) ||
      !Result.SupportedStackIDs.contains(StackID::noAllocStack().value())) {
    return invalidDescription("targets must support the default and noalloc stack IDs");
  }

  if (Result.Description.FunctionStateType && Result.Description.FunctionStateType->value() == 0) {
    return invalidDescription("target function state TypeID must be nonzero");
  }
  const bool HasStateType = Result.Description.FunctionStateType.has_value();
  const bool HasParser = Result.Description.ParseFunctionState != nullptr;
  const bool HasPrinter = Result.Description.PrintFunctionState != nullptr;
  if (HasStateType != HasParser || HasStateType != HasPrinter) {
    return invalidDescription(
        "target function state TypeID, parser, and printer must be provided together");
  }
  const bool HasConstantParser = Result.Description.ParseTargetConstant != nullptr;
  const bool HasConstantPrinter = Result.Description.PrintTargetConstant != nullptr;
  if (HasConstantParser != HasConstantPrinter) {
    return invalidDescription("target constant parser and printer must be provided together");
  }

  return Result;
}

std::shared_ptr<const RegisterMask>
TargetMIRTextInfo::registerMask(std::string_view Name) const noexcept {
  const auto Position = RegisterMaskNameToIndex.find(std::string(Name));
  if (Position == RegisterMaskNameToIndex.end())
    return {};
  return Description.RegisterMasks[Position->second].Mask;
}

std::optional<std::string_view>
TargetMIRTextInfo::registerMaskName(const RegisterMask &Mask) const noexcept {
  for (const MIRRegisterMaskDesc &NamedMask : Description.RegisterMasks) {
    if (*NamedMask.Mask == Mask)
      return NamedMask.Name;
  }
  return std::nullopt;
}

std::optional<TargetIndex> TargetMIRTextInfo::targetIndex(std::string_view Name) const noexcept {
  const auto Position = TargetIndexNameToIndex.find(std::string(Name));
  if (Position == TargetIndexNameToIndex.end())
    return std::nullopt;
  return Description.TargetIndices[Position->second].Index;
}

std::optional<std::string_view>
TargetMIRTextInfo::targetIndexName(TargetIndex Index) const noexcept {
  const auto Position = TargetIndexToIndex.find(Index);
  if (Position == TargetIndexToIndex.end())
    return std::nullopt;
  return Description.TargetIndices[Position->second].Name;
}

std::optional<std::uint32_t>
TargetMIRTextInfo::directOperandFlag(std::string_view Name) const noexcept {
  const auto Position = DirectOperandFlagNameToIndex.find(std::string(Name));
  if (Position == DirectOperandFlagNameToIndex.end())
    return std::nullopt;
  return Description.DirectOperandFlags[Position->second].Value;
}

std::optional<std::uint32_t>
TargetMIRTextInfo::bitmaskOperandFlag(std::string_view Name) const noexcept {
  const auto Position = BitmaskOperandFlagNameToIndex.find(std::string(Name));
  if (Position == BitmaskOperandFlagNameToIndex.end())
    return std::nullopt;
  return Description.BitmaskOperandFlags[Position->second].Value;
}

Expected<void> TargetMIRTextInfo::validateOperandFlags(std::uint32_t Flags) const {
  if ((Flags & ~SerializableOperandFlagMask) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine operand contains an unknown target flag bit");
  }
  const std::uint32_t DirectFlag = Flags & Description.DirectOperandFlagMask;
  if (DirectFlag != 0 && !DirectOperandFlagToIndex.contains(DirectFlag)) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine operand contains an unregistered direct target flag");
  }
  return {};
}

Expected<std::vector<std::string_view>>
TargetMIRTextInfo::operandFlagNames(std::uint32_t Flags) const {
  auto Validation = validateOperandFlags(Flags);
  if (!Validation)
    return std::unexpected(Validation.error());

  std::vector<std::string_view> Names;
  const std::uint32_t DirectFlag = Flags & Description.DirectOperandFlagMask;
  if (DirectFlag != 0) {
    Names.push_back(Description.DirectOperandFlags[DirectOperandFlagToIndex.at(DirectFlag)].Name);
  }
  for (const MIROperandFlagDesc &Flag : Description.BitmaskOperandFlags) {
    if ((Flags & Flag.Value) != 0)
      Names.push_back(Flag.Name);
  }
  return Names;
}

std::optional<std::uint16_t>
TargetMIRTextInfo::machineMemOperandFlag(std::string_view Name) const noexcept {
  const auto Position = MachineMemOperandFlagNameToIndex.find(std::string(Name));
  if (Position == MachineMemOperandFlagNameToIndex.end())
    return std::nullopt;
  return Description.MachineMemOperandFlags[Position->second].Value;
}

Expected<void> TargetMIRTextInfo::validateMachineMemOperandFlags(std::uint16_t Flags) const {
  if ((Flags & static_cast<std::uint16_t>(~SerializableMachineMemOperandFlagMask)) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "machine memory operand contains an unknown target flag");
  }
  return {};
}

Expected<std::vector<std::string_view>>
TargetMIRTextInfo::machineMemOperandFlagNames(std::uint16_t Flags) const {
  auto Validation = validateMachineMemOperandFlags(Flags);
  if (!Validation)
    return std::unexpected(Validation.error());

  std::vector<std::string_view> Names;
  for (const MIRMachineMemOperandFlagDesc &Flag : Description.MachineMemOperandFlags) {
    if ((Flags & Flag.Value) != 0)
      Names.push_back(Flag.Name);
  }
  return Names;
}

std::optional<std::uint8_t>
TargetMIRTextInfo::virtualRegisterFlag(std::string_view Name) const noexcept {
  const auto Position = VirtualRegisterFlagNameToIndex.find(std::string(Name));
  if (Position == VirtualRegisterFlagNameToIndex.end())
    return std::nullopt;
  return Description.VirtualRegisterFlags[Position->second].Value;
}

Expected<void> TargetMIRTextInfo::validateVirtualRegisterFlags(std::uint8_t Flags) const {
  if ((Flags & static_cast<std::uint8_t>(~SerializableVirtualRegisterFlagMask)) != 0) {
    return makeUnexpected(ErrorCode::VerificationError,
                          "virtual register contains an unknown target flag");
  }
  return {};
}

Expected<std::vector<std::string_view>>
TargetMIRTextInfo::virtualRegisterFlagNames(std::uint8_t Flags) const {
  auto Validation = validateVirtualRegisterFlags(Flags);
  if (!Validation)
    return std::unexpected(Validation.error());

  std::vector<std::string_view> Names;
  for (const MIRVirtualRegisterFlagDesc &Flag : Description.VirtualRegisterFlags) {
    if ((Flags & Flag.Value) != 0)
      Names.push_back(Flag.Name);
  }
  return Names;
}

std::optional<IntrinsicID> TargetMIRTextInfo::intrinsic(std::string_view Name) const noexcept {
  const auto Position = IntrinsicNameToIndex.find(std::string(Name));
  if (Position == IntrinsicNameToIndex.end())
    return std::nullopt;
  return Description.Intrinsics[Position->second].ID;
}

std::optional<std::string_view> TargetMIRTextInfo::intrinsicName(IntrinsicID ID) const noexcept {
  const auto Position = IntrinsicToIndex.find(ID);
  if (Position == IntrinsicToIndex.end())
    return std::nullopt;
  return Description.Intrinsics[Position->second].Name;
}

std::optional<std::uint32_t>
TargetMIRTextInfo::dwarfRegister(PhysicalRegister Register) const noexcept {
  const auto Position = RegisterToDwarf.find(Register);
  if (Position == RegisterToDwarf.end())
    return std::nullopt;
  return Position->second;
}

std::optional<PhysicalRegister>
TargetMIRTextInfo::physicalRegister(std::uint32_t DwarfRegister) const noexcept {
  const auto Position = DwarfToRegister.find(DwarfRegister);
  if (Position == DwarfToRegister.end())
    return std::nullopt;
  return Position->second;
}

Expected<TargetFunctionState>
TargetMIRTextInfo::parseFunctionState(const MIRTargetNode &Node) const {
  if (Description.ParseFunctionState == nullptr || !Description.FunctionStateType) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a machineFunctionInfo MIR schema");
  }
  auto NodeResult = Node.verify();
  if (!NodeResult)
    return std::unexpected(NodeResult.error());
  auto State = Description.ParseFunctionState(Node);
  if (!State)
    return std::unexpected(State.error());
  auto StateResult = State->verify();
  if (!StateResult)
    return std::unexpected(StateResult.error());
  if (State->typeID() != Description.FunctionStateType) {
    return makeUnexpected(ErrorCode::ParseError,
                          "parsed target function state has the wrong schema TypeID");
  }
  return State;
}

Expected<MIRTargetNode>
TargetMIRTextInfo::printFunctionState(const TargetFunctionState &State) const {
  if (Description.PrintFunctionState == nullptr || !Description.FunctionStateType) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a machineFunctionInfo MIR schema");
  }
  auto StateResult = State.verify();
  if (!StateResult)
    return std::unexpected(StateResult.error());
  if (State.typeID() != Description.FunctionStateType) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target function state has the wrong MIR schema TypeID");
  }
  auto Node = Description.PrintFunctionState(State);
  if (!Node)
    return std::unexpected(Node.error());
  auto NodeResult = Node->verify();
  if (!NodeResult)
    return std::unexpected(NodeResult.error());
  if (Node->Kind != MIRTargetNodeKind::Mapping) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "machineFunctionInfo printer must produce a mapping");
  }
  return Node;
}

Expected<TargetConstant> TargetMIRTextInfo::parseTargetConstant(std::string_view Text) const {
  if (Description.ParseTargetConstant == nullptr) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a target constant MIR schema");
  }
  auto Constant = Description.ParseTargetConstant(Text);
  if (!Constant)
    return std::unexpected(Constant.error());
  auto Verification = Constant->verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Constant;
}

Expected<std::string> TargetMIRTextInfo::printTargetConstant(const TargetConstant &Constant) const {
  if (Description.PrintTargetConstant == nullptr) {
    return makeUnexpected(ErrorCode::Unsupported,
                          "target does not define a target constant MIR schema");
  }
  auto Verification = Constant.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Text = Description.PrintTargetConstant(Constant);
  if (!Text)
    return std::unexpected(Text.error());
  if (Text->empty()) {
    return makeUnexpected(ErrorCode::InvalidArgument,
                          "target constant printer produced an empty MIR value");
  }
  return Text;
}

} // namespace rcc
