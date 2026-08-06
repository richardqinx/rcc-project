// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/TargetRegisterInfo.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {
namespace {

[[nodiscard]] std::unexpected<Error> invalidDescription(std::string Message) {
  return makeUnexpected(ErrorCode::InvalidArgument,
                        "target register description: " + std::move(Message));
}

[[nodiscard]] bool isPowerOfTwo(std::uint32_t Value) noexcept {
  return Value != 0 && std::has_single_bit(Value);
}

[[nodiscard]] std::string lowerASCII(std::string_view Name) {
  std::string Result(Name);
  for (char &Character : Result) {
    if (Character >= 'A' && Character <= 'Z')
      Character = static_cast<char>(Character - 'A' + 'a');
  }
  return Result;
}

template <typename RangeT, typename ValueT>
[[nodiscard]] bool contains(const RangeT &Range, const ValueT &Value) {
  return std::ranges::find(Range, Value) != std::ranges::end(Range);
}

[[nodiscard]] const RegisterUnitLane *findUnit(const PhysicalRegisterDesc &Register,
                                               RegisterUnitID Unit) noexcept {
  const auto Position = std::ranges::find(Register.Units, Unit, &RegisterUnitLane::Unit);
  return Position == Register.Units.end() ? nullptr : &*Position;
}

} // namespace

