// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RCC_CODEGEN_TARGETREGISTERINFO_H
#define RCC_CODEGEN_TARGETREGISTERINFO_H

#include "rcc/CodeGen/MachineIRTypes.h"
#include "rcc/CodeGen/Register.h"
#include "rcc/CodeGen/RegisterMask.h"
#include "rcc/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rcc {

struct RegisterUnitDesc final {
  RegisterUnitID ID;
  std::string Name;
};

struct RegisterUnitLane final {
  RegisterUnitID Unit;
  LaneMask Lanes;
};

struct PhysicalRegisterDesc final {
  PhysicalRegister Reg;
  std::string Name;
  std::uint32_t Encoding = 0;
  std::vector<RegisterUnitLane> Units;
  bool IsConstant = false;
  bool IsArtificial = false;
};

struct SubRegIndexDesc final {
  SubRegIndex ID;
  std::string Name;
  LaneMask Lanes;
  std::uint32_t SizeInBits = 0;
  std::uint32_t OffsetInBits = 0;
};

struct PhysicalSubRegisterDesc final {
  PhysicalRegister SuperReg;
  SubRegIndex Index;
  PhysicalRegister SubReg;
};

struct SubRegIndexComposition final {
  SubRegIndex First;
  SubRegIndex Second;
  SubRegIndex Result;
};

struct RegisterClassDesc final {
  RegisterClassID ID;
  std::string Name;
  std::vector<PhysicalRegister> Registers;
  std::vector<PhysicalRegister> AllocationOrder;
  std::uint32_t SizeInBits = 0;
  std::uint32_t SpillSizeInBits = 0;
  std::uint32_t SpillAlignmentInBits = 0;
  LaneMask Lanes;
  std::vector<SubRegIndex> SupportedSubRegs;
  std::vector<RegisterClassID> SuperClasses;
  RegisterBankID DefaultBank;
  bool Allocatable = true;
  std::uint32_t CopyCost = 1;
  bool CoveredBySubRegs = false;
};

struct SubRegClassMapping final {
  RegisterClassID SourceClass;
  SubRegIndex Index;
  RegisterClassID ResultClass;
};

struct RegisterBankDesc final {
  RegisterBankID ID;
  std::string Name;
  std::uint32_t MaximumSizeInBits = 0;
  std::vector<RegisterClassID> RegisterClasses;
};

struct TargetRegisterInfoDesc final {
  std::vector<RegisterUnitDesc> RegisterUnits;
  std::vector<PhysicalRegisterDesc> PhysicalRegisters;
  std::vector<SubRegIndexDesc> SubRegIndices;
  std::vector<PhysicalSubRegisterDesc> PhysicalSubRegisters;
  std::vector<SubRegIndexComposition> SubRegCompositions;
  std::vector<RegisterClassDesc> RegisterClasses;
  std::vector<SubRegClassMapping> SubRegClassMappings;
  std::vector<RegisterBankDesc> RegisterBanks;
};

class TargetRegisterInfo final {
public:
  [[nodiscard]] static Expected<TargetRegisterInfo> create(TargetRegisterInfoDesc Description);

  [[nodiscard]] std::span<const RegisterUnitDesc> registerUnits() const noexcept {
    return Description.RegisterUnits;
  }

  [[nodiscard]] std::span<const PhysicalRegisterDesc> physicalRegisters() const noexcept {
    return Description.PhysicalRegisters;
  }

  [[nodiscard]] std::span<const SubRegIndexDesc> subRegIndices() const noexcept {
    return Description.SubRegIndices;
  }

  [[nodiscard]] std::span<const RegisterClassDesc> registerClasses() const noexcept {
    return Description.RegisterClasses;
  }

  [[nodiscard]] std::span<const RegisterBankDesc> registerBanks() const noexcept {
    return Description.RegisterBanks;
  }

  [[nodiscard]] const RegisterUnitDesc *registerUnit(RegisterUnitID ID) const noexcept;
  [[nodiscard]] const PhysicalRegisterDesc *physicalRegister(PhysicalRegister Reg) const noexcept;
  [[nodiscard]] const PhysicalRegisterDesc *physicalRegister(std::string_view Name) const noexcept;
  [[nodiscard]] const SubRegIndexDesc *subRegIndex(SubRegIndex Index) const noexcept;
  [[nodiscard]] const SubRegIndexDesc *subRegIndex(std::string_view Name) const noexcept;
  [[nodiscard]] const RegisterClassDesc *registerClass(RegisterClassID ID) const noexcept;
  [[nodiscard]] const RegisterClassDesc *registerClass(std::string_view Name) const noexcept;
  [[nodiscard]] const RegisterBankDesc *registerBank(RegisterBankID ID) const noexcept;
  [[nodiscard]] const RegisterBankDesc *registerBank(std::string_view Name) const noexcept;

