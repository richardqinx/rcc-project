// SPDX-License-Identifier: GPL-3.0-or-later

#include "rcc/MIR/LLVMConstantSyntax.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

class TestContext final {
public:
  void expect(bool Condition, std::string_view Message) {
    if (Condition)
      return;
    std::cerr << Message << '\n';
    ++FailureCount;
  }

  [[nodiscard]] int result() const noexcept {
    return FailureCount == 0 ? 0 : 1;
  }

private:
  int FailureCount = 0;
};

constexpr rcc::mir::LLVMConstantSyntaxOptions Options{
    .DataLayout = "e-p:64:64",
    .NamedTypes = {},
};

void expectRoundTrip(TestContext &Context, std::string_view Text, std::uint64_t ExpectedSize) {
  auto Parsed = rcc::mir::parseLLVMConstant(Text, Options);
  Context.expect(Parsed.has_value(), "valid LLVM constant must parse");
  if (!Parsed) {
    std::cerr << Text << ": " << Parsed.error().message() << '\n';
    return;
  }
  auto Size = rcc::mir::llvmConstantStorageSize(Parsed->Type, Options);
  if (!Size || *Size != ExpectedSize) {
    std::cerr << Text << ": expected storage size " << ExpectedSize << ", got ";
    if (Size)
      std::cerr << *Size;
    else
      std::cerr << Size.error().message();
    std::cerr << '\n';
  }
  Context.expect(Size.has_value() && *Size == ExpectedSize,
                 "LLVM constant storage size must match its structured type");
  auto Printed = rcc::mir::printLLVMConstant(*Parsed);
  Context.expect(Printed.has_value(), "structured LLVM constant must print");
  if (!Printed)
    return;
  auto Reparsing = rcc::mir::parseLLVMConstant(*Printed, Options);
  Context.expect(Reparsing.has_value() && *Reparsing == *Parsed,
                 "LLVM constant parse-print-parse must preserve the value tree");
  if (!Reparsing)
    std::cerr << *Printed << ": " << Reparsing.error().message() << '\n';
}

void expectLayoutRoundTrip(TestContext &Context, std::string_view Text,
                           rcc::mir::LLVMConstantSyntaxOptions LayoutOptions,
                           std::uint64_t ExpectedSize, std::uint64_t ExpectedPreferredAlignment) {
  auto Parsed = rcc::mir::parseLLVMConstant(Text, LayoutOptions);
  Context.expect(Parsed.has_value(), "DataLayout-aware LLVM constant must parse");
  if (!Parsed) {
    std::cerr << Text << ": " << Parsed.error().message() << '\n';
    return;
  }
  auto Size = rcc::mir::llvmConstantStorageSize(Parsed->Type, LayoutOptions);
  auto Alignment = rcc::mir::llvmConstantPreferredAlignment(Parsed->Type, LayoutOptions);
  Context.expect(Size.has_value() && *Size == ExpectedSize,
                 "DataLayout must determine the exact allocation size");
  if (!Size || *Size != ExpectedSize) {
    std::cerr << Text << ": expected size " << ExpectedSize << ", got "
              << (Size ? std::to_string(*Size) : Size.error().message()) << '\n';
  }
  Context.expect(Alignment.has_value() && Alignment->bytes() == ExpectedPreferredAlignment,
                 "DataLayout must determine the exact preferred alignment");
  if (!Alignment || Alignment->bytes() != ExpectedPreferredAlignment) {
    std::cerr << Text << ": expected preferred alignment " << ExpectedPreferredAlignment << ", got "
              << (Alignment ? std::to_string(Alignment->bytes()) : Alignment.error().message())
              << '\n';
  }
  auto Printed = rcc::mir::printLLVMConstant(*Parsed);
  Context.expect(Printed.has_value(), "DataLayout-aware LLVM constant must print");
  if (!Printed)
    return;
  auto Reparsing = rcc::mir::parseLLVMConstant(*Printed, LayoutOptions);
  Context.expect(Reparsing.has_value() && *Reparsing == *Parsed,
                 "DataLayout-aware named constants must round-trip structurally");
}