Expected<TargetRegisterInfo> TargetRegisterInfo::create(TargetRegisterInfoDesc Description) {
  if (Description.RegisterUnits.empty() || Description.PhysicalRegisters.empty() ||
      Description.RegisterClasses.empty() || Description.RegisterBanks.empty()) {
    return invalidDescription(
        "register units, physical registers, classes, and banks must be nonempty");
  }

  TargetRegisterInfo Result(std::move(Description));

  std::unordered_set<std::string> UnitNames;
  for (std::size_t Index = 0; Index < Result.Description.RegisterUnits.size(); ++Index) {
    const RegisterUnitDesc &Unit = Result.Description.RegisterUnits[Index];
    if (Unit.ID.value() == 0 || Unit.Name.empty()) {
      return invalidDescription("register unit requires a nonzero ID and name");
    }
    if (!Result.UnitToIndex.emplace(Unit.ID, Index).second || !UnitNames.insert(Unit.Name).second) {
      return invalidDescription("register unit IDs and names must be unique");
    }
  }

  for (std::size_t Index = 0; Index < Result.Description.PhysicalRegisters.size(); ++Index) {
    const PhysicalRegisterDesc &Register = Result.Description.PhysicalRegisters[Index];
    if (!Register.Reg.isValid() || Register.Name.empty()) {
      return invalidDescription("physical register requires a valid number and name");
    }
    std::string MIRName = lowerASCII(Register.Name);
    if (!Result.RegisterToIndex.emplace(Register.Reg, Index).second ||
        Result.AlternativeRegisterNameToIndex.contains(MIRName) ||
        !Result.RegisterNameToIndex.emplace(MIRName, Index).second) {
      return invalidDescription(
          "physical register numbers and names must be unique case-insensitively");
    }
    for (const std::string &AlternativeName : Register.AlternativeNames) {
      const std::string NormalizedName = lowerASCII(AlternativeName);
      if (AlternativeName.empty() || Result.RegisterNameToIndex.contains(NormalizedName) ||
          !Result.AlternativeRegisterNameToIndex.emplace(NormalizedName, Index).second) {
        return invalidDescription(
            "physical register alternative names must be nonempty and unique case-insensitively");
      }
    }
    Result.RegisterMIRNames.push_back(std::move(MIRName));
  }

  std::unordered_set<std::string> SubRegNames;
  for (std::size_t Index = 0; Index < Result.Description.SubRegIndices.size(); ++Index) {
    const SubRegIndexDesc &SubReg = Result.Description.SubRegIndices[Index];
    if (SubReg.ID.value() == 0 || SubReg.Name.empty() || SubReg.Lanes.empty() ||
        SubReg.SizeInBits == 0 ||
        static_cast<std::uint64_t>(SubReg.OffsetInBits) + SubReg.SizeInBits >
            std::numeric_limits<std::uint32_t>::max()) {
      return invalidDescription(
          "subregister index requires a nonzero ID, name, lanes, and valid bit range");
    }
    if (!Result.SubRegIndexToIndex.emplace(SubReg.ID, Index).second ||
        !SubRegNames.insert(SubReg.Name).second ||
        !Result.SubRegNameToIndex.emplace(SubReg.Name, Index).second) {
      return invalidDescription("subregister index IDs and names must be unique");
    }
  }

  for (std::size_t Index = 0; Index < Result.Description.RegisterClasses.size(); ++Index) {
    const RegisterClassDesc &Class = Result.Description.RegisterClasses[Index];
    if (Class.ID.value() == 0 || Class.Name.empty()) {
      return invalidDescription("register class requires a nonzero ID and name");
    }
    std::string MIRName = lowerASCII(Class.Name);
    if (!Result.ClassToIndex.emplace(Class.ID, Index).second ||
        !Result.ClassNameToIndex.emplace(MIRName, Index).second) {
      return invalidDescription("register class IDs and names must be unique case-insensitively");
    }
    Result.ClassMIRNames.push_back(std::move(MIRName));
  }

  for (std::size_t Index = 0; Index < Result.Description.RegisterBanks.size(); ++Index) {
    const RegisterBankDesc &Bank = Result.Description.RegisterBanks[Index];
    if (Bank.ID.value() == 0 || Bank.Name.empty() || Bank.MaximumSizeInBits == 0) {
      return invalidDescription("register bank requires a nonzero ID, name, and maximum size");
    }
    std::string MIRName = lowerASCII(Bank.Name);
    if (!Result.BankToIndex.emplace(Bank.ID, Index).second ||
        !Result.BankNameToIndex.emplace(MIRName, Index).second) {
      return invalidDescription("register bank IDs and names must be unique case-insensitively");
    }
    Result.BankMIRNames.push_back(std::move(MIRName));
  }

  std::unordered_set<RegisterUnitID> ReferencedUnits;
  for (const PhysicalRegisterDesc &Register : Result.Description.PhysicalRegisters) {
    if (Register.Units.empty())
      return invalidDescription("physical register must reference at least one register unit");

    std::unordered_set<RegisterUnitID> SeenUnits;
    LaneMask RegisterLanes;
    for (const RegisterUnitLane &Unit : Register.Units) {
      if (Result.registerUnit(Unit.Unit) == nullptr || Unit.Lanes.empty() ||
          !SeenUnits.insert(Unit.Unit).second) {
        return invalidDescription(
            "physical register has an unknown, empty-lane, or duplicate register unit");
      }
      ReferencedUnits.insert(Unit.Unit);
      RegisterLanes = RegisterLanes | Unit.Lanes;
    }
    if (RegisterLanes.empty())
      return invalidDescription("physical register has no addressable lanes");
  }
  if (ReferencedUnits.size() != Result.Description.RegisterUnits.size())
    return invalidDescription("every declared register unit must be used by a physical register");

  const std::size_t RegisterCount = Result.Description.PhysicalRegisters.size();
  std::vector<std::vector<bool>> IsSubRegister(RegisterCount,
                                               std::vector<bool>(RegisterCount, false));
  for (const PhysicalSubRegisterDesc &Mapping : Result.Description.PhysicalSubRegisters) {
    const PhysicalRegisterDesc *Super = Result.physicalRegister(Mapping.SuperReg);
    const PhysicalRegisterDesc *Sub = Result.physicalRegister(Mapping.SubReg);
    const SubRegIndexDesc *Index = Result.subRegIndex(Mapping.Index);
    if (Super == nullptr || Sub == nullptr || Index == nullptr ||
        Mapping.SuperReg == Mapping.SubReg) {
      return invalidDescription(
          "physical subregister mapping references an unknown or identical register");
    }

    const std::uint64_t Key = pairKey(Mapping.SuperReg.number(), Mapping.Index.value());
    if (!Result.SubRegisterMap.emplace(Key, Mapping.SubReg).second)
      return invalidDescription("physical subregister mapping key must be unique");

    LaneMask ProjectedLanes;
    for (const RegisterUnitLane &SubUnit : Sub->Units) {
      const RegisterUnitLane *SuperUnit = findUnit(*Super, SubUnit.Unit);
      if (SuperUnit == nullptr) {
        return invalidDescription(
            "physical subregister units must be a subset of its superregister units");
      }
      ProjectedLanes = ProjectedLanes | SuperUnit->Lanes;
    }
    if (ProjectedLanes != Index->Lanes) {
      return invalidDescription(
          "physical subregister units do not match the subregister index lane mask");
    }

    const std::size_t SuperIndex = Result.RegisterToIndex.at(Mapping.SuperReg);
    const std::size_t SubIndex = Result.RegisterToIndex.at(Mapping.SubReg);
    IsSubRegister[SuperIndex][SubIndex] = true;
  }

  for (std::size_t Through = 0; Through < RegisterCount; ++Through) {
    for (std::size_t Super = 0; Super < RegisterCount; ++Super) {
      if (!IsSubRegister[Super][Through])
        continue;
      for (std::size_t Sub = 0; Sub < RegisterCount; ++Sub)
        IsSubRegister[Super][Sub] = IsSubRegister[Super][Sub] || IsSubRegister[Through][Sub];
    }
  }
  for (std::size_t Index = 0; Index < RegisterCount; ++Index) {
    if (IsSubRegister[Index][Index])
      return invalidDescription("physical subregister graph must be acyclic");
  }

  Result.SuperRegisters.resize(RegisterCount);
  for (std::size_t Sub = 0; Sub < RegisterCount; ++Sub) {
    for (std::size_t Super = 0; Super < RegisterCount; ++Super) {
      if (IsSubRegister[Super][Sub]) {
        Result.SuperRegisters[Sub].push_back(Result.Description.PhysicalRegisters[Super].Reg);
      }
    }
  }

  for (const SubRegIndexComposition &Composition : Result.Description.SubRegCompositions) {
    const SubRegIndexDesc *First = Result.subRegIndex(Composition.First);
    const SubRegIndexDesc *Second = Result.subRegIndex(Composition.Second);
    const SubRegIndexDesc *Composed = Result.subRegIndex(Composition.Result);
    if (First == nullptr || Second == nullptr || Composed == nullptr) {
      return invalidDescription("subregister composition references an unknown index");
    }
    const std::uint64_t ComposedOffset =
        static_cast<std::uint64_t>(First->OffsetInBits) + Second->OffsetInBits;
    if (static_cast<std::uint64_t>(Second->OffsetInBits) + Second->SizeInBits > First->SizeInBits ||
        Composed->SizeInBits != Second->SizeInBits || Composed->OffsetInBits != ComposedOffset) {
      return invalidDescription("subregister composition has inconsistent bit ranges");
    }
    const std::uint64_t Key = pairKey(Composition.First.value(), Composition.Second.value());
    if (!Result.CompositionMap.emplace(Key, Composition.Result).second)
      return invalidDescription("subregister composition key must be unique");
  }

  for (const SubRegIndexComposition &Composition : Result.Description.SubRegCompositions) {
    for (const PhysicalRegisterDesc &Super : Result.Description.PhysicalRegisters) {
      const auto Intermediate = Result.subRegister(Super.Reg, Composition.First);
      if (!Intermediate)
        continue;
      const auto Final = Result.subRegister(*Intermediate, Composition.Second);
      if (!Final)
        continue;
      const auto Composed = Result.subRegister(Super.Reg, Composition.Result);
      if (!Composed || *Composed != *Final) {
        return invalidDescription(
            "subregister composition conflicts with physical subregister mappings");
      }
    }
  }

  for (const RegisterBankDesc &Bank : Result.Description.RegisterBanks) {
    if (Bank.RegisterClasses.empty())
      return invalidDescription("register bank must cover at least one register class");

    std::unordered_set<RegisterClassID> SeenClasses;
    for (const RegisterClassID ClassID : Bank.RegisterClasses) {
      const RegisterClassDesc *Class = Result.registerClass(ClassID);
      if (Class == nullptr || !SeenClasses.insert(ClassID).second) {
        return invalidDescription("register bank has an unknown or duplicate register class");
      }
      if (Class->SizeInBits > Bank.MaximumSizeInBits) {
        return invalidDescription(
            "register bank maximum size is smaller than a covered register class");
      }
    }
  }

  for (const RegisterClassDesc &Class : Result.Description.RegisterClasses) {
    if (Class.Registers.empty() || Class.SizeInBits == 0 ||
        Class.SpillSizeInBits < Class.SizeInBits || !isPowerOfTwo(Class.SpillAlignmentInBits) ||
        Class.Lanes.empty()) {
      return invalidDescription(
          "register class requires members, sizes, power-of-two spill alignment, and lanes");
    }

    std::unordered_set<PhysicalRegister> Members;
    for (const PhysicalRegister Reg : Class.Registers) {
      if (!Result.isKnownPhysicalRegister(Reg) || !Members.insert(Reg).second ||
          !Result.isValidLaneMask(Reg, Class.Lanes)) {
        return invalidDescription(
            "register class has an unknown, duplicate, or lane-incompatible member");
      }
    }

    std::unordered_set<PhysicalRegister> AllocationRegisters;
    for (const PhysicalRegister Reg : Class.AllocationOrder) {
      const PhysicalRegisterDesc *Register = Result.physicalRegister(Reg);
      const bool IsUnavailable =
          Register != nullptr &&
          (Register->IsConstant || Register->IsArtificial || Register->IsReserved);
      if (!Members.contains(Reg) || IsUnavailable || !AllocationRegisters.insert(Reg).second) {
        return invalidDescription(
            "register class allocation order must contain unique allocatable class members");
      }
    }
    if ((Class.Allocatable && Class.AllocationOrder.empty()) ||
        (!Class.Allocatable && !Class.AllocationOrder.empty())) {
      return invalidDescription(
          "allocatable classes require an allocation order and nonallocatable classes forbid one");
    }

    std::unordered_set<SubRegIndex> SupportedSubRegs;
    LaneMask CoveredSubRegLanes;
    for (const SubRegIndex SubReg : Class.SupportedSubRegs) {
      const SubRegIndexDesc *SubRegDesc = Result.subRegIndex(SubReg);
      if (SubRegDesc == nullptr || !SupportedSubRegs.insert(SubReg).second ||
          !Class.Lanes.contains(SubRegDesc->Lanes)) {
        return invalidDescription(
            "register class has an unknown, duplicate, or lane-incompatible subregister index");
      }
      CoveredSubRegLanes = CoveredSubRegLanes | SubRegDesc->Lanes;
    }
    if (Class.CoveredBySubRegs &&
        (Class.SupportedSubRegs.empty() || !CoveredSubRegLanes.contains(Class.Lanes))) {
      return invalidDescription(
          "subregister-covered class must cover every class lane with supported indices");
    }

    std::unordered_set<RegisterClassID> SuperClasses;
    for (const RegisterClassID SuperClass : Class.SuperClasses) {
      if (Result.registerClass(SuperClass) == nullptr || SuperClass == Class.ID ||
          !SuperClasses.insert(SuperClass).second) {
        return invalidDescription("register class has an unknown, self, or duplicate superclass");
      }
      Result.SubclassRelations.insert(pairKey(Class.ID.value(), SuperClass.value()));
    }
    Result.SubclassRelations.insert(pairKey(Class.ID.value(), Class.ID.value()));

    if (Result.registerBank(Class.DefaultBank) == nullptr ||
        !Result.bankCoversClass(Class.DefaultBank, Class.ID)) {
      return invalidDescription(
          "register class default bank must exist and explicitly cover the class");
    }
  }

  bool RelationsChanged = true;
  while (RelationsChanged) {
    RelationsChanged = false;
    for (const RegisterClassDesc &Candidate : Result.Description.RegisterClasses) {
      for (const RegisterClassDesc &Intermediate : Result.Description.RegisterClasses) {
        if (!Result.isSubclass(Candidate.ID, Intermediate.ID))
          continue;
        for (const RegisterClassDesc &Required : Result.Description.RegisterClasses) {
          if (!Result.isSubclass(Intermediate.ID, Required.ID))
            continue;
          RelationsChanged |=
              Result.SubclassRelations.insert(pairKey(Candidate.ID.value(), Required.ID.value()))
                  .second;
        }
      }
    }
  }

  for (const RegisterClassDesc &Class : Result.Description.RegisterClasses) {
    for (const RegisterClassDesc &Other : Result.Description.RegisterClasses) {
      if (Class.ID != Other.ID && Result.isSubclass(Class.ID, Other.ID) &&
          Result.isSubclass(Other.ID, Class.ID)) {
        return invalidDescription("register class hierarchy must be acyclic");
      }
    }
    for (const RegisterClassID SuperID : Class.SuperClasses) {
      const RegisterClassDesc *Super = Result.registerClass(SuperID);
      if (Super == nullptr || Class.SizeInBits != Super->SizeInBits ||
          Class.SpillSizeInBits != Super->SpillSizeInBits ||
          Class.SpillAlignmentInBits != Super->SpillAlignmentInBits ||
          !std::ranges::all_of(Class.Registers, [Super](PhysicalRegister Reg) {
            return contains(Super->Registers, Reg);
          })) {
        return invalidDescription(
            "register class must be storage-compatible with each declared superclass");
      }
    }
  }

  for (const SubRegClassMapping &Mapping : Result.Description.SubRegClassMappings) {
    const RegisterClassDesc *Source = Result.registerClass(Mapping.SourceClass);
    const SubRegIndexDesc *Index = Result.subRegIndex(Mapping.Index);
    const RegisterClassDesc *Mapped = Result.registerClass(Mapping.ResultClass);
    if (Source == nullptr || Index == nullptr || Mapped == nullptr ||
        !contains(Source->SupportedSubRegs, Mapping.Index)) {
      return invalidDescription(
          "subregister class mapping references an unknown or unsupported entity");
    }
    if (Mapped->SizeInBits != Index->SizeInBits) {
      return invalidDescription("subregister class mapping result size must match its index size");
    }
    if (static_cast<std::uint64_t>(Index->OffsetInBits) + Index->SizeInBits > Source->SizeInBits) {
      return invalidDescription("subregister class mapping index exceeds its source class size");
    }

    const std::uint64_t Key = pairKey(Mapping.SourceClass.value(), Mapping.Index.value());
    if (!Result.SubRegClassMap.emplace(Key, Mapping.ResultClass).second) {
      return invalidDescription("subregister class mapping key must be unique");
    }
    for (const PhysicalRegister SuperReg : Source->Registers) {
      const auto SubReg = Result.subRegister(SuperReg, Mapping.Index);
      if (!SubReg || !Result.registerClassContains(Mapping.ResultClass, *SubReg)) {
        return invalidDescription("subregister class mapping does not cover every source register");
      }
    }
  }

  for (const RegisterClassDesc &Class : Result.Description.RegisterClasses) {
    for (const SubRegIndex SubReg : Class.SupportedSubRegs) {
      if (!Result.subRegClass(Class.ID, SubReg)) {
        return invalidDescription(
            "every supported class subregister index requires a class mapping");
      }
    }
  }

  for (const SubRegIndexComposition &Composition : Result.Description.SubRegCompositions) {
    for (const RegisterClassDesc &Source : Result.Description.RegisterClasses) {
      const auto Intermediate = Result.subRegClass(Source.ID, Composition.First);
      if (!Intermediate)
        continue;
      const auto Final = Result.subRegClass(*Intermediate, Composition.Second);
      if (!Final)
        continue;
      const auto Composed = Result.subRegClass(Source.ID, Composition.Result);
      if (!Composed || !Result.isSubclass(*Final, *Composed)) {
        return invalidDescription("subregister composition conflicts with register class mappings");
      }
    }
  }

  for (const PhysicalRegisterDesc &Register : Result.Description.PhysicalRegisters) {
    if (std::ranges::none_of(Result.Description.RegisterClasses,
                             [&Register](const RegisterClassDesc &Class) {
                               return contains(Class.Registers, Register.Reg);
                             })) {
      return invalidDescription(
          "every physical register must belong to at least one register class");
    }
  }

  Result.Aliases.resize(RegisterCount);
  for (std::size_t Left = 0; Left < RegisterCount; ++Left) {
    for (std::size_t Right = 0; Right < RegisterCount; ++Right) {
      if (Left != Right && Result.regsOverlap(Result.Description.PhysicalRegisters[Left].Reg,
                                              Result.Description.PhysicalRegisters[Right].Reg)) {
        Result.Aliases[Left].push_back(Result.Description.PhysicalRegisters[Right].Reg);
      }
    }
  }

  return Result;
}

