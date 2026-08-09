// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/CodeGen/AsmPrinter.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GenericConstant.h"
#include "rcc/CodeGen/GenericOpcodes.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/CodeGen/InstructionDescriptor.h"
#include "rcc/CodeGen/MachineBasicBlock.h"
#include "rcc/CodeGen/MachineCFIInfo.h"
#include "rcc/CodeGen/MachineConstantPool.h"
#include "rcc/CodeGen/MachineFunction.h"
#include "rcc/CodeGen/MachineInstr.h"
#include "rcc/CodeGen/MachineJumpTableInfo.h"
#include "rcc/CodeGen/MachineMCContext.h"
#include "rcc/CodeGen/MachineOperand.h"
#include "rcc/CodeGen/MachineVerifier.h"
#include "rcc/CodeGen/TargetConstant.h"
#include "rcc/CodeGen/TargetDescription.h"
#include "rcc/CodeGen/TargetMCInstLowering.h"
#include "rcc/MC/MCCFI.h"
#include "rcc/MC/MCContext.h"
#include "rcc/MC/MCExpr.h"
#include "rcc/MC/MCSection.h"
#include "rcc/MC/MCStreamer.h"
#include "rcc/MC/MCSymbol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace rcc {
namespace {

[[nodiscard]] Expected<mc::MCExprRef> symbolExpression(mc::MCSymbolRef Symbol) {
  auto Expression = mc::MCExpr::symbol(std::move(Symbol));
  if (!Expression)
    return std::unexpected(Expression.error());
  return *Expression;
}

[[nodiscard]] Expected<mc::MCExprRef> addConstant(mc::MCExprRef Expression, std::int64_t Addend) {
  if (Addend == 0)
    return Expression;
  auto Result = mc::MCExpr::binary(mc::MCBinaryOperator::Add, std::move(Expression),
                                   mc::MCExpr::constant(Addend));
  if (!Result)
    return std::unexpected(Result.error());
  return *Result;
}

[[nodiscard]] Expected<mc::MCExprRef>
symbolDifference(mc::MCSymbolRef Minuend, mc::MCSymbolRef Subtrahend, std::int64_t Addend) {
  auto Left = symbolExpression(std::move(Minuend));
  if (!Left)
    return std::unexpected(Left.error());
  auto Right = symbolExpression(std::move(Subtrahend));
  if (!Right)
    return std::unexpected(Right.error());
  auto Difference =
      mc::MCExpr::binary(mc::MCBinaryOperator::Subtract, std::move(*Left), std::move(*Right));
  if (!Difference)
    return std::unexpected(Difference.error());
  return addConstant(std::move(*Difference), Addend);
}

[[nodiscard]] Expected<mc::MCCFIInstruction>
lowerCFIInstruction(const MachineCFIInstruction &Instruction, MachineMCContext &Context) {
  mc::MCCFIInstruction Result;
  const auto Register = [&Result](const MachineCFIRegisterRule &Rule) {
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.Register.value()));
  };
  const auto Offset = [&Result](const MachineCFIOffsetRule &Rule) {
    Result.Operands.push_back(Rule.Offset);
  };
  const auto RegisterOffset = [&Result](const MachineCFIRegisterOffsetRule &Rule) {
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.Register.value()));
    Result.Operands.push_back(Rule.Offset);
  };

  switch (Instruction.operation()) {
  case MachineCFIOperation::SameValue:
    Result.Operation = mc::MCCFIOperation::SameValue;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::RememberState:
    Result.Operation = mc::MCCFIOperation::RememberState;
    break;
  case MachineCFIOperation::RestoreState:
    Result.Operation = mc::MCCFIOperation::RestoreState;
    break;
  case MachineCFIOperation::Offset:
    Result.Operation = mc::MCCFIOperation::Offset;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::DefCFARegister:
    Result.Operation = mc::MCCFIOperation::DefCFARegister;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::DefCFAOffset:
    Result.Operation = mc::MCCFIOperation::DefCFAOffset;
    Offset(*Instruction.getIf<MachineCFIOffsetRule>());
    break;
  case MachineCFIOperation::DefCFA:
    Result.Operation = mc::MCCFIOperation::DefCFA;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::RelativeOffset:
    Result.Operation = mc::MCCFIOperation::RelativeOffset;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::AdjustCFAOffset:
    Result.Operation = mc::MCCFIOperation::AdjustCFAOffset;
    Offset(*Instruction.getIf<MachineCFIOffsetRule>());
    break;
  case MachineCFIOperation::Escape:
    Result.Operation = mc::MCCFIOperation::Escape;
    Result.Bytes = Instruction.getIf<MachineCFIEscapeRule>()->Bytes;
    break;
  case MachineCFIOperation::Restore:
    Result.Operation = mc::MCCFIOperation::Restore;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::Undefined:
    Result.Operation = mc::MCCFIOperation::Undefined;
    Register(*Instruction.getIf<MachineCFIRegisterRule>());
    break;
  case MachineCFIOperation::Register: {
    Result.Operation = mc::MCCFIOperation::Register;
    const auto &Rule = *Instruction.getIf<MachineCFIRegisterRegisterRule>();
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.Register.value()));
    Result.Operands.push_back(static_cast<std::int64_t>(Rule.SourceRegister.value()));
    break;
  }
  case MachineCFIOperation::WindowSave:
    Result.Operation = mc::MCCFIOperation::WindowSave;
    break;
  case MachineCFIOperation::NegateReturnAddressState:
    Result.Operation = mc::MCCFIOperation::NegateReturnAddressState;
    break;
  case MachineCFIOperation::NegateReturnAddressStateWithPC:
    Result.Operation = mc::MCCFIOperation::NegateReturnAddressStateWithPC;
    break;
  case MachineCFIOperation::GNUArgsSize:
    Result.Operation = mc::MCCFIOperation::GNUArgsSize;
    Offset(*Instruction.getIf<MachineCFIOffsetRule>());
    break;
  case MachineCFIOperation::Label: {
    Result.Operation = mc::MCCFIOperation::Label;
    auto Symbol = Context.symbol(Instruction.getIf<MachineCFILabelRule>()->Symbol,
                                 mc::MCSymbolKind::Temporary);
    if (!Symbol)
      return std::unexpected(Symbol.error());
    Result.Symbol = std::move(*Symbol);
    break;
  }
  case MachineCFIOperation::ValueOffset:
    Result.Operation = mc::MCCFIOperation::ValueOffset;
    RegisterOffset(*Instruction.getIf<MachineCFIRegisterOffsetRule>());
    break;
  case MachineCFIOperation::LLVMDefAddressSpaceCFA:
  case MachineCFIOperation::LLVMRegisterPair:
  case MachineCFIOperation::LLVMVectorRegisters:
  case MachineCFIOperation::LLVMVectorOffset:
  case MachineCFIOperation::LLVMVectorRegisterMask:
    return makeUnexpected(ErrorCode::Unsupported,
                          "assembly emission does not support this LLVM-specific CFI operation");
  case MachineCFIOperation::Count:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "machine CFI instruction has an invalid operation");
  }

  auto Verification = Result.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Result;
}