void testScalarConstants(TestContext &Context) {
  expectRoundTrip(Context, "i1 true", 1);
  expectRoundTrip(Context, "i65 -1", 12);
  expectRoundTrip(Context, "half 0xH3C00", 2);
  expectRoundTrip(Context, "bfloat 0xR3F80", 2);
  expectRoundTrip(Context, "float 1.250000e+00", 4);
  expectRoundTrip(Context, "double 0x3FF0000000000000", 8);
  expectRoundTrip(Context, "x86_fp80 f0x4000C000000000000000", 16);
  expectRoundTrip(Context, "fp128 f0x3FFF0000000000000000000000000000", 16);
  expectRoundTrip(Context, "ptr null", 8);
  expectRoundTrip(Context, "i32 undef", 4);
  expectRoundTrip(Context, "i32 poison", 4);
  expectRoundTrip(Context, "i32 zeroinitializer", 4);

  auto BFloat = rcc::mir::parseLLVMConstant("bfloat 0xR3E80", Options);
  auto PrintedBFloat = BFloat ? rcc::mir::printLLVMConstant(*BFloat)
                              : rcc::Expected<std::string>(std::unexpected(BFloat.error()));
  Context.expect(PrintedBFloat && *PrintedBFloat == "bfloat 2.500000e-01",
                 "LLVM-compatible floating printing must prefer an exact decimal spelling");

  auto Pi = rcc::mir::parseLLVMConstant("double f0x400921FB54442D18", Options);
  auto PrintedPi = Pi ? rcc::mir::printLLVMConstant(*Pi)
                      : rcc::Expected<std::string>(std::unexpected(Pi.error()));
  Context.expect(
      PrintedPi && *PrintedPi == "double f0x400921FB54442D18",
      "floating printing must retain exact bits when six decimal digits do not round-trip");
}

void expectIntegerWords(TestContext &Context, std::string_view Text,
                        std::span<const std::uint64_t> ExpectedWords) {
  auto Parsed = rcc::mir::parseLLVMConstant(Text, Options);
  Context.expect(Parsed.has_value() && Parsed->IntegerValue.has_value() &&
                     std::ranges::equal(Parsed->IntegerValue->words(), ExpectedWords),
                 "LLVM integer literal must preserve its exact bit pattern");
  if (!Parsed)
    std::cerr << Text << ": " << Parsed.error().message() << '\n';
}

void testDecimalIntegerTruncation(TestContext &Context) {
  constexpr std::array<std::uint64_t, 1> NegativeOne{std::numeric_limits<std::uint64_t>::max()};
  constexpr std::array<std::uint64_t, 1> NarrowNegative{767};
  constexpr std::array<std::uint64_t, 1> NarrowMinimum{8};
  constexpr std::array<std::uint64_t, 1> WidePositive{1};

  expectIntegerWords(Context, "i64 -1", NegativeOne);
  expectIntegerWords(Context, "i16 -64769", NarrowNegative);
  expectIntegerWords(Context, "i4 -8", NarrowMinimum);
  expectIntegerWords(Context, "i64 18446744073709551617", WidePositive);
}

void expectFloatingWords(TestContext &Context, std::string_view Text,
                         std::span<const std::uint64_t> ExpectedWords) {
  auto Parsed = rcc::mir::parseLLVMConstant(Text, Options);
  Context.expect(Parsed.has_value() && Parsed->FloatingValue.has_value() &&
                     std::ranges::equal(Parsed->FloatingValue->words(), ExpectedWords),
                 "LLVM floating special literal must preserve its exact bit pattern");
  if (!Parsed) {
    std::cerr << Text << ": " << Parsed.error().message() << '\n';
    return;
  }
  if (!Parsed->FloatingValue ||
      !std::ranges::equal(Parsed->FloatingValue->words(), ExpectedWords)) {
    std::cerr << Text << ": expected words";
    for (const std::uint64_t Word : ExpectedWords)
      std::cerr << " 0x" << std::hex << Word;
    std::cerr << ", got";
    if (Parsed->FloatingValue) {
      for (const std::uint64_t Word : Parsed->FloatingValue->words())
        std::cerr << " 0x" << std::hex << Word;
    }
    std::cerr << std::dec << '\n';
  }
  auto Printed = rcc::mir::printLLVMConstant(*Parsed);
  Context.expect(Printed.has_value(), "LLVM floating special literal must print");
  if (!Printed)
    return;
  auto Reparsing = rcc::mir::parseLLVMConstant(*Printed, Options);
  Context.expect(Reparsing.has_value() && *Reparsing == *Parsed,
                 "printed LLVM floating special literal must round-trip");
}