const RegisterUnitDesc *TargetRegisterInfo::registerUnit(RegisterUnitID ID) const noexcept {
  const auto Position = UnitToIndex.find(ID);
  return Position == UnitToIndex.end() ? nullptr : &Description.RegisterUnits[Position->second];
}

const PhysicalRegisterDesc *
TargetRegisterInfo::physicalRegister(PhysicalRegister Reg) const noexcept {
  const auto Position = RegisterToIndex.find(Reg);
  return Position == RegisterToIndex.end() ? nullptr
                                           : &Description.PhysicalRegisters[Position->second];
}

const PhysicalRegisterDesc *
TargetRegisterInfo::physicalRegister(std::string_view Name) const noexcept {
  const auto Position = RegisterNameToIndex.find(std::string(Name));
  return Position == RegisterNameToIndex.end() ? nullptr
                                               : &Description.PhysicalRegisters[Position->second];
}

const PhysicalRegisterDesc *
TargetRegisterInfo::physicalRegisterByAlternativeName(std::string_view Name) const noexcept {
  const auto Position = AlternativeRegisterNameToIndex.find(lowerASCII(Name));
  return Position == AlternativeRegisterNameToIndex.end()
             ? nullptr
             : &Description.PhysicalRegisters[Position->second];
}

bool TargetRegisterInfo::isReservedRegister(PhysicalRegister Reg) const noexcept {
  const PhysicalRegisterDesc *Register = physicalRegister(Reg);
  return Register != nullptr && Register->IsReserved;
}