[[nodiscard]] Expected<void> emitNamedLabel(const MachineOperand &Operand,
                                            MachineMCContext &Context, mc::MCStreamer &Streamer) {
  const auto *SymbolOperand = Operand.getIf<MCSymbolOperand>();
  if (SymbolOperand == nullptr || SymbolOperand->Offset != 0 || Operand.targetFlags() != 0) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "label pseudo requires an unmodified MC symbol operand");
  }
  auto Symbol = Context.symbol(SymbolOperand->Name, mc::MCSymbolKind::Temporary);
  if (!Symbol)
    return std::unexpected(Symbol.error());
  return Streamer.emitLabel(std::move(*Symbol));
}

[[nodiscard]] bool validValueSize(std::uint64_t Size) noexcept {
  return Size == 1 || Size == 2 || Size == 4 || Size == 8;
}

[[nodiscard]] Expected<void> emitGlobalAttributes(const GlobalObjectDesc &Object,
                                                  const mc::MCSymbolRef &Symbol,
                                                  mc::MCStreamer &Streamer) {
  switch (Object.Linkage) {
  case GlobalLinkage::External:
  case GlobalLinkage::Appending:
  case GlobalLinkage::Common: {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Global);
    if (!Emitted)
      return std::unexpected(Emitted.error());
    break;
  }
  case GlobalLinkage::LinkOnceAny:
  case GlobalLinkage::LinkOnceODR:
  case GlobalLinkage::WeakAny:
  case GlobalLinkage::WeakODR:
  case GlobalLinkage::ExternalWeak: {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Weak);
    if (!Emitted)
      return std::unexpected(Emitted.error());
    break;
  }
  case GlobalLinkage::Internal:
  case GlobalLinkage::Private: {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Local);
    if (!Emitted)
      return std::unexpected(Emitted.error());
    break;
  }
  case GlobalLinkage::AvailableExternally:
    break;
  case GlobalLinkage::Count:
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "global object has invalid linkage during assembly emission");
  }

  if (Object.Visibility == GlobalVisibility::Hidden) {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Hidden);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  } else if (Object.Visibility == GlobalVisibility::Protected) {
    auto Emitted = Streamer.emitSymbolAttribute(Symbol, mc::MCSymbolAttribute::Protected);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }
  return Streamer.emitSymbolAttribute(Symbol, Object.TLSModel == GlobalTLSModel::None
                                                  ? mc::MCSymbolAttribute::Object
                                                  : mc::MCSymbolAttribute::TLSObject);
}

} // namespace