void testExplicitHexIntegerConstants(TestContext &Context) {
  constexpr std::array<std::uint64_t, 2> AllOnes65{std::numeric_limits<std::uint64_t>::max(), 1};
  constexpr std::array<std::uint64_t, 2> SignBit65{0, 1};
  constexpr std::array<std::uint64_t, 1> UnsignedFF{0xff};
  constexpr std::array<std::uint64_t, 1> SignedFF{0xffff};
  constexpr std::array<std::uint64_t, 1> SignedFF8{0xff};

  expectIntegerWords(Context, "i65 u0x1FFFFFFFFFFFFFFFF", AllOnes65);
  expectIntegerWords(Context, "i65 s0x1FFFFFFFFFFFFFFFF", AllOnes65);
  expectIntegerWords(Context, "i65 s0x10000000000000000", SignBit65);
  expectIntegerWords(Context, "i65 s0x01", AllOnes65);
  expectIntegerWords(Context, "i16 u0xFF", UnsignedFF);
  expectIntegerWords(Context, "i16 s0xFF", SignedFF);
  expectIntegerWords(Context, "i8 s0x1FF", SignedFF8);

  expectRoundTrip(Context, "i65 s0x01", 12);
  Context.expect(!rcc::mir::parseLLVMConstant("i32 u0x", Options) &&
                     !rcc::mir::parseLLVMConstant("i32 u0X1", Options),
                 "explicit LLVM hexadecimal integers must use the fixed spelling");
}

void testSpecialFloatingLiterals(TestContext &Context) {
  constexpr std::array<std::uint64_t, 1> HalfInfinity{0x7c00};
  constexpr std::array<std::uint64_t, 1> NegativeHalfInfinity{0xfc00};
  constexpr std::array<std::uint64_t, 1> HalfQuietNaN{0x7e00};
  constexpr std::array<std::uint64_t, 1> NegativeHalfQuietNaN{0xfe00};
  constexpr std::array<std::uint64_t, 1> HalfSignalingNaN{0x7d00};
  constexpr std::array<std::uint64_t, 1> HalfPayloadNaN{0x7fef};
  constexpr std::array<std::uint64_t, 1> FloatSignalingNaN{0x7f800001};
  constexpr std::array<std::uint64_t, 2> X87Infinity{0x8000000000000000, 0x7fff};
  constexpr std::array<std::uint64_t, 2> X87QuietNaN{0xc000000000000000, 0x7fff};
  constexpr std::array<std::uint64_t, 2> QuadOne{0, 0x3fff000000000000};
  constexpr std::array<std::uint64_t, 2> QuadInfinity{0, 0x7fff000000000000};
  constexpr std::array<std::uint64_t, 2> QuadQuietNaN{0, 0x7fff800000000000};
  constexpr std::array<std::uint64_t, 2> PPCOne{0x3ff0000000000000, 0};
  constexpr std::array<std::uint64_t, 2> PPCInfinity{0x7ff0000000000000, 0};
  constexpr std::array<std::uint64_t, 2> PPCQuietNaN{0x7ff8000000000000, 0};

  expectFloatingWords(Context, "half +inf", HalfInfinity);
  expectFloatingWords(Context, "half -inf", NegativeHalfInfinity);
  expectFloatingWords(Context, "half +qnan", HalfQuietNaN);
  expectFloatingWords(Context, "half -qnan", NegativeHalfQuietNaN);
  expectFloatingWords(Context, "half +snan(0x100)", HalfSignalingNaN);
  expectFloatingWords(Context, "half +nan(0x123456789ABCDEF)", HalfPayloadNaN);
  expectFloatingWords(Context, "float +snan(0x400001)", FloatSignalingNaN);
  expectFloatingWords(Context, "x86_fp80 +inf", X87Infinity);
  expectFloatingWords(Context, "x86_fp80 +qnan", X87QuietNaN);
  expectFloatingWords(Context, "fp128 +inf", QuadInfinity);
  expectFloatingWords(Context, "fp128 +qnan", QuadQuietNaN);
  expectFloatingWords(Context, "fp128 0xL00000000000000003FFF000000000000", QuadOne);
  expectFloatingWords(Context, "ppc_fp128 +inf", PPCInfinity);
  expectFloatingWords(Context, "ppc_fp128 +qnan", PPCQuietNaN);
  expectFloatingWords(Context, "ppc_fp128 0xM3FF00000000000000000000000000000", PPCOne);

  for (const std::string_view Text : {
           std::string_view{"float nan(0x)"},
           std::string_view{"float snan(12)"},
           std::string_view{"float qnan(0x1)"},
           std::string_view{"float inf(0x1)"},
           std::string_view{"float inf"},
           std::string_view{"float +nan"},
           std::string_view{"float +snan"},
           std::string_view{"float +qnan(0x1)"},
       }) {
    Context.expect(!rcc::mir::parseLLVMConstant(Text, Options),
                   "malformed LLVM floating special literal must be rejected");
  }
}