const SubRegIndexDesc *TargetRegisterInfo::subRegIndex(SubRegIndex Index) const noexcept {
  const auto Position = SubRegIndexToIndex.find(Index);
  return Position == SubRegIndexToIndex.end() ? nullptr
                                              : &Description.SubRegIndices[Position->second];
}

const SubRegIndexDesc *TargetRegisterInfo::subRegIndex(std::string_view Name) const noexcept {
  const auto Position = SubRegNameToIndex.find(std::string(Name));
  return Position == SubRegNameToIndex.end() ? nullptr
                                             : &Description.SubRegIndices[Position->second];
}

const RegisterClassDesc *TargetRegisterInfo::registerClass(RegisterClassID ID) const noexcept {
  const auto Position = ClassToIndex.find(ID);
  return Position == ClassToIndex.end() ? nullptr : &Description.RegisterClasses[Position->second];
}

const RegisterClassDesc *TargetRegisterInfo::registerClass(std::string_view Name) const noexcept {
  const auto Position = ClassNameToIndex.find(std::string(Name));
  return Position == ClassNameToIndex.end() ? nullptr
                                            : &Description.RegisterClasses[Position->second];
}

const RegisterBankDesc *TargetRegisterInfo::registerBank(RegisterBankID ID) const noexcept {
  const auto Position = BankToIndex.find(ID);
  return Position == BankToIndex.end() ? nullptr : &Description.RegisterBanks[Position->second];
}