Expected<void> AsmPrinter::verifyFunction(const MachineFunction &Function) const {
  const MachineFunctionProperties Required = {
      MachineFunctionProperty::NoPHIs,
      MachineFunctionProperty::Legalized,
      MachineFunctionProperty::RegBankSelected,
      MachineFunctionProperty::Selected,
      MachineFunctionProperty::TiedOpsRewritten,
      MachineFunctionProperty::NoVRegs,
      MachineFunctionProperty::FrameFinalized,
      MachineFunctionProperty::NoFrameIndices,
      MachineFunctionProperty::NoExecutablePseudos,
  };
  if (!Function.properties().containsAll(Required)) {
    const MachineFunctionProperties Missing = Required.missingFrom(Function.properties());
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "assembly emission requires finalized MIR properties: " +
                              Missing.toString());
  }
  if (!Options.VerifyInput)
    return {};
  return MachineVerifier::verify(Function, *Target,
                                 MachineVerifierOptions{.Mode = MachineVerifierMode::Generic,
                                                        .Stage = PipelineStage::Emission,
                                                        .VerifyProperties = true});
}

Expected<void> AsmPrinter::emitCompilationUnit(const CompilationUnit &Unit) {
  if (UnitEmissionStarted) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "a compilation unit can only be emitted once by an AsmPrinter");
  }
  UnitEmissionStarted = true;

  auto Initialized = Context->initialize(Unit);
  if (!Initialized)
    return std::unexpected(Initialized.error());
  auto Header = Lowering->emitFileHeader(*Context, *Streamer);
  if (!Header)
    return std::unexpected(Header.error());

  auto Globals = emitGlobalObjects(Unit);
  if (!Globals)
    return std::unexpected(Globals.error());

  for (std::size_t Index = 0; Index < Unit.functionCount(); ++Index) {
    const MachineFunction *Function = Unit.functionAt(Index);
    if (Function == nullptr) {
      return makeUnexpected(ErrorCode::InternalError,
                            "compilation unit contains a null function during assembly emission");
    }
    auto Emitted = emitFunction(*Function);
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }

  auto Trailer = Lowering->emitFileTrailer(*Context, *Streamer);
  if (!Trailer)
    return std::unexpected(Trailer.error());
  auto Finished = Streamer->finish();
  if (!Finished)
    return std::unexpected(Finished.error());
  UnitEmissionFinished = true;
  return {};
}

