/* Source code for RCC-Legacy.
   Copyright (C) 2026 Richard Qin

This file is part of RCC.

RCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

RCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with RCC; see the file LICENSE.  If not see
<http://www.gnu.org/licenses/>.  */

/// @file targ.cc
/// @brief RISC-V 64 lp64d target descriptor.

#include "rcc/rv64.h"

namespace rcc {

// Per-op attribute table.
Rv64Op rv64_op[NOp];

namespace {
struct Rv64OpInit {
    Rv64OpInit() {
        for (size_t i = 0; i < NOp; ++i)
            rv64_op[i].imm = kops[i].imm;
    }
} rv64_op_init_;
}  // namespace

// Caller-save list for rega/spill scanning.

int rv64_rsave[] = {
    // GP segment (NGPS = 14): args + temps
    A0,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    A7,
    T0,
    T1,
    T2,
    T3,
    T4,
    T5,

    // FP segment (NFPS = 19): temps + args
    FT0,
    FT1,
    FT2,
    FT3,
    FT4,
    FT5,
    FT6,
    FT7,
    FT8,
    FT9,
    FT10,
    FA0,
    FA1,
    FA2,
    FA3,
    FA4,
    FA5,
    FA6,
    FA7,

    -1,
};

// Callee-save list for prologue/epilogue generation.

int rv64_rclob[] = {
    S1,  S2,  S3,  S4,  S5,  S6,  S7,  S8,  S9,  S10,  S11,  FS0,
    FS1, FS2, FS3, FS4, FS5, FS6, FS7, FS8, FS9, FS10, FS11, -1,
};

// Global live registers bitmask.

namespace {
constexpr bits kRv64GlobLive = BIT(FP) | BIT(SP) | BIT(GP) | BIT(TP) | BIT(RA);

// RV64 ISA has no memory operand instructions.
int memargs_none(int /*op*/) {
    return 0;
}
}  // namespace

// Target descriptor.

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++20-designator"
#endif

Target T_rv64 = {
    .name = "rv64",
    .apple = 0,
    .windows = 0,
    .gpr0 = T0,
    .ngpr = NGPR,
    .fpr0 = FT0,
    .nfpr = NFPR,
    .rglob = kRv64GlobLive,
    .nrglob = 5,
    .rsave = rv64_rsave,
    .nrsave = {NGPS, NFPS},
    .retregs = rv64_retregs,
    .argregs = rv64_argregs,
    .memargs = memargs_none,
    .abi0 = elimsb,
    .abi1 = rv64_abi,
    .isel = rv64_isel,
    .emitfn = rv64_emitfn,
    .emitfin = rv64_emitfin,
    .asloc = ".L",
    .assym = "",
    .cansel = 0,
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// Table size consistency checks.

MAKESURE(rv64_rsave_size_ok,
         sizeof(rv64_rsave) / sizeof(int) == static_cast<size_t>(NGPS + NFPS + 1));
MAKESURE(rv64_rclob_size_ok, sizeof(rv64_rclob) / sizeof(int) == static_cast<size_t>(NCLR + 1));

}  // namespace rcc