const RegisterBankDesc *TargetRegisterInfo::registerBank(std::string_view Name) const noexcept {
  const auto Position = BankNameToIndex.find(std::string(Name));
  return Position == BankNameToIndex.end() ? nullptr : &Description.RegisterBanks[Position->second];
}

std::optional<std::string_view>
TargetRegisterInfo::physicalRegisterMIRName(PhysicalRegister Reg) const noexcept {
  const auto Position = RegisterToIndex.find(Reg);
  if (Position == RegisterToIndex.end())
    return std::nullopt;
  return RegisterMIRNames[Position->second];
}

std::optional<std::string_view>
TargetRegisterInfo::registerClassMIRName(RegisterClassID ID) const noexcept {
  const auto Position = ClassToIndex.find(ID);
  if (Position == ClassToIndex.end())
    return std::nullopt;
  return ClassMIRNames[Position->second];
}

std::optional<std::string_view>
TargetRegisterInfo::registerBankMIRName(RegisterBankID ID) const noexcept {
  const auto Position = BankToIndex.find(ID);
  if (Position == BankToIndex.end())
    return std::nullopt;
  return BankMIRNames[Position->second];
}

LaneMask TargetRegisterInfo::registerLaneMask(PhysicalRegister Reg) const noexcept {
  const PhysicalRegisterDesc *Register = physicalRegister(Reg);
  if (Register == nullptr)
    return {};

  LaneMask Result;
  for (const RegisterUnitLane &Unit : Register->Units)
    Result = Result | Unit.Lanes;
  return Result;
}