Expected<void> AsmPrinter::emitGlobalObjects(const CompilationUnit &Unit) {
  for (std::size_t Index = 0; Index < Unit.globalObjectCount(); ++Index) {
    const GlobalObjectDesc *Object = Unit.globalObjectAt(Index);
    if (Object == nullptr)
      return makeUnexpected(ErrorCode::InternalError, "compilation unit contains a null global");
    if (Object->Linkage == GlobalLinkage::AvailableExternally)
      continue;

    const mc::MCSymbolRef Symbol = Context->globalSymbol(*Object);
    if (!Symbol)
      return makeUnexpected(ErrorCode::InternalError, "global object has no MC symbol");
    auto Attributes = emitGlobalAttributes(*Object, Symbol, *Streamer);
    if (!Attributes)
      return std::unexpected(Attributes.error());
    if (Object->isDeclaration())
      continue;

    if (Object->Linkage == GlobalLinkage::Common) {
      auto Common = Streamer->emitCommonSymbol(Symbol, Object->Size, Object->Align.bytes());
      if (!Common)
        return std::unexpected(Common.error());
      continue;
    }

    auto Description = Lowering->globalSection(*Object, *Context);
    if (!Description)
      return std::unexpected(Description.error());
    auto Section = Context->context().getOrCreateSection(std::move(*Description));
    if (!Section)
      return std::unexpected(Section.error());
    auto Switched = Streamer->switchSection(std::move(*Section));
    if (!Switched)
      return std::unexpected(Switched.error());
    auto Aligned = Streamer->emitAlignment(Object->Align.bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());
    auto Label = Streamer->emitLabel(Symbol);
    if (!Label)
      return std::unexpected(Label.error());
    if (!Object->Initializer) {
      return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                            "defined global object has no initializer");
    }
    auto Initializer = Lowering->emitGlobalInitializer(*Object->Initializer, *Context, *Streamer);
    if (!Initializer)
      return std::unexpected(Initializer.error());
    auto Size = Streamer->emitSymbolSize(
        Symbol, mc::MCExpr::constant(static_cast<std::int64_t>(Object->Size)));
    if (!Size)
      return std::unexpected(Size.error());
  }
  return {};
}