void testExactFloatingConversion(TestContext &Context) {
  constexpr std::array<std::uint64_t, 1> PositiveZero{0};
  constexpr std::array<std::uint64_t, 1> NegativeFloatZero{0x80000000};
  constexpr std::array<std::uint64_t, 1> HalfOne{0x3c00};
  constexpr std::array<std::uint64_t, 1> HalfTwoUlps{0x3c02};
  constexpr std::array<std::uint64_t, 1> HalfSmallestSubnormal{0x0001};
  constexpr std::array<std::uint64_t, 1> BFloatOne{0x3f80};
  constexpr std::array<std::uint64_t, 1> BFloatTwoUlps{0x3f82};
  constexpr std::array<std::uint64_t, 1> FloatOne{0x3f800000};
  constexpr std::array<std::uint64_t, 1> FloatTwoUlps{0x3f800002};
  constexpr std::array<std::uint64_t, 1> FloatHexPower{0x2f800000};
  constexpr std::array<std::uint64_t, 1> FloatSignalingNaN{0x7f800001};
  constexpr std::array<std::uint64_t, 1> FloatQuietNaN{0x7fc00000};
  constexpr std::array<std::uint64_t, 1> LargestDouble{0x7fefffffffffffff};
  constexpr std::array<std::uint64_t, 2> X87One{0x8000000000000000, 0x3fff};
  constexpr std::array<std::uint64_t, 2> QuadOne{0, 0x3fff000000000000};
  constexpr std::array<std::uint64_t, 2> PPCOne{0x3ff0000000000000, 0};

  expectFloatingWords(Context, "float +0.0", PositiveZero);
  expectFloatingWords(Context, "float -0.0", NegativeFloatZero);
  expectFloatingWords(Context, "half 1.00048828125", HalfOne);
  expectFloatingWords(Context, "half 1.00146484375", HalfTwoUlps);
  expectFloatingWords(Context, "half 0.0000000298023223876953126", HalfSmallestSubnormal);
  expectFloatingWords(Context, "bfloat 1.00390625", BFloatOne);
  expectFloatingWords(Context, "bfloat 1.01171875", BFloatTwoUlps);
  expectFloatingWords(Context, "float 1.000000059604644775390625", FloatOne);
  expectFloatingWords(Context, "float 1.000000178813934326171875", FloatTwoUlps);
  expectFloatingWords(Context, "float 0x1.0p-32", FloatHexPower);
  expectFloatingWords(Context, "double 1.79769313486231570815e+308", LargestDouble);
  expectFloatingWords(Context, "x86_fp80 1.0", X87One);
  expectFloatingWords(Context, "x86_fp80 0x1.0p0", X87One);
  expectFloatingWords(Context, "fp128 1.0", QuadOne);
  expectFloatingWords(Context, "fp128 0x1.0p0", QuadOne);
  expectFloatingWords(Context, "ppc_fp128 1.0", PPCOne);
  expectFloatingWords(Context, "ppc_fp128 0x1.0p0", PPCOne);
  expectFloatingWords(Context, "float 0x3FF0000000000000", FloatOne);
  expectFloatingWords(Context, "float 0x7FF0000020000000", FloatSignalingNaN);
  expectFloatingWords(Context, "float 0x7FF8000000000000", FloatQuietNaN);
  expectFloatingWords(Context, "float f0x3F800000", FloatOne);

  for (const std::string_view Text : {
           std::string_view{"half 0.0000000298023223876953125"},
           std::string_view{"float 0x3FF0000000000001"},
           std::string_view{"float 0x7FF8000000000001"},
           std::string_view{"float f0x3F8000"},
           std::string_view{"float f0X3F800000"},
           std::string_view{"float 0X1.0p0"},
           std::string_view{"float 0x1p0"},
       }) {
    Context.expect(!rcc::mir::parseLLVMConstant(Text, Options),
                   "noncanonical LLVM floating syntax must be rejected");
  }
}

