// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVTarget.h"

#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/Target/RISCV/RISCVFunctionInfo.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rcc::riscv {
namespace {

[[nodiscard]] PhysicalRegister physical(PhysicalRegisterID ID) {
  const auto Register = physicalRegister(ID);
  assert(Register.has_value());
  return *Register;
}

[[nodiscard]] RegisterUnitLane unitLane(RegisterUnit Unit, std::uint64_t Lanes) {
  return {
      .Unit = RegisterUnitID::fromValue(static_cast<std::uint32_t>(Unit)),
      .Lanes = LaneMask::fromRaw(Lanes),
  };
}

[[nodiscard]] std::vector<PhysicalRegister>
physicalRegisters(std::initializer_list<PhysicalRegisterID> IDs) {
  std::vector<PhysicalRegister> Result;
  Result.reserve(IDs.size());
  for (const PhysicalRegisterID ID : IDs)
    Result.push_back(physical(ID));
  return Result;
}

[[nodiscard]] RegisterClassDesc registerClass(
    RegisterClass ID, const char *Name, std::initializer_list<PhysicalRegisterID> Registers,
    std::initializer_list<PhysicalRegisterID> AllocationOrder, RegisterBank Bank,
    std::uint32_t SizeInBits, std::uint32_t SpillSizeInBits, std::uint32_t SpillAlignmentInBits,
    std::uint64_t Lanes, bool Allocatable, std::uint32_t CopyCost) {
  return {
      .ID = RegisterClassID::fromValue(static_cast<std::uint32_t>(ID)),
      .Name = Name,
      .Registers = physicalRegisters(Registers),
      .AllocationOrder = physicalRegisters(AllocationOrder),
      .SizeInBits = SizeInBits,
      .SpillSizeInBits = SpillSizeInBits,
      .SpillAlignmentInBits = SpillAlignmentInBits,
      .Lanes = LaneMask::fromRaw(Lanes),
      .SupportedSubRegs = {},
      .SuperClasses = {},
      .DefaultBank = RegisterBankID::fromValue(static_cast<std::uint32_t>(Bank)),
      .Allocatable = Allocatable,
      .CopyCost = CopyCost,
  };
}

[[nodiscard]] Expected<void> appendRISCVSubRegisterInfo(TargetRegisterInfoDesc &Description) {
  constexpr SubRegIndex FPR32SubReg = SubRegIndex::fromValue(1);
  constexpr std::uint32_t FirstDouble = static_cast<std::uint32_t>(PhysicalRegisterID::F0_D);
  constexpr std::uint32_t LastDouble = static_cast<std::uint32_t>(PhysicalRegisterID::F31_D);
  constexpr std::uint32_t FirstSingle = static_cast<std::uint32_t>(PhysicalRegisterID::F0_F);
  constexpr std::uint32_t LastSingle = static_cast<std::uint32_t>(PhysicalRegisterID::F31_F);
  static_assert(LastDouble - FirstDouble == 31);
  static_assert(LastSingle - FirstSingle == 31);

  Description.SubRegIndices.push_back({FPR32SubReg, "sub_32", LaneMask::fromRaw(1), 32, 0});
  for (std::uint32_t Index = 0; Index != 32; ++Index) {
    auto Double = PhysicalRegister::create(FirstDouble + Index);
    auto Single = PhysicalRegister::create(FirstSingle + Index);
    if (!Double || !Single)
      return makeUnexpected(ErrorCode::InternalError, "RISC-V FPR view ID is invalid");
    Description.PhysicalSubRegisters.push_back({*Double, FPR32SubReg, *Single});
  }

  const auto AddClassMapping = [&](RegisterClass Super, RegisterClass Sub) -> Expected<void> {
    const RegisterClassID SuperID = RegisterClassID::fromValue(static_cast<std::uint32_t>(Super));
    const RegisterClassID SubID = RegisterClassID::fromValue(static_cast<std::uint32_t>(Sub));
    const auto Position =
        std::ranges::find(Description.RegisterClasses, SuperID, &RegisterClassDesc::ID);
    if (Position == Description.RegisterClasses.end()) {
      return makeUnexpected(ErrorCode::InternalError, "RISC-V FPR super-register class is missing");
    }
    Position->SupportedSubRegs.push_back(FPR32SubReg);
    Description.SubRegClassMappings.push_back({SuperID, FPR32SubReg, SubID});
    return {};
  };
  if (auto Result = AddClassMapping(RegisterClass::FPR64, RegisterClass::FPR32); !Result)
    return Result;
  return AddClassMapping(RegisterClass::FPR64C, RegisterClass::FPR32C);
}

[[nodiscard]] Expected<void> appendRISCVVectorTupleInfo(TargetRegisterInfoDesc &Description) {
  constexpr std::uint32_t FirstVector = static_cast<std::uint32_t>(PhysicalRegisterID::V0);
  constexpr std::uint32_t LastVector = static_cast<std::uint32_t>(PhysicalRegisterID::V31);
  static_assert(LastVector - FirstVector == 31);

  const RegisterBankID VectorBankID =
      RegisterBankID::fromValue(static_cast<std::uint32_t>(RegisterBank::VR));
  auto VectorBank =
      std::ranges::find(Description.RegisterBanks, VectorBankID, &RegisterBankDesc::ID);
  if (VectorBank == Description.RegisterBanks.end())
    return makeUnexpected(ErrorCode::InternalError, "RISC-V vector register bank is missing");

  std::uint32_t NextRegisterID = 1;
  for (const PhysicalRegisterDesc &Register : Description.PhysicalRegisters)
    NextRegisterID = std::max(NextRegisterID, Register.Reg.number() + 1U);
  std::uint32_t NextClassID = 1;
  for (const RegisterClassDesc &Class : Description.RegisterClasses)
    NextClassID = std::max(NextClassID, Class.ID.value() + 1U);

  for (std::uint32_t Length = 2; Length <= 8; ++Length) {
    std::vector<PhysicalRegister> TupleRegisters;
    TupleRegisters.reserve(33U - Length);
    for (std::uint32_t Start = 0; Start + Length <= 32; ++Start) {
      auto Tuple = PhysicalRegister::create(NextRegisterID++);
      if (!Tuple)
        return makeUnexpected(ErrorCode::InternalError, "RISC-V vector tuple ID is invalid");

      std::string Name;
      std::vector<RegisterUnitLane> Units;
      Units.reserve(Length);
      for (std::uint32_t Offset = 0; Offset < Length; ++Offset) {
        auto Element = PhysicalRegister::create(FirstVector + Start + Offset);
        if (!Element)
          return makeUnexpected(ErrorCode::InternalError,
                                "RISC-V vector tuple element ID is invalid");
        const auto ElementDescription =
            std::ranges::find(Description.PhysicalRegisters, *Element, &PhysicalRegisterDesc::Reg);
        if (ElementDescription == Description.PhysicalRegisters.end() ||
            ElementDescription->Units.size() != 1) {
          return makeUnexpected(ErrorCode::InternalError,
                                "RISC-V vector tuple element has invalid register units");
        }
        if (!Name.empty())
          Name.push_back('_');
        Name.append(ElementDescription->Name);
        Units.push_back(ElementDescription->Units.front());
      }

      Description.PhysicalRegisters.push_back({
          .Reg = *Tuple,
          .Name = std::move(Name),
          .Encoding = Start,
          .Units = std::move(Units),
          .IsConstant = false,
          .IsArtificial = false,
          .IsReserved = false,
          .AlternativeNames = {},
      });
      TupleRegisters.push_back(*Tuple);
    }

    const RegisterClassID ClassID = RegisterClassID::fromValue(NextClassID++);
    Description.RegisterClasses.push_back({
        .ID = ClassID,
        .Name = "VRTuple" + std::to_string(Length),
        .Registers = std::move(TupleRegisters),
        .AllocationOrder = {},
        .SizeInBits = Length * 64U,
        .SpillSizeInBits = Length * 64U,
        .SpillAlignmentInBits = 64,
        .Lanes = LaneMask::fromRaw(1),
        .SupportedSubRegs = {},
        .SuperClasses = {},
        .DefaultBank = VectorBankID,
        .Allocatable = false,
        .CopyCost = Length,
    });
    VectorBank->RegisterClasses.push_back(ClassID);
  }
  return {};
}

[[nodiscard]] std::shared_ptr<const RegisterMask>
makePreservedMask(std::initializer_list<PhysicalRegisterID> Preserved) {
  std::vector<std::uint64_t> Words;
  for (const PhysicalRegisterID ID : Preserved) {
    const std::uint32_t Number = static_cast<std::uint32_t>(ID);
    const std::size_t Word = Number / 64U;
    if (Words.size() <= Word)
      Words.resize(Word + 1U);
    Words[Word] |= std::uint64_t{1} << (Number % 64U);
  }
  return std::make_shared<const RegisterMask>(std::move(Words));
}

#include "RISCVGenTargetDescription.inc"

} // namespace