Expected<void> AsmPrinter::emitFunction(const MachineFunction &Function) {
  if (UnitEmissionFinished) {
    return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                          "cannot emit a function after the assembly stream is finalized");
  }
  auto Verification = verifyFunction(Function);
  if (!Verification)
    return std::unexpected(Verification.error());
  auto Initialized = Context->initializeFunction(Function);
  if (!Initialized)
    return std::unexpected(Initialized.error());

  auto SectionDescription = Lowering->textSection(Function);
  if (!SectionDescription)
    return std::unexpected(SectionDescription.error());
  auto Section = Context->context().getOrCreateSection(std::move(*SectionDescription));
  if (!Section)
    return std::unexpected(Section.error());
  auto Switched = Streamer->switchSection(std::move(*Section));
  if (!Switched)
    return std::unexpected(Switched.error());

  const mc::MCSymbolRef FunctionSymbol = Context->functionSymbol(Function);
  if (!FunctionSymbol) {
    return makeUnexpected(ErrorCode::InternalError,
                          "function has no MC symbol after context initialization");
  }
  if (Options.EmitFunctionDirectives) {
    auto Global = Streamer->emitSymbolAttribute(FunctionSymbol, mc::MCSymbolAttribute::Global);
    if (!Global)
      return std::unexpected(Global.error());
    auto Type = Streamer->emitSymbolAttribute(FunctionSymbol, mc::MCSymbolAttribute::Function);
    if (!Type)
      return std::unexpected(Type.error());
  }
  if (const auto Alignment = Function.attributes().alignment()) {
    auto Aligned = Streamer->emitAlignment(Alignment->bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());
  }
  auto FunctionLabel = Streamer->emitLabel(FunctionSymbol);
  if (!FunctionLabel)
    return std::unexpected(FunctionLabel.error());
  if (Options.EmitCFI) {
    mc::MCCFIInstruction StartProc;
    StartProc.Operation = mc::MCCFIOperation::StartProc;
    auto CFIStart = Streamer->emitCFI(StartProc);
    if (!CFIStart)
      return std::unexpected(CFIStart.error());
  }

  auto Body = emitFunctionBody(Function);
  if (!Body)
    return std::unexpected(Body.error());
  if (Options.EmitCFI) {
    mc::MCCFIInstruction EndProc;
    EndProc.Operation = mc::MCCFIOperation::EndProc;
    auto CFIEnd = Streamer->emitCFI(EndProc);
    if (!CFIEnd)
      return std::unexpected(CFIEnd.error());
  }

  auto EndSymbol = Context->symbol(".Lfunc_end" + std::to_string(Function.stableID().value()),
                                   mc::MCSymbolKind::Temporary);
  if (!EndSymbol)
    return std::unexpected(EndSymbol.error());
  auto EndLabel = Streamer->emitLabel(*EndSymbol);
  if (!EndLabel)
    return std::unexpected(EndLabel.error());
  if (Options.EmitFunctionDirectives) {
    auto Size = symbolDifference(*EndSymbol, FunctionSymbol, 0);
    if (!Size)
      return std::unexpected(Size.error());
    auto EmittedSize = Streamer->emitSymbolSize(FunctionSymbol, std::move(*Size));
    if (!EmittedSize)
      return std::unexpected(EmittedSize.error());
  }

  auto Constants = emitConstantPool(Function);
  if (!Constants)
    return std::unexpected(Constants.error());
  return emitJumpTables(Function);
}

Expected<void> AsmPrinter::emitFunctionBody(const MachineFunction &Function) {
  for (const MachineBasicBlock *Block = Function.firstBlock(); Block != nullptr;
       Block = Block->next()) {
    if (Block->attributes().alignment().bytes() > 1) {
      auto Aligned = Streamer->emitAlignment(Block->attributes().alignment().bytes());
      if (!Aligned)
        return std::unexpected(Aligned.error());
    }
    if (Options.EmitBlockLabels) {
      const mc::MCSymbolRef Symbol = Context->blockSymbol(*Block);
      if (!Symbol)
        return makeUnexpected(ErrorCode::InternalError, "machine block has no MC symbol");
      auto Label = Streamer->emitLabel(Symbol);
      if (!Label)
        return std::unexpected(Label.error());
    }

    for (const MachineInstr *Instruction = Block->firstInstruction(); Instruction != nullptr;
         Instruction = Instruction->next()) {
      const InstructionDescriptor *Descriptor =
          Target->instructionDescriptor(Instruction->opcode());
      if (Descriptor == nullptr) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "assembly emission encountered an unknown opcode");
      }
      if (Options.EmitInstructionSymbols) {
        if (const auto SymbolName = Instruction->preInstrSymbol()) {
          auto Symbol = Context->symbol(std::string(*SymbolName), mc::MCSymbolKind::Temporary);
          if (!Symbol)
            return std::unexpected(Symbol.error());
          auto Label = Streamer->emitLabel(std::move(*Symbol));
          if (!Label)
            return std::unexpected(Label.error());
        }
      }

      Expected<void> Emitted;
      if (Instruction->opcode() == opcode(GenericOpcode::CFI_INSTRUCTION)) {
        const auto *Index = Instruction->operandAt(0) == nullptr
                                ? nullptr
                                : Instruction->operandAt(0)->getIf<CFIIndexOperand>();
        const MachineCFIInstruction *CFI =
            Index == nullptr ? nullptr : Function.cfiInfo().instruction(Index->Index);
        if (CFI == nullptr) {
          return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                                "CFI pseudo references an invalid CFI instruction");
        }
        if (Options.EmitCFI) {
          auto Lowered = lowerCFIInstruction(*CFI, *Context);
          if (!Lowered)
            return std::unexpected(Lowered.error());
          Emitted = Streamer->emitCFI(*Lowered);
        }
      } else if (Instruction->opcode() == opcode(GenericOpcode::EH_LABEL) ||
                 Instruction->opcode() == opcode(GenericOpcode::GC_LABEL) ||
                 Instruction->opcode() == opcode(GenericOpcode::ANNOTATION_LABEL)) {
        if (Instruction->operandAt(0) == nullptr) {
          return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                                "label pseudo has no symbol operand");
        }
        Emitted = emitNamedLabel(*Instruction->operandAt(0), *Context, *Streamer);
      } else if (Descriptor->Flags.has(InstructionFlag::TerminalEmission)) {
        Emitted = Lowering->emitTerminalInstruction(*Instruction, *Context, *Streamer);
      } else if (Descriptor->Flags.has(InstructionFlag::Meta)) {
        Emitted = {};
      } else if (Descriptor->Flags.has(InstructionFlag::ProducesCode)) {
        auto Lowered = Lowering->lowerInstruction(*Instruction, *Context);
        if (!Lowered)
          return std::unexpected(Lowered.error());
        Emitted = Streamer->emitInstruction(*Lowered);
      } else {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "non-meta instruction has no assembly emission behavior");
      }
      if (!Emitted)
        return std::unexpected(Emitted.error());

      if (Options.EmitInstructionSymbols) {
        if (const auto SymbolName = Instruction->postInstrSymbol()) {
          auto Symbol = Context->symbol(std::string(*SymbolName), mc::MCSymbolKind::Temporary);
          if (!Symbol)
            return std::unexpected(Symbol.error());
          auto Label = Streamer->emitLabel(std::move(*Symbol));
          if (!Label)
            return std::unexpected(Label.error());
        }
      }
    }
  }
  return {};
}

