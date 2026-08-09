// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/Target/RISCV/RISCVAssemblyPrinter.h"

#include "rcc/CodeGen/CompilationUnit.h"
#include "rcc/CodeGen/GlobalObject.h"
#include "rcc/Target/RISCV/RISCVTarget.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int fail(std::string_view Message) {
  std::cerr << Message << '\n';
  return 1;
}

[[nodiscard]] bool contains(std::string_view Text, std::string_view Needle) {
  return Text.find(Needle) != std::string_view::npos;
}

[[nodiscard]] rcc::Expected<rcc::GlobalObjectDesc>
makeObject(std::string Symbol, std::uint64_t Size, std::uint64_t Alignment,
           rcc::GlobalInitializer Initializer, bool IsConstant = false) {
  auto Align = rcc::Alignment::create(Alignment);
  if (!Align)
    return std::unexpected(Align.error());
  rcc::GlobalObjectDesc Object;
  Object.Symbol = std::move(Symbol);
  Object.ObjectType = rcc::GenericConstantType::byte();
  Object.Align = *Align;
  Object.Size = Size;
  Object.IsConstant = IsConstant;
  Object.Initializer.emplace(std::move(Initializer));
  auto Verification = Object.verify();
  if (!Verification)
    return std::unexpected(Verification.error());
  return Object;
}

[[nodiscard]] rcc::Expected<void> addObjects(rcc::CompilationUnit &Unit) {
  auto AnswerValue = rcc::IntegerImmediate::fromUnsigned(32, 42);
  if (!AnswerValue)
    return std::unexpected(AnswerValue.error());
  auto Answer =
      makeObject("answer", 4, 4, rcc::GlobalInitializer::integer(std::move(*AnswerValue)), true);
  if (!Answer)
    return std::unexpected(Answer.error());
  Answer->Visibility = rcc::GlobalVisibility::Hidden;
  if (!Unit.addGlobalObject(std::move(*Answer)))
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to add answer");

  auto Zero = rcc::GlobalInitializer::zeroFill(16);
  if (!Zero)
    return std::unexpected(Zero.error());
  auto Buffer = makeObject("zero_buffer", 16, 16, std::move(*Zero));
  if (!Buffer)
    return std::unexpected(Buffer.error());
  Buffer->Linkage = rcc::GlobalLinkage::Internal;
  if (!Unit.addGlobalObject(std::move(*Buffer)))
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to add buffer");

  auto TLSZero = rcc::GlobalInitializer::zeroFill(8);
  if (!TLSZero)
    return std::unexpected(TLSZero.error());
  auto TLS = makeObject("tls_counter", 8, 8, std::move(*TLSZero));
  if (!TLS)
    return std::unexpected(TLS.error());
  TLS->TLSModel = rcc::GlobalTLSModel::LocalExec;
  if (!Unit.addGlobalObject(std::move(*TLS)))
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to add TLS object");

  auto Address = rcc::GlobalInitializer::symbolAddress(
      rcc::GlobalSymbolAddress{.Symbol = "answer", .Addend = 8, .Size = 8, .TargetFlags = 0});
  if (!Address)
    return std::unexpected(Address.error());
  auto Pointer = makeObject("answer_pointer", 8, 8, std::move(*Address), true);
  if (!Pointer)
    return std::unexpected(Pointer.error());
  Pointer->Linkage = rcc::GlobalLinkage::WeakAny;
  if (!Unit.addGlobalObject(std::move(*Pointer)))
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to add pointer");

  auto Bytes = rcc::GlobalInitializer::bytes(
      {std::byte{0x52}, std::byte{0x43}, std::byte{0x43}, std::byte{0x00}});
  if (!Bytes)
    return std::unexpected(Bytes.error());
  auto Grouped = makeObject("grouped_data", 4, 4, std::move(*Bytes));
  if (!Grouped)
    return std::unexpected(Grouped.error());
  Grouped->Linkage = rcc::GlobalLinkage::LinkOnceODR;
  Grouped->COMDAT = "grouped_data";
  if (!Unit.addGlobalObject(std::move(*Grouped)))
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to add COMDAT object");

  auto CommonAlign = rcc::Alignment::create(16);
  if (!CommonAlign)
    return std::unexpected(CommonAlign.error());
  rcc::GlobalObjectDesc Common;
  Common.Symbol = "tentative";
  Common.ObjectType = rcc::GenericConstantType::byte();
  Common.Linkage = rcc::GlobalLinkage::Common;
  Common.Align = *CommonAlign;
  Common.Size = 32;
  if (!Unit.addGlobalObject(std::move(Common)))
    return rcc::makeUnexpected(rcc::ErrorCode::InternalError, "failed to add common object");
  return {};
}

} // namespace

