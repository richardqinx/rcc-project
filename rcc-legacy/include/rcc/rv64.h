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

/// @file rv64.h
/// @brief RISC-V 64 (lp64d ABI) backend per-target header.

#pragma once

#include "rcc/all.h"

#include <cstdint>

namespace rcc {

// Register Numbers

enum Rv64Reg : int {
    // GPR: caller-save temps
    T0 = RXX + 1,
    T1,
    T2,
    T3,
    T4,
    T5,

    // GPR: arguments / return values (caller-save)
    A0,
    A1,
    A2,
    A3,
    A4,
    A5,
    A6,
    A7,

    // GPR: callee-save (S0 = FP, see below; therefore jumps to S1)
    S1,
    S2,
    S3,
    S4,
    S5,
    S6,
    S7,
    S8,
    S9,
    S10,
    S11,

    // Globally live
    FP,  // = S0 alias, frame pointer
    SP,  // stack pointer
    GP,  // global pointer
    TP,  // thread pointer
    RA,  // return address

    // FPR: caller-save temps
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

    // FPR: arguments / return values (caller-save)
    FA0,
    FA1,
    FA2,
    FA3,
    FA4,
    FA5,
    FA6,
    FA7,

    // FPR: callee-save
    FS0,
    FS1,
    FS2,
    FS3,
    FS4,
    FS5,
    FS6,
    FS7,
    FS8,
    FS9,
    FS10,
    FS11,

    // Reserved (emit.cc internal scratch, does not participate in rega)
    T6,
    FT11,

    // Derived counters (layout-dependent)
    NFPR = FS11 - FT0 + 1,      // Total FPR count (including reserved)
    NGPR = RA - T0 + 1,         // Total GPR count (including reserved)
    NGPS = A7 - T0 + 1,         // T0..A7: scratch + arg segment
    NFPS = FA7 - FT0 + 1,       // FT0..FA7: scratch + arg segment
    NCLR = (S11 - S1 + 1)       // Total callee-save count (GP)
           + (FS11 - FS0 + 1),  //              + (FP)
};

MAKESURE(rv64_reg_not_tmp, FT11 < static_cast<int>(Tmp0));

// Per-op attribute table (defined in targ.cc)

struct Rv64Op {
    uint8_t imm;  // 1 = op accepts immediate operand
};

// Global tables provided by targ.cc

extern int rv64_rsave[];  // caller-save list (for rega)
extern int rv64_rclob[];  // call boundary clobber list
extern Rv64Op rv64_op[];  // Indexed by O* op enum value

// rv64 target descriptor. Assigned `T = T_rv64` early in main startup (before worker thread
// creation), read-only afterwards.
extern Target T_rv64;

// Backend pass entries

// abi.cc - RCall encode/decode (pure query, no allocations)
bits rv64_retregs(Ref, int p[2]);
bits rv64_argregs(Ref, int p[2]);

// abi.cc - ABI lowering (internal newtmp/emit/idup/vgrow/alloc, requires Ctx*)
void rv64_abi(Ctx*, Fn*);

// isel.cc - Instruction selection (internal newtmp/emit, requires Ctx*)
void rv64_isel(Ctx*, Fn*);

// emit.cc - Assembly emission
void rv64_emitfn(Fn*, FILE*);

}  // namespace rcc