void testAggregateConstants(TestContext &Context) {
  expectRoundTrip(Context, "[4 x i8] c\"ab\\00\\FF\"", 4);
  expectRoundTrip(Context, "[3 x i16] [i16 1, i16 2, i16 3]", 6);
  expectRoundTrip(Context, "<4 x i32> <i32 0, i32 1, i32 2, i32 3>", 16);
  expectRoundTrip(Context, "<4 x float> <float 1.0, float -1.0, float 2.0, float -2.0>", 16);
  expectRoundTrip(Context, "{ i32, ptr } { i32 7, ptr @global }", 16);
  expectRoundTrip(Context, "<{ i8, i32 }> <{ i8 1, i32 2 }>", 5);
  expectRoundTrip(Context, "<8 x i16> splat (i16 9)", 16);
}

void testAddressConstants(TestContext &Context) {
  expectRoundTrip(Context, "ptr @global", 8);
  expectRoundTrip(Context, "ptr @\"quoted name\"", 8);
  expectRoundTrip(Context, "ptr blockaddress(@function, %entry)", 8);
  expectRoundTrip(Context, "ptr dso_local_equivalent @function", 8);
  expectRoundTrip(Context, "ptr no_cfi @function", 8);
  expectRoundTrip(Context, "i64 ptrtoint (ptr @global to i64)", 8);
  expectRoundTrip(Context, "ptr bitcast (ptr @global to ptr)", 8);
  expectRoundTrip(Context, "i64 add nuw nsw (i64 1, i64 2)", 8);
  expectRoundTrip(Context, "ptr getelementptr inbounds nusw nuw (i8, ptr @global, i64 4)", 8);
  expectRoundTrip(Context,
                  "<2 x i32> shufflevector (<2 x i32> <i32 1, i32 2>, "
                  "<2 x i32> <i32 3, i32 4>, <2 x i32> <i32 0, i32 3>)",
                  8);
}

void testConstantExpressionVerification(TestContext &Context) {
  expectRoundTrip(Context, "i8 trunc (i16 257 to i8)", 1);
  expectRoundTrip(Context, "i32 bitcast (float 1.0 to i32)", 4);
  expectRoundTrip(Context, "ptr addrspacecast (ptr addrspace(1) @global to ptr)", 8);
  expectRoundTrip(Context, "ptr inttoptr (i64 0 to ptr)", 8);
  expectRoundTrip(Context, "i64 ptrtoaddr (ptr @global to i64)", 8);
  expectRoundTrip(Context, "i64 ptrtoint (ptr @global to i64)", 8);
  expectRoundTrip(Context,
                  "<2 x i32> insertelement (<2 x i32> <i32 1, i32 2>, "
                  "i32 3, i64 0)",
                  8);
  expectRoundTrip(Context, "i32 extractelement (<2 x i32> <i32 1, i32 2>, i64 0)", 4);
  expectRoundTrip(Context, "ptr ptrauth (ptr @function, i32 1, i64 0, ptr null, ptr null)", 8);

  for (const std::string_view Text : {
           std::string_view{"i16 trunc (i8 1 to i16)"},
           std::string_view{"i64 bitcast (float 1.0 to i64)"},
           std::string_view{"ptr bitcast (ptr addrspace(1) @global to ptr)"},
           std::string_view{"ptr addrspacecast (ptr @global to ptr)"},
           std::string_view{"ptr inttoptr (float 1.0 to ptr)"},
           std::string_view{"i64 ptrtoint (i64 0 to i64)"},
           std::string_view{"i32 add nuw (i32 1, i64 2)"},
           std::string_view{"ptr getelementptr (i8, ptr @global, float 1.0)"},
           std::string_view{"<2 x i32> shufflevector (<2 x i32> <i32 1, i32 2>, "
                            "<2 x i32> <i32 3, i32 4>, <2 x i64> <i64 0, i64 3>)"},
           std::string_view{"ptr ptrauth (ptr @function, i64 1)"},
       }) {
    Context.expect(!rcc::mir::parseLLVMConstant(Text, Options),
                   "constant expression with incompatible typed operands must be rejected");
  }
}