Expected<PhysicalRegister> physicalRegister(PhysicalRegisterID ID) {
  return PhysicalRegister::create(static_cast<std::uint32_t>(ID));
}

Expected<TargetRegisterInfo> createRegisterInfo() {
  TargetRegisterInfoDesc Description;
  appendGeneratedRegisterInfo(Description);
  if (auto Result = appendRISCVSubRegisterInfo(Description); !Result)
    return std::unexpected(Result.error());
  if (auto Result = appendRISCVVectorTupleInfo(Description); !Result)
    return std::unexpected(Result.error());
  return TargetRegisterInfo::create(std::move(Description));
}

Expected<TargetMIRTextInfo> createMIRTextInfo() {
  auto Registers = createRegisterInfo();
  if (!Registers)
    return std::unexpected(Registers.error());
  TargetMIRTextInfoDesc Description;
  appendGeneratedMIRTextInfo(Description);
  configureFunctionInfoMIRSchema(Description);
  return TargetMIRTextInfo::create(std::move(Description), *Registers);
}

Expected<TargetDescription> createTargetDescription() {
  auto Registers = createRegisterInfo();
  if (!Registers)
    return std::unexpected(Registers.error());

  std::vector<InstructionDescriptor> Descriptors;
  std::vector<rcc::ImmediatePredicate> Predicates;
  appendGeneratedInstructionInfo(Descriptors, Predicates);
  auto GenericInfo = appendGenericInstructionInfo(Descriptors, Predicates);
  if (!GenericInfo)
    return std::unexpected(GenericInfo.error());
  auto Instructions = InstructionSet::create(std::move(Descriptors), std::move(Predicates));
  if (!Instructions)
    return std::unexpected(Instructions.error());

  TargetMIRTextInfoDesc MIRText;
  appendGeneratedMIRTextInfo(MIRText);
  configureFunctionInfoMIRSchema(MIRText);
  TargetBooleanContentInfo BooleanInfo{
      .ScalarInteger = BooleanContent::ZeroOrOne,
      .ScalarFloatingPoint = BooleanContent::ZeroOrOne,
  };
  auto StackPointer = physicalRegister(PhysicalRegisterID::X2);
  if (!StackPointer)
    return std::unexpected(StackPointer.error());
  TargetStackInfo StackInfo{
      .StackPointerRegisterToSaveRestore = *StackPointer,
      .GrowthDirection = StackGrowthDirection::Down,
  };
  return TargetDescription::create(std::move(*Instructions), std::move(*Registers),
                                   std::move(MIRText), BooleanInfo, StackInfo);
}

