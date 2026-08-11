// SPDX-License-Identifier: GPL-3.0-or-later

#include "MIRDocumentReferenceValidation.h"

#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFrameInfo.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineFunctionRecords.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineMetadata.h"
#include "rcc/MIR/MIRDocument.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>

namespace rcc::mir {

Expected<void> verifyMIRDocumentReferences(const MIRDocument &Document,
                                           const MachineFunction &Function, bool UseEmbeddedIR) {
  const EmbeddedIRReferenceIndex *Embedded = UseEmbeddedIR ? Document.embeddedIR() : nullptr;
  const EmbeddedIRFunction *IRFunction = nullptr;
  if (Embedded != nullptr) {
    if (Function.name().empty()) {
      std::size_t AnonymousOrdinal = 0;
      bool FoundFunction = false;
      for (std::size_t Index = 0; Index < Document.unit().functionCount(); ++Index) {
        const MachineFunction *Candidate = Document.unit().functionAt(Index);
        if (Candidate == &Function) {
          FoundFunction = true;
          break;
        }
        if (Candidate != nullptr && Candidate->name().empty())
          ++AnonymousOrdinal;
      }
      if (FoundFunction)
        IRFunction = Embedded->unnamedFunction(AnonymousOrdinal);
    } else {
      IRFunction = Embedded->function(Function.name());
    }
    if (IRFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "machine function has no matching embedded LLVM IR function");
    }
  }