Expected<void> AsmPrinter::emitConstantPool(const MachineFunction &Function) {
  for (const MachineConstantPoolEntry &Entry : Function.constantPool().entries()) {
    const MachineConstantPoolEntryDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    auto SectionDescription = Lowering->constantPoolSection(Function, Entry);
    if (!SectionDescription)
      return std::unexpected(SectionDescription.error());
    auto Section = Context->context().getOrCreateSection(std::move(*SectionDescription));
    if (!Section)
      return std::unexpected(Section.error());
    auto Switched = Streamer->switchSection(std::move(*Section));
    if (!Switched)
      return std::unexpected(Switched.error());
    auto Aligned = Streamer->emitAlignment(Description->Align.bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());

    const mc::MCSymbolRef Symbol = Context->constantPoolSymbol(Function, Entry.index());
    if (!Symbol)
      return makeUnexpected(ErrorCode::InternalError, "constant-pool entry has no MC symbol");
    auto Label = Streamer->emitLabel(Symbol);
    if (!Label)
      return std::unexpected(Label.error());

    Expected<void> Emitted;
    if (const auto *Generic = std::get_if<GenericConstantData>(&Description->Value)) {
      Emitted = Lowering->emitGenericConstant(*Generic, Description->Size, *Context, *Streamer);
    } else if (const auto *TargetValue = std::get_if<TargetConstant>(&Description->Value)) {
      Emitted = Lowering->emitTargetConstant(*TargetValue, *Context, *Streamer);
    } else if (const auto *Difference =
                   std::get_if<SymbolDifferenceConstant>(&Description->Value)) {
      if (!validValueSize(Description->Size)) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "symbol-difference constant has an unsupported storage size");
      }
      auto Minuend = Context->symbol(Difference->Minuend);
      if (!Minuend)
        return std::unexpected(Minuend.error());
      auto Subtrahend = Context->symbol(Difference->Subtrahend);
      if (!Subtrahend)
        return std::unexpected(Subtrahend.error());
      auto Expression = symbolDifference(*Minuend, *Subtrahend, Difference->Addend);
      if (!Expression)
        return std::unexpected(Expression.error());
      Emitted =
          Streamer->emitValue(std::move(*Expression), static_cast<std::uint8_t>(Description->Size));
    } else if (const auto *Address = std::get_if<BlockAddressConstant>(&Description->Value)) {
      if (!validValueSize(Description->Size)) {
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed,
                              "block-address constant has an unsupported storage size");
      }
      auto AddressSymbol = Context->blockAddressSymbol(Address->FunctionName, Address->BlockName);
      if (!AddressSymbol)
        return std::unexpected(AddressSymbol.error());
      auto Expression = symbolExpression(std::move(*AddressSymbol));
      if (!Expression)
        return std::unexpected(Expression.error());
      Expression = addConstant(std::move(*Expression), Address->Addend);
      if (!Expression)
        return std::unexpected(Expression.error());
      Emitted =
          Streamer->emitValue(std::move(*Expression), static_cast<std::uint8_t>(Description->Size));
    } else if (const auto *TargetExpression =
                   std::get_if<TargetExpressionConstant>(&Description->Value)) {
      Emitted = Lowering->emitTargetConstant(TargetExpression->Expression, *Context, *Streamer);
    } else {
      return makeUnexpected(ErrorCode::InternalError, "constant-pool value has an invalid kind");
    }
    if (!Emitted)
      return std::unexpected(Emitted.error());
  }
  return {};
}