void testDataLayoutAndNamedTypes(TestContext &Context) {
  static constexpr std::array Definitions{
      rcc::mir::LLVMNamedTypeDefinition{
          .Name = "pair", .Definition = "{ i8, i64 }", .NumericName = false},
      rcc::mir::LLVMNamedTypeDefinition{.Name = "word", .Definition = "i32", .NumericName = false},
      rcc::mir::LLVMNamedTypeDefinition{
          .Name = "node", .Definition = "{ i32, %node* }", .NumericName = false},
      rcc::mir::LLVMNamedTypeDefinition{
          .Name = "loop", .Definition = "{ %loop }", .NumericName = false},
  };
  static constexpr rcc::mir::LLVMConstantSyntaxOptions LayoutOptions{
      .DataLayout = "e-p:64:64-p1:32:16:64-i64:64:128-i128:128-v96:128:256-a:0:64",
      .NamedTypes = Definitions,
  };

  expectLayoutRoundTrip(Context, "i65 -1", LayoutOptions, 16, 16);
  expectLayoutRoundTrip(Context, "[3 x i24] [i24 1, i24 2, i24 3]", LayoutOptions, 12, 4);
  expectLayoutRoundTrip(Context, "<3 x i32> <i32 1, i32 2, i32 3>", LayoutOptions, 16, 32);
  expectLayoutRoundTrip(Context, "ptr addrspace(1) null", LayoutOptions, 4, 8);
  expectLayoutRoundTrip(Context, "%pair { i8 1, i64 2 }", LayoutOptions, 16, 8);
  expectLayoutRoundTrip(Context, "%word 7", LayoutOptions, 4, 4);
  expectLayoutRoundTrip(Context, "%node zeroinitializer", LayoutOptions, 16, 8);

  Context.expect(!rcc::mir::parseLLVMType("%loop", LayoutOptions),
                 "directly recursive named aggregate types must be rejected as unsized");
  auto NodeType = rcc::mir::parseLLVMType("%node", LayoutOptions);
  Context.expect(NodeType.has_value(),
                 "named recursion through a pointer must remain sized and legal");
}

void testAddressSpacePointerWidths(TestContext &Context) {
  constexpr rcc::mir::LLVMConstantSyntaxOptions HeterogeneousLayoutOptions{
      .DataLayout = "e-p:64:64-p1:32:16:64",
      .NamedTypes = {},
  };
  auto DefaultPointer = rcc::mir::llvmPointerSizeInBits(0, HeterogeneousLayoutOptions);
  auto AddressSpaceOnePointer = rcc::mir::llvmPointerSizeInBits(1, HeterogeneousLayoutOptions);
  Context.expect(DefaultPointer.has_value() && *DefaultPointer == 64 &&
                     AddressSpaceOnePointer.has_value() && *AddressSpaceOnePointer == 32,
                 "LLVM data layout must provide pointer width by address space");

  auto MissingPointer = rcc::mir::llvmPointerSizeInBits(7);
  Context.expect(!MissingPointer,
                 "pointer width lookup must reject a missing explicit data layout");
}