  [[nodiscard]] std::optional<std::string_view>
  physicalRegisterMIRName(PhysicalRegister Reg) const noexcept;
  [[nodiscard]] std::optional<std::string_view>
  registerClassMIRName(RegisterClassID ID) const noexcept;
  [[nodiscard]] std::optional<std::string_view>
  registerBankMIRName(RegisterBankID ID) const noexcept;

  [[nodiscard]] bool isKnownPhysicalRegister(PhysicalRegister Reg) const noexcept {
    return physicalRegister(Reg) != nullptr;
  }

  [[nodiscard]] LaneMask registerLaneMask(PhysicalRegister Reg) const noexcept;
  [[nodiscard]] bool isValidLaneMask(PhysicalRegister Reg, LaneMask Lanes) const noexcept;
  [[nodiscard]] bool regsOverlap(PhysicalRegister Left, PhysicalRegister Right) const noexcept;
  [[nodiscard]] std::span<const PhysicalRegister> aliases(PhysicalRegister Reg) const noexcept;

  [[nodiscard]] std::optional<PhysicalRegister> subRegister(PhysicalRegister SuperReg,
                                                            SubRegIndex Index) const noexcept;
  [[nodiscard]] bool isSubRegister(PhysicalRegister SuperReg,
                                   PhysicalRegister SubReg) const noexcept;
  [[nodiscard]] std::span<const PhysicalRegister>
  superRegisters(PhysicalRegister Reg) const noexcept;
  [[nodiscard]] std::optional<SubRegIndex> composeSubRegIndices(SubRegIndex First,
                                                                SubRegIndex Second) const noexcept;

  [[nodiscard]] bool registerClassContains(RegisterClassID Class,
                                           PhysicalRegister Reg) const noexcept;
  [[nodiscard]] bool isSubclass(RegisterClassID Candidate, RegisterClassID Required) const noexcept;
  [[nodiscard]] bool registerClassesOverlap(RegisterClassID Left,
                                            RegisterClassID Right) const noexcept;
  [[nodiscard]] std::optional<RegisterClassID> subRegClass(RegisterClassID Source,
                                                           SubRegIndex Index) const noexcept;

  [[nodiscard]] bool bankCoversClass(RegisterBankID Bank, RegisterClassID Class) const noexcept;
  [[nodiscard]] bool bankContainsRegister(RegisterBankID Bank, PhysicalRegister Reg) const noexcept;
  [[nodiscard]] bool registerBanksOverlap(RegisterBankID Left, RegisterBankID Right) const noexcept;

  [[nodiscard]] Expected<void> validateRegisterMask(const RegisterMask &Mask) const;

private:
  explicit TargetRegisterInfo(TargetRegisterInfoDesc Description)
      : Description(std::move(Description)) {}

  [[nodiscard]] static std::uint64_t pairKey(std::uint32_t First, std::uint32_t Second) noexcept {
    return (static_cast<std::uint64_t>(First) << 32U) | Second;
  }

  TargetRegisterInfoDesc Description;
  std::unordered_map<RegisterUnitID, std::size_t> UnitToIndex;
  std::unordered_map<PhysicalRegister, std::size_t> RegisterToIndex;
  std::unordered_map<std::string, std::size_t> RegisterNameToIndex;
  std::unordered_map<SubRegIndex, std::size_t> SubRegIndexToIndex;
  std::unordered_map<std::string, std::size_t> SubRegNameToIndex;
  std::unordered_map<RegisterClassID, std::size_t> ClassToIndex;
  std::unordered_map<std::string, std::size_t> ClassNameToIndex;
  std::unordered_map<RegisterBankID, std::size_t> BankToIndex;
  std::unordered_map<std::string, std::size_t> BankNameToIndex;
  std::unordered_map<std::uint64_t, PhysicalRegister> SubRegisterMap;
  std::unordered_map<std::uint64_t, SubRegIndex> CompositionMap;
  std::unordered_map<std::uint64_t, RegisterClassID> SubRegClassMap;
  std::unordered_set<std::uint64_t> SubclassRelations;
  std::vector<std::vector<PhysicalRegister>> Aliases;
  std::vector<std::vector<PhysicalRegister>> SuperRegisters;
  std::vector<std::string> RegisterMIRNames;
  std::vector<std::string> ClassMIRNames;
  std::vector<std::string> BankMIRNames;
};

} // namespace rcc

#endif // RCC_CODEGEN_TARGETREGISTERINFO_H