Expected<void> AsmPrinter::emitJumpTables(const MachineFunction &Function) {
  for (const MachineJumpTableEntry &Entry : Function.jumpTableInfo().tables()) {
    const MachineJumpTableDesc *Description = Entry.description();
    if (Description == nullptr)
      continue;
    auto SectionDescription = Lowering->jumpTableSection(Function, Entry);
    if (!SectionDescription)
      return std::unexpected(SectionDescription.error());
    auto Section = Context->context().getOrCreateSection(std::move(*SectionDescription));
    if (!Section)
      return std::unexpected(Section.error());
    auto Switched = Streamer->switchSection(std::move(*Section));
    if (!Switched)
      return std::unexpected(Switched.error());
    auto Aligned = Streamer->emitAlignment(Description->Align.bytes());
    if (!Aligned)
      return std::unexpected(Aligned.error());

    const mc::MCSymbolRef TableSymbol = Context->jumpTableSymbol(Function, Entry.index());
    if (!TableSymbol)
      return makeUnexpected(ErrorCode::InternalError, "jump-table entry has no MC symbol");
    auto Label = Streamer->emitLabel(TableSymbol);
    if (!Label)
      return std::unexpected(Label.error());

    if (Description->Encoding != JumpTableEntryEncoding::BlockAddress &&
        Description->Encoding != JumpTableEntryEncoding::LabelDifference32 &&
        Description->Encoding != JumpTableEntryEncoding::LabelDifference64) {
      auto TargetTable = Lowering->emitTargetJumpTable(Function, Entry, *Context, *Streamer);
      if (!TargetTable)
        return std::unexpected(TargetTable.error());
      continue;
    }

    for (const MachineBasicBlock *TargetBlock : Description->Targets) {
      if (TargetBlock == nullptr)
        return makeUnexpected(ErrorCode::AssemblyLoweringFailed, "jump table has a null target");
      const mc::MCSymbolRef TargetSymbol = Context->blockSymbol(*TargetBlock);
      if (!TargetSymbol)
        return makeUnexpected(ErrorCode::InternalError, "jump-table target has no MC symbol");

      Expected<mc::MCExprRef> Expression = symbolExpression(TargetSymbol);
      if (Description->Encoding == JumpTableEntryEncoding::LabelDifference32 ||
          Description->Encoding == JumpTableEntryEncoding::LabelDifference64)
        Expression = symbolDifference(TargetSymbol, TableSymbol, 0);
      if (!Expression)
        return std::unexpected(Expression.error());
      auto Value = Streamer->emitValue(std::move(*Expression), Description->EntrySize);
      if (!Value)
        return std::unexpected(Value.error());
    }
  }
  return {};
}

} // namespace rcc