  const auto ValidateGlobal = [Embedded](std::string_view Name,
                                         std::string_view Description) -> Expected<void> {
    if (Embedded == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " requires embedded LLVM IR to resolve global '" +
                                std::string(Name) + "'");
    }
    if (Embedded->hasGlobalValue(Name))
      return {};
    return makeUnexpected(ErrorCode::VerificationError,
                          std::string(Description) +
                              " references an undefined embedded LLVM IR global '" +
                              std::string(Name) + "'");
  };
  const auto ValidateBlockAddress = [Embedded](std::string_view FunctionName,
                                               std::string_view BlockName,
                                               std::string_view Description) -> Expected<void> {
    if (Embedded == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " requires embedded LLVM IR to resolve a block address");
    }
    const EmbeddedIRFunction *AddressFunction = Embedded->function(FunctionName);
    if (AddressFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " references an undefined embedded LLVM IR function '" +
                                std::string(FunctionName) + "'");
    }
    if (AddressFunction->block(BlockName) == nullptr) {
      return makeUnexpected(
          ErrorCode::VerificationError,
          std::string(Description) + " references an undefined embedded LLVM IR block '" +
              std::string(BlockName) + "' in function '" + std::string(FunctionName) + "'");
    }
    return {};
  };
  const auto ValidateIRValue = [IRFunction](std::string_view Name,
                                            std::string_view Description) -> Expected<void> {
    if (IRFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " requires embedded LLVM IR to resolve IR value '%" +
                                std::string(Name) + "'");
    }
    if (IRFunction->value(Name) != nullptr)
      return {};
    return makeUnexpected(ErrorCode::VerificationError,
                          std::string(Description) +
                              " references an undefined embedded LLVM IR value '%" +
                              std::string(Name) + "'");
  };
  const auto ValidateMetadata = [Embedded](MachineMetadataReference Reference,
                                           std::initializer_list<std::string_view> ExpectedTypes,
                                           std::string_view Description) -> Expected<void> {
    if (!Reference.isValid()) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) + " has an invalid metadata reference");
    }
    if (Reference.scope() != MachineMetadataScope::EmbeddedIR)
      return {};
    if (Embedded == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) + " requires embedded LLVM IR metadata !" +
                                std::to_string(Reference.slot()));
    }
    const EmbeddedIRMetadataDefinition *Definition =
        Embedded->metadata(std::to_string(Reference.slot()));
    if (Definition == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            std::string(Description) +
                                " references undefined embedded LLVM IR metadata !" +
                                std::to_string(Reference.slot()));
    }
    if (ExpectedTypes.size() == 0 ||
        std::ranges::find(ExpectedTypes, Definition->DynamicType) != ExpectedTypes.end()) {
      return {};
    }
    std::string Expected;
    for (const std::string_view Type : ExpectedTypes) {
      if (!Expected.empty())
        Expected.append(" or ");
      Expected.append(Type);
    }
    return makeUnexpected(ErrorCode::VerificationError,
                          std::string(Description) + " requires " + Expected + " metadata, but !" +
                              std::to_string(Reference.slot()) + " is " +
                              (Definition->DynamicType.empty()
                                   ? std::string("an unclassified metadata node")
                                   : Definition->DynamicType));
  };
  const auto ValidateMetadataValue =
      [&ValidateMetadata](const MachineMetadataValue &Value,
                          std::string_view Description) -> Expected<void> {
    const MachineMetadataReference *Reference = Value.reference();
    return Reference == nullptr ? Expected<void>{} : ValidateMetadata(*Reference, {}, Description);
  };
  const auto ValidateLocation =
      [&ValidateMetadata](const MachineDebugLocation &Location,
                          std::string_view Description) -> Expected<void> {
    if (const MachineMetadataReference *Reference = Location.reference())
      return ValidateMetadata(*Reference, {"DILocation"}, Description);
    const MachineDILocation *Inline = Location.inlineLocation();
    if (Inline == nullptr)
      return {};
    for (const MachineDILocationFrame &Frame : Inline->inlineChain()) {
      auto Verification =
          ValidateMetadata(Frame.Scope, {"DISubprogram", "DILexicalBlock", "DILexicalBlockFile"},
                           "inline DILocation scope");
      if (!Verification)
        return Verification;
    }
    if (Inline->terminalInlinedAt()) {
      return ValidateMetadata(*Inline->terminalInlinedAt(), {"DILocation"},
                              "inline DILocation inlinedAt");
    }
    return {};
  };
  const auto ValidateVariableDebugInfo =
      [&ValidateMetadata, &ValidateMetadataValue,
       &ValidateLocation](const MachineVariableDebugInfo &DebugInfo) -> Expected<void> {
    if (DebugInfo.Variable) {
      auto Verification =
          ValidateMetadata(*DebugInfo.Variable, {"DILocalVariable"}, "debug variable");
      if (!Verification)
        return Verification;
    }
    if (DebugInfo.Expression) {
      if (const MachineMetadataReference *Reference = DebugInfo.Expression->reference();
          Reference != nullptr) {
        auto Verification = ValidateMetadata(*Reference, {"DIExpression"}, "debug expression");
        if (!Verification)
          return Verification;
      }
      auto Verification = ValidateMetadataValue(*DebugInfo.Expression, "debug expression");
      if (!Verification)
        return Verification;
    }
    if (DebugInfo.Location)
      return ValidateLocation(*DebugInfo.Location, "debug location");
    return {};
  };
  const auto ValidateGenericType = [Embedded](const auto &Self,
                                              const GenericConstantType &Type) -> Expected<void> {
    if (Type.Kind == GenericConstantTypeKind::Named) {
      if (Embedded == nullptr) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic constant type requires embedded LLVM IR to resolve named type '%" + Type.Name +
                "'");
      }
      if (Embedded->typeDefinition(Type.Name) == nullptr) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "generic constant type references an undefined embedded LLVM IR named type '%" +
                Type.Name + "'");
      }
    }
    for (const GenericConstantType &Element : Type.Elements) {
      auto Verification = Self(Self, Element);
      if (!Verification)
        return Verification;
    }
    return {};
  };
  const auto ValidateGenericConstant =
      [&ValidateBlockAddress, &ValidateGenericType,
       &ValidateGlobal](const auto &Self, const GenericConstantData &Constant) -> Expected<void> {
    auto TypeVerification = ValidateGenericType(ValidateGenericType, Constant.Type);
    if (!TypeVerification)
      return TypeVerification;
    if (Constant.SourceElementType) {
      TypeVerification = ValidateGenericType(ValidateGenericType, *Constant.SourceElementType);
      if (!TypeVerification)
        return TypeVerification;
    }
    switch (Constant.Kind) {
    case GenericConstantKind::GlobalAddress:
    case GenericConstantKind::DSOLocalEquivalent:
    case GenericConstantKind::NoCFI:
      if (auto Verification = ValidateGlobal(Constant.Symbol, "generic constant"); !Verification)
        return Verification;
      break;
    case GenericConstantKind::BlockAddress:
      if (auto Verification = ValidateBlockAddress(Constant.Symbol, Constant.Block,
                                                   "generic constant blockaddress");
          !Verification) {
        return Verification;
      }
      break;
    default:
      break;
    }
    for (const GenericConstantData &Element : Constant.Elements) {
      auto Verification = Self(Self, Element);
      if (!Verification)
        return Verification;
    }
    return {};
  };

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (const auto Name = Block->attributes().irBlockName(); Name) {
      if (IRFunction == nullptr) {
        return makeUnexpected(ErrorCode::VerificationError,
                              "machine basic block requires embedded LLVM IR to resolve block '" +
                                  std::string(*Name) + "'");
      }
      if (IRFunction->block(*Name) == nullptr) {
        return makeUnexpected(
            ErrorCode::VerificationError,
            "machine basic block references an undefined embedded LLVM IR block '" +
                std::string(*Name) + "'");
      }
    }
  }
  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description == nullptr || !Description->AllocaName)
      continue;
    if (IRFunction == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object requires embedded LLVM IR to resolve alloca '" +
                                *Description->AllocaName + "'");
    }
    if (IRFunction->alloca(*Description->AllocaName) == nullptr) {
      return makeUnexpected(ErrorCode::VerificationError,
                            "stack object references an undefined embedded LLVM IR alloca '" +
                                *Description->AllocaName + "'");
    }
  }

  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    const MachineConstantPoolEntryDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    if (const auto *Generic = std::get_if<GenericConstantData>(&Description->Value)) {
      auto Verification = ValidateGenericConstant(ValidateGenericConstant, *Generic);
      if (!Verification)
        return Verification;
    } else if (const auto *Address = std::get_if<BlockAddressConstant>(&Description->Value)) {
      auto Verification = ValidateBlockAddress(Address->FunctionName, Address->BlockName,
                                               "constant-pool block address");
      if (!Verification)
        return Verification;
    }
  }

  for (const MachineMetadataNode &Node : Function.metadata().nodes()) {
    for (const MachineMetadataElement &Element : Node.Elements) {
      const MachineMetadataReference *Reference = Element.reference();
      if (Reference == nullptr)
        continue;
      auto Verification = ValidateMetadata(*Reference, {}, "machine metadata node");
      if (!Verification)
        return Verification;
    }
  }

  for (const StackObject &Object : Function.frameInfo().objects()) {
    const StackObjectDesc *Description = Object.description();
    if (Description == nullptr || !Description->DebugInfo)
      continue;
    auto Verification = ValidateVariableDebugInfo(*Description->DebugInfo);
    if (!Verification)
      return Verification;
  }
  for (const MachineEntryValueInfo &Entry : Function.records().entryValues()) {
    auto Verification = ValidateVariableDebugInfo(Entry.DebugInfo);
    if (!Verification)
      return Verification;
  }
  for (const MachineCalledGlobal &Called : Function.records().calledGlobals()) {
    auto Verification = ValidateGlobal(Called.Callee, "called-global record");
    if (!Verification)
      return Verification;
  }

  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      if (Instruction->debugLocation()) {
        auto Verification =
            ValidateLocation(*Instruction->debugLocation(), "instruction debug location");
        if (!Verification)
          return Verification;
      }
      const std::array InstructionMetadata{
          Instruction->heapAllocMarker(),
          Instruction->pcSections(),
          Instruction->mmra(),
      };
      for (const auto &Reference : InstructionMetadata) {
        if (!Reference)
          continue;
        auto Verification = ValidateMetadata(*Reference, {}, "instruction metadata attachment");
        if (!Verification)
          return Verification;
      }

      for (const MachineOperand &Operand : Instruction->operands()) {
        if (const auto *Global = Operand.getIf<GlobalSymbolOperand>(); Global != nullptr) {
          auto Verification = ValidateGlobal(Global->Name, "global-symbol operand");
          if (!Verification)
            return Verification;
        }
        if (const auto *Address = Operand.getIf<BlockAddressOperand>(); Address != nullptr) {
          auto Verification = ValidateBlockAddress(Address->FunctionName, Address->BlockName,
                                                   "block-address operand");
          if (!Verification)
            return Verification;
        }
        if (const auto *Metadata = Operand.getIf<MetadataOperand>(); Metadata != nullptr) {
          auto Verification = ValidateMetadataValue(Metadata->Value, "metadata operand");
          if (!Verification)
            return Verification;
        }
      }

      for (const MachineMemOperand *Memory : Instruction->memOperands()) {
        const MachineMemOperandDesc &Description = Memory->description();
        const std::optional<std::string_view> BaseName = Description.Base.symbolName();
        switch (Description.Base.kind()) {
        case MemoryBase::Kind::GlobalSymbol:
        case MemoryBase::Kind::CallEntryGlobal:
          if (BaseName) {
            auto Verification = ValidateGlobal(*BaseName, "machine memory base");
            if (!Verification)
              return Verification;
          }
          break;
        case MemoryBase::Kind::IRValue:
          if (BaseName) {
            auto Verification = ValidateIRValue(*BaseName, "machine memory base");
            if (!Verification)
              return Verification;
          }
          break;
        default:
          break;
        }
        const std::array MetadataReferences{
            Description.TBAA,          Description.AliasScope,
            Description.NoAlias,       Description.NoAliasAddressSpace,
            Description.RangeMetadata,
        };
        for (const auto &Reference : MetadataReferences) {
          if (!Reference)
            continue;
          auto Verification = ValidateMetadata(*Reference, {}, "machine memory operand metadata");
          if (!Verification)
            return Verification;
        }
        if (Description.SyncScope.value() == 0)
          continue;
        const auto Scope =
            std::ranges::find_if(Document.syncScopes(), [&Description](const auto &Slot) {
              return Slot.ID == Description.SyncScope;
            });
        if (Scope == Document.syncScopes().end()) {
          return makeUnexpected(
              ErrorCode::VerificationError,
              "machine memory operand synchronization scope has no document name");
        }
      }
    }
  }
  return {};
}

} // namespace rcc::mir