int main(int ArgumentCount, char **Arguments) {
  auto Target = rcc::riscv::createTargetDescription();
  if (!Target)
    return fail(Target.error().message());

  rcc::CompilationUnit Unit;
  auto Added = addObjects(Unit);
  if (!Added)
    return fail(Added.error().message());
  auto Assembly = rcc::riscv::emitAssembly(Unit, *Target);
  if (!Assembly)
    return fail(Assembly.error().message());

  if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--print") {
    std::cout << *Assembly;
    return 0;
  }

  const std::string_view Text = *Assembly;
  if (!contains(Text, ".section .rodata,\"a\",@progbits\n") || !contains(Text, ".globl answer\n") ||
      !contains(Text, ".hidden answer\n") || !contains(Text, ".type answer, @object\n") ||
      !contains(Text, "answer:\n") || !contains(Text, ".byte 0x2a, 0x00, 0x00, 0x00\n") ||
      !contains(Text, ".size answer, 4\n")) {
    return fail("read-only global assembly is incomplete");
  }
  if (!contains(Text, ".section .bss,\"aw\",@nobits\n") ||
      !contains(Text, ".local zero_buffer\n") || !contains(Text, "zero_buffer:\n") ||
      !contains(Text, ".zero 16\n")) {
    return fail("BSS global assembly is incomplete");
  }
  if (!contains(Text, ".section .tbss,\"awT\",@nobits\n") ||
      !contains(Text, ".type tls_counter, @tls_object\n")) {
    return fail("TLS global assembly is incomplete");
  }
  if (!contains(Text, ".section .data.rel.ro,\"aw\",@progbits\n") ||
      !contains(Text, ".weak answer_pointer\n") || !contains(Text, ".8byte (answer+8)\n")) {
    return fail("relocatable constant global assembly is incomplete");
  }
  if (!contains(Text, ".section .data.grouped_data,\"awG\",@progbits,grouped_data,comdat\n") ||
      !contains(Text, ".weak grouped_data\n")) {
    return fail("COMDAT global assembly is incomplete");
  }
  if (!contains(Text, ".comm tentative,32,16\n"))
    return fail("common global assembly is incomplete");
  if (!contains(Text, ".attribute arch, \"rv64gc\"\n") ||
      !contains(Text, ".section \".note.GNU-stack\",\"\",@progbits\n")) {
    std::cerr << Text;
    return fail("RV64GC file directives are incomplete");
  }

  rcc::CompilationUnit UnsupportedUnit;
  auto UnsupportedZero = rcc::GlobalInitializer::zeroFill(1);
  if (!UnsupportedZero)
    return fail("unsupported global test initializer creation failed");
  auto UnsupportedObject = makeObject("unsupported_global", 1, 1, std::move(*UnsupportedZero));
  if (!UnsupportedObject)
    return fail("unsupported global test object creation failed");
  UnsupportedObject->TargetFlags = 1;
  if (!UnsupportedUnit.addGlobalObject(std::move(*UnsupportedObject)))
    return fail("unsupported global test insertion failed");
  auto UnsupportedAssembly = rcc::riscv::emitAssembly(UnsupportedUnit, *Target);
  if (UnsupportedAssembly || UnsupportedAssembly.error().code() != rcc::ErrorCode::Unsupported)
    return fail("RV64GC assembly must reject unknown global target state");
  return 0;
}