bool TargetRegisterInfo::isValidLaneMask(PhysicalRegister Reg, LaneMask Lanes) const noexcept {
  if (physicalRegister(Reg) == nullptr || Lanes.empty())
    return false;
  if (Lanes == LaneMask::all())
    return true;
  return registerLaneMask(Reg).contains(Lanes);
}

bool TargetRegisterInfo::regsOverlap(PhysicalRegister Left, PhysicalRegister Right) const noexcept {
  const PhysicalRegisterDesc *LeftDesc = physicalRegister(Left);
  const PhysicalRegisterDesc *RightDesc = physicalRegister(Right);
  if (LeftDesc == nullptr || RightDesc == nullptr)
    return false;

  return std::ranges::any_of(LeftDesc->Units, [RightDesc](const RegisterUnitLane &Unit) {
    return findUnit(*RightDesc, Unit.Unit) != nullptr;
  });
}

std::span<const PhysicalRegister> TargetRegisterInfo::aliases(PhysicalRegister Reg) const noexcept {
  const auto Position = RegisterToIndex.find(Reg);
  if (Position == RegisterToIndex.end())
    return {};
  return Aliases[Position->second];
}

std::optional<PhysicalRegister> TargetRegisterInfo::subRegister(PhysicalRegister SuperReg,
                                                                SubRegIndex Index) const noexcept {
  const auto Position = SubRegisterMap.find(pairKey(SuperReg.number(), Index.value()));
  if (Position == SubRegisterMap.end())
    return std::nullopt;
  return Position->second;
}