std::shared_ptr<const RegisterMask> createCallPreservedMask(CallingConvention Convention) {
  const CallingConventionInfo *Info = callingConvention(Convention);
  if (Info == nullptr)
    return {};
  TargetMIRTextInfoDesc Description;
  appendGeneratedMIRTextInfo(Description);
  configureFunctionInfoMIRSchema(Description);
  const auto Mask = std::ranges::find(Description.RegisterMasks, Info->PreservedMaskName,
                                      &MIRRegisterMaskDesc::Name);
  return Mask == Description.RegisterMasks.end() ? std::shared_ptr<const RegisterMask>{}
                                                 : Mask->Mask;
}

std::span<const SchedulingClassInfo> schedulingClasses() noexcept {
  return GeneratedSchedulingClasses;
}

std::span<const EncodingCategoryInfo> encodingCategories() noexcept {
  return GeneratedEncodingCategories;
}

std::span<const AssemblyFormatInfo> assemblyFormats() noexcept {
  return GeneratedAssemblyFormats;
}

std::span<const AssemblyInstructionInfo> assemblyInstructions() noexcept {
  return GeneratedAssemblyInstructions;
}

const AssemblyInstructionInfo *assemblyInstruction(Opcode InstructionOpcode) noexcept {
  for (const AssemblyInstructionInfo &Info : GeneratedAssemblyInstructions) {
    if (Info.InstructionOpcode == InstructionOpcode)
      return &Info;
  }
  return nullptr;
}

std::span<const CallingConventionInfo> callingConventions() noexcept {
  return GeneratedCallingConventions;
}

const CallingConventionInfo *callingConvention(CallingConvention ID) noexcept {
  for (const CallingConventionInfo &Info : GeneratedCallingConventions) {
    if (Info.ID == ID)
      return &Info;
  }
  return nullptr;
}

} // namespace rcc::riscv