void testDataLayoutBoundaryRules(TestContext &Context) {
  static constexpr rcc::mir::LLVMConstantSyntaxOptions IntegerAlignmentOptions{
      .DataLayout = "e-p:64:64-i24:128:128",
      .NamedTypes = {},
  };
  static constexpr rcc::mir::LLVMConstantSyntaxOptions HistoricalAggregateOptions{
      .DataLayout = "e-p:64:64-a0:64:128",
      .NamedTypes = {},
  };
  static constexpr rcc::mir::LLVMConstantSyntaxOptions ZeroAggregateOptions{
      .DataLayout = "e-p:64:64-a:0:64",
      .NamedTypes = {},
  };
  static constexpr rcc::mir::LLVMConstantSyntaxOptions NonBytePointerOptions{
      .DataLayout = "e-p:33:8:8:32",
      .NamedTypes = {},
  };
  static constexpr rcc::mir::LLVMConstantSyntaxOptions PointerFlagsOptions{
      .DataLayout = "e-pue1(storage):33:8:8:32-po2(allones):64:64",
      .NamedTypes = {},
  };

  expectLayoutRoundTrip(Context, "i20 1", IntegerAlignmentOptions, 16, 16);
  expectLayoutRoundTrip(Context, "{ i8, i32 } { i8 1, i32 2 }", HistoricalAggregateOptions, 8, 16);
  expectLayoutRoundTrip(Context, "{ i8, i32 } { i8 1, i32 2 }", ZeroAggregateOptions, 8, 8);
  expectLayoutRoundTrip(Context, "<{ i8, i32 }> <{ i8 1, i32 2 }>", ZeroAggregateOptions, 5, 8);
  expectLayoutRoundTrip(Context, "ptr null", NonBytePointerOptions, 5, 1);
  expectLayoutRoundTrip(Context, "ptr addrspace(1) null", PointerFlagsOptions, 5, 1);

  static constexpr rcc::mir::LLVMConstantSyntaxOptions InvalidIndexSizeOptions{
      .DataLayout = "e-p:33:8:8:34",
      .NamedTypes = {},
  };
  auto Pointer = rcc::mir::parseLLVMType("ptr", InvalidIndexSizeOptions);
  Context.expect(Pointer.has_value() &&
                     !rcc::mir::llvmConstantStorageSize(*Pointer, InvalidIndexSizeOptions),
                 "LLVM pointer index size larger than its representation must be rejected");

  static constexpr rcc::mir::LLVMConstantSyntaxOptions NonLayoutOptions{
      .DataLayout = "e-m:e-n8:16:32-S128-Fi64-P1-G2-A3-ni:1:2-ve-s-p:64:64",
      .NamedTypes = {},
  };
  expectLayoutRoundTrip(Context, "ptr null", NonLayoutOptions, 8, 8);

  const auto ExpectInvalidDataLayout = [&Context](std::string_view DataLayout) {
    const rcc::mir::LLVMConstantSyntaxOptions InvalidOptions{
        .DataLayout = DataLayout,
        .NamedTypes = {},
    };
    auto Type = rcc::mir::parseLLVMType("ptr", InvalidOptions);
    Context.expect(Type.has_value() && !rcc::mir::llvmConstantStorageSize(*Type, InvalidOptions),
                   "malformed non-layout LLVM data-layout component must be rejected");
  };
  for (const std::string_view DataLayout : {
           std::string_view{"ee-p:64:64"},
           std::string_view{"m:q-p:64:64"},
           std::string_view{"n64:0-p:64:64"},
           std::string_view{"S-p:64:64"},
           std::string_view{"Fx64-p:64:64"},
           std::string_view{"P16777216-p:64:64"},
           std::string_view{"ni:-p:64:64"},
           std::string_view{"ni:0-p:64:64"},
           std::string_view{"pe:64:64"},
           std::string_view{"pu:64:64"},
           std::string_view{"pzo1:64:64"},
           std::string_view{"p1(P):64:64"},
           std::string_view{"p1(foo):64:64-p2(foo):64:64"},
           std::string_view{"p16777216:64:64"},
       }) {
    ExpectInvalidDataLayout(DataLayout);
  }
}

void testInvalidConstants(TestContext &Context) {
  for (const std::string_view Text : {
           std::string_view{""},
           std::string_view{"opaque 0"},
           std::string_view{"i32 null"},
           std::string_view{"ptr 42"},
           std::string_view{"[2 x i8] [i8 1]"},
           std::string_view{"<2 x i32> <i32 1, i64 2>"},
           std::string_view{"i64 ptrtoint (ptr @global to i32)"},
           std::string_view{"ptr getelementptr (i8, i64 0)"},
           std::string_view{"i32 1 trailing"},
       }) {
    Context.expect(!rcc::mir::parseLLVMConstant(Text, Options),
                   "invalid LLVM constant must be rejected");
  }
}

} // namespace

int main() {
  TestContext Context;
  testScalarConstants(Context);
  testExplicitHexIntegerConstants(Context);
  testDecimalIntegerTruncation(Context);
  testSpecialFloatingLiterals(Context);
  testExactFloatingConversion(Context);
  testAggregateConstants(Context);
  testAddressConstants(Context);
  testConstantExpressionVerification(Context);
  testDataLayoutAndNamedTypes(Context);
  testAddressSpacePointerWidths(Context);
  testDataLayoutBoundaryRules(Context);
  testInvalidConstants(Context);
  return Context.result();
}