bool TargetRegisterInfo::isSubRegister(PhysicalRegister SuperReg,
                                       PhysicalRegister SubReg) const noexcept {
  const auto Position = RegisterToIndex.find(SubReg);
  if (Position == RegisterToIndex.end())
    return false;
  return contains(SuperRegisters[Position->second], SuperReg);
}

std::span<const PhysicalRegister>
TargetRegisterInfo::superRegisters(PhysicalRegister Reg) const noexcept {
  const auto Position = RegisterToIndex.find(Reg);
  if (Position == RegisterToIndex.end())
    return {};
  return SuperRegisters[Position->second];
}

std::optional<SubRegIndex>
TargetRegisterInfo::composeSubRegIndices(SubRegIndex First, SubRegIndex Second) const noexcept {
  const auto Position = CompositionMap.find(pairKey(First.value(), Second.value()));
  if (Position == CompositionMap.end())
    return std::nullopt;
  return Position->second;
}

bool TargetRegisterInfo::registerClassContains(RegisterClassID Class,
                                               PhysicalRegister Reg) const noexcept {
  const RegisterClassDesc *ClassDesc = registerClass(Class);
  return ClassDesc != nullptr && contains(ClassDesc->Registers, Reg);
}

bool TargetRegisterInfo::isSubclass(RegisterClassID Candidate,
                                    RegisterClassID Required) const noexcept {
  return SubclassRelations.contains(pairKey(Candidate.value(), Required.value()));
}

bool TargetRegisterInfo::registerClassesOverlap(RegisterClassID Left,
                                                RegisterClassID Right) const noexcept {
  const RegisterClassDesc *LeftClass = registerClass(Left);
  const RegisterClassDesc *RightClass = registerClass(Right);
  if (LeftClass == nullptr || RightClass == nullptr)
    return false;
  return std::ranges::any_of(LeftClass->Registers, [RightClass](PhysicalRegister Reg) {
    return contains(RightClass->Registers, Reg);
  });
}

std::optional<RegisterClassID> TargetRegisterInfo::subRegClass(RegisterClassID Source,
                                                               SubRegIndex Index) const noexcept {
  const auto Position = SubRegClassMap.find(pairKey(Source.value(), Index.value()));
  if (Position == SubRegClassMap.end())
    return std::nullopt;
  return Position->second;
}

bool TargetRegisterInfo::bankCoversClass(RegisterBankID Bank,
                                         RegisterClassID Class) const noexcept {
  const RegisterBankDesc *BankDesc = registerBank(Bank);
  return BankDesc != nullptr && contains(BankDesc->RegisterClasses, Class);
}

bool TargetRegisterInfo::bankContainsRegister(RegisterBankID Bank,
                                              PhysicalRegister Reg) const noexcept {
  const RegisterBankDesc *BankDesc = registerBank(Bank);
  if (BankDesc == nullptr)
    return false;
  return std::ranges::any_of(BankDesc->RegisterClasses, [this, Reg](RegisterClassID Class) {
    return registerClassContains(Class, Reg);
  });
}

bool TargetRegisterInfo::registerBanksOverlap(RegisterBankID Left,
                                              RegisterBankID Right) const noexcept {
  const RegisterBankDesc *LeftBank = registerBank(Left);
  const RegisterBankDesc *RightBank = registerBank(Right);
  if (LeftBank == nullptr || RightBank == nullptr)
    return false;
  return std::ranges::any_of(Description.PhysicalRegisters,
                             [this, Left, Right](const PhysicalRegisterDesc &Register) {
                               return bankContainsRegister(Left, Register.Reg) &&
                                      bankContainsRegister(Right, Register.Reg);
                             });
}

Expected<void> TargetRegisterInfo::validateRegisterMask(const RegisterMask &Mask) const {
  constexpr std::uint64_t BitsPerWord = 64;
  for (std::size_t WordIndex = 0; WordIndex < Mask.words().size(); ++WordIndex) {
    std::uint64_t Word = Mask.words()[WordIndex];
    while (Word != 0) {
      const std::uint64_t BitIndex = static_cast<std::uint64_t>(std::countr_zero(Word));
      const std::uint64_t RegisterNumber =
          static_cast<std::uint64_t>(WordIndex) * BitsPerWord + BitIndex;
      const auto Reg = PhysicalRegister::create(RegisterNumber);
      if (!Reg || !isKnownPhysicalRegister(*Reg)) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "register mask contains an unknown physical register");
      }
      Word &= Word - 1;
    }
  }
  return {};
}

} // namespace rcc
