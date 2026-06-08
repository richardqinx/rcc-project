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

/// @file backend/rv64/emit.cc
/// @brief RISC-V 64 (lp64d) assembly emission.

#include "rcc/rv64.h"

#include <cinttypes>
#include <cstdio>

namespace rcc {

// Pseudo K values for omap wildcards.

namespace {

enum : short {
    Ki = -1,  // Matches Kw / Kl
    Ka = -2,  // Matches all cls
};

// Single instruction format templates.
struct AsmFmt {
    short op;
    short cls;
    const char* fmt;
};

constexpr AsmFmt kAsmFmt[] = {
    {Oadd, Ki, "add%k %=, %0, %1"},
    {Oadd, Ka, "fadd.%k %=, %0, %1"},
    {Osub, Ki, "sub%k %=, %0, %1"},
    {Osub, Ka, "fsub.%k %=, %0, %1"},
    {Oneg, Ki, "neg%k %=, %0"},
    {Oneg, Ka, "fneg.%k %=, %0"},
    {Odiv, Ki, "div%k %=, %0, %1"},
    {Odiv, Ka, "fdiv.%k %=, %0, %1"},
    {Orem, Ki, "rem%k %=, %0, %1"},
    {Orem, Kl, "rem %=, %0, %1"},
    {Oudiv, Ki, "divu%k %=, %0, %1"},
    {Ourem, Ki, "remu%k %=, %0, %1"},
    {Omul, Ki, "mul%k %=, %0, %1"},
    {Omul, Ka, "fmul.%k %=, %0, %1"},
    {Oand, Ki, "and %=, %0, %1"},
    {Oor, Ki, "or %=, %0, %1"},
    {Oxor, Ki, "xor %=, %0, %1"},
    {Osar, Ki, "sra%k %=, %0, %1"},
    {Oshr, Ki, "srl%k %=, %0, %1"},
    {Oshl, Ki, "sll%k %=, %0, %1"},
    {Ocsltl, Ki, "slt %=, %0, %1"},
    {Ocultl, Ki, "sltu %=, %0, %1"},
    {Oceqs, Ki, "feq.s %=, %0, %1"},
    {Ocges, Ki, "fge.s %=, %0, %1"},
    {Ocgts, Ki, "fgt.s %=, %0, %1"},
    {Ocles, Ki, "fle.s %=, %0, %1"},
    {Oclts, Ki, "flt.s %=, %0, %1"},
    {Oceqd, Ki, "feq.d %=, %0, %1"},
    {Ocged, Ki, "fge.d %=, %0, %1"},
    {Ocgtd, Ki, "fgt.d %=, %0, %1"},
    {Ocled, Ki, "fle.d %=, %0, %1"},
    {Ocltd, Ki, "flt.d %=, %0, %1"},
    {Ostoreb, Kw, "sb %0, %M1"},
    {Ostoreh, Kw, "sh %0, %M1"},
    {Ostorew, Kw, "sw %0, %M1"},
    {Ostorel, Ki, "sd %0, %M1"},
    {Ostores, Kw, "fsw %0, %M1"},
    {Ostored, Kw, "fsd %0, %M1"},
    {Oloadsb, Ki, "lb %=, %M0"},
    {Oloadub, Ki, "lbu %=, %M0"},
    {Oloadsh, Ki, "lh %=, %M0"},
    {Oloaduh, Ki, "lhu %=, %M0"},
    {Oloadsw, Ki, "lw %=, %M0"},
    // rv64 sign-extends 32-bit values into 64-bit registers.
    {Oloaduw, Kw, "lw %=, %M0"},
    {Oloaduw, Kl, "lwu %=, %M0"},
    {Oload, Kw, "lw %=, %M0"},
    {Oload, Kl, "ld %=, %M0"},
    {Oload, Ks, "flw %=, %M0"},
    {Oload, Kd, "fld %=, %M0"},
    {Oextsb, Ki, "sext.b %=, %0"},
    {Oextub, Ki, "zext.b %=, %0"},
    {Oextsh, Ki, "sext.h %=, %0"},
    {Oextuh, Ki, "zext.h %=, %0"},
    {Oextsw, Kl, "sext.w %=, %0"},
    {Oextuw, Kl, "zext.w %=, %0"},
    {Otruncd, Ks, "fcvt.s.d %=, %0"},
    {Oexts, Kd, "fcvt.d.s %=, %0"},
    {Ostosi, Kw, "fcvt.w.s %=, %0, rtz"},
    {Ostosi, Kl, "fcvt.l.s %=, %0, rtz"},
    {Ostoui, Kw, "fcvt.wu.s %=, %0, rtz"},
    {Ostoui, Kl, "fcvt.lu.s %=, %0, rtz"},
    {Odtosi, Kw, "fcvt.w.d %=, %0, rtz"},
    {Odtosi, Kl, "fcvt.l.d %=, %0, rtz"},
    {Odtoui, Kw, "fcvt.wu.d %=, %0, rtz"},
    {Odtoui, Kl, "fcvt.lu.d %=, %0, rtz"},
    {Oswtof, Ka, "fcvt.%k.w %=, %0"},
    {Ouwtof, Ka, "fcvt.%k.wu %=, %0"},
    {Osltof, Ka, "fcvt.%k.l %=, %0"},
    {Oultof, Ka, "fcvt.%k.lu %=, %0"},
    {Ocast, Kw, "fmv.x.w %=, %0"},
    {Ocast, Kl, "fmv.x.d %=, %0"},
    {Ocast, Ks, "fmv.w.x %=, %0"},
    {Ocast, Kd, "fmv.d.x %=, %0"},
    {Ocopy, Ki, "mv %=, %0"},
    {Ocopy, Ka, "fmv.%k %=, %0"},
    {Oswap, Ki, "mv %?, %0\n\tmv %0, %1\n\tmv %1, %?"},
    {Oswap, Ka, "fmv.%k %?, %0\n\tfmv.%k %0, %1\n\tfmv.%k %1, %?"},
    {Oreqz, Ki, "seqz %=, %0"},
    {Ornez, Ki, "snez %=, %0"},
    {Ocall, Kw, "jalr %0"},
    {NOp, 0, nullptr},
};

// Register names.

constexpr auto kRegName = [] {
    std::array<const char*, FT11 + 1> a{};
    a[T0] = "t0";
    a[T1] = "t1";
    a[T2] = "t2";
    a[T3] = "t3";
    a[T4] = "t4";
    a[T5] = "t5";
    a[A0] = "a0";
    a[A1] = "a1";
    a[A2] = "a2";
    a[A3] = "a3";
    a[A4] = "a4";
    a[A5] = "a5";
    a[A6] = "a6";
    a[A7] = "a7";
    a[S1] = "s1";
    a[S2] = "s2";
    a[S3] = "s3";
    a[S4] = "s4";
    a[S5] = "s5";
    a[S6] = "s6";
    a[S7] = "s7";
    a[S8] = "s8";
    a[S9] = "s9";
    a[S10] = "s10";
    a[S11] = "s11";
    a[FP] = "fp";
    a[SP] = "sp";
    a[GP] = "gp";
    a[TP] = "tp";
    a[RA] = "ra";
    a[FT0] = "ft0";
    a[FT1] = "ft1";
    a[FT2] = "ft2";
    a[FT3] = "ft3";
    a[FT4] = "ft4";
    a[FT5] = "ft5";
    a[FT6] = "ft6";
    a[FT7] = "ft7";
    a[FT8] = "ft8";
    a[FT9] = "ft9";
    a[FT10] = "ft10";
    a[FA0] = "fa0";
    a[FA1] = "fa1";
    a[FA2] = "fa2";
    a[FA3] = "fa3";
    a[FA4] = "fa4";
    a[FA5] = "fa5";
    a[FA6] = "fa6";
    a[FA7] = "fa7";
    a[FS0] = "fs0";
    a[FS1] = "fs1";
    a[FS2] = "fs2";
    a[FS3] = "fs3";
    a[FS4] = "fs4";
    a[FS5] = "fs5";
    a[FS6] = "fs6";
    a[FS7] = "fs7";
    a[FS8] = "fs8";
    a[FS9] = "fs9";
    a[FS10] = "fs10";
    a[FS11] = "fs11";
    a[T6] = "t6";
    a[FT11] = "ft11";
    return a;
}();

// Register name access helper.
static inline const char* regname(int r) {
    return kRegName[static_cast<size_t>(r)];
}

constexpr char kClsChar[] = {'w', 'l', 's', 'd'};

// Slot offsets.
int64_t slot_offset(Ref r, Fn* fn) {
    int s = rsval(r);
    assert(s <= fn->slot);
    if (s < 0)
        return 8 * -s;
    return -4 * (fn->slot - s);
}

// CAddr symbol literals.

void emit_addr_sym(Con* c, FILE* f) {
    assert((c->sym.type & ~Sym::SExt) == Sym::SGlo);
    fputs(str(c->sym.id), f);
    if (c->bits.i)
        fprintf(f, "+%" PRIi64, c->bits.i);
}

// Load into specific register.

void load_sym_into(Con* c, const char* rn, FILE* f) {
    char off[32];
    switch (c->sym.type) {
        case Sym::SGlo:
        case Sym::SExt:
            // medany compatibility.
            fprintf(f, "\tla %s, ", rn);
            emit_addr_sym(c, f);
            fputc('\n', f);
            return;
        case Sym::SThr:
            if (c->bits.i)
                snprintf(off, sizeof(off), "+%" PRIi64, c->bits.i);
            else
                off[0] = 0;
            fprintf(f, "\tlui %s, %%tprel_hi(%s)%s\n", rn, str(c->sym.id), off);
            fprintf(f, "\tadd %s, %s, tp, %%tprel_add(%s)%s\n", rn, rn, str(c->sym.id), off);
            fprintf(f, "\taddi %s, %s, %%tprel_lo(%s)%s\n", rn, rn, str(c->sym.id), off);
            return;
        case Sym::SExtThr:
            die("extern thread unavailable on rv64");
    }
}

void load_con_into(Con* c, int r, int k, FILE* f) {
    const char* rn = regname(r);
    switch (c->type) {
        case Con::CAddr:
            load_sym_into(c, rn, f);
            return;
        case Con::CBits: {
            int64_t n = c->bits.i;
            if (!KWIDE(k))
                n = static_cast<int32_t>(n);
            fprintf(f, "\tli %s, %" PRIi64 "\n", rn, n);
            return;
        }
        default:
            die("invalid constant");
    }
}

// Materialize memory operand into t6.

void materialize_mem(Ref* pr, Fn* fn, FILE* f) {
    Ref r = *pr;
    if (rtype(r) == RCon) {
        Con* c = &fn->con[r.val];
        if (c->type == Con::CAddr && c->sym.type != Sym::SGlo) {
            load_con_into(c, T6, Kl, f);
            *pr = Ref::tmp(static_cast<uint>(T6));
        }
    }
    if (rtype(r) == RSlot) {
        int64_t s = slot_offset(r, fn);
        if (s < -2048 || s > 2047) {
            fprintf(f, "\tli t6, %" PRId64 "\n", s);
            fputs("\tadd t6, fp, t6\n", f);
            *pr = Ref::tmp(static_cast<uint>(T6));
        }
    }
}

// Format string expansion.

void emit_reg_arg(Ref r, FILE* f) {
    assert(isreg(r));
    fputs(kRegName[r.val], f);
}

void emit_imm_or_reg(Ref r, Ins* /*i*/, Fn* fn, FILE* f) {
    switch (rtype(r)) {
        case RTmp:
            emit_reg_arg(r, f);
            return;
        case RCon: {
            Con* pc = &fn->con[r.val];
            assert(pc->type == Con::CBits);
            assert(pc->bits.i >= -2048 && pc->bits.i < 2048);
            fprintf(f, "%d", static_cast<int>(pc->bits.i));
            return;
        }
        default:
            die("invalid second argument");
    }
}

void emit_mem_arg(Ref r, Ins* i, Fn* fn, FILE* f) {
    switch (rtype(r)) {
        case RTmp:
            fprintf(f, "0(%s)", kRegName[r.val]);
            return;
        case RCon: {
            Con* pc = &fn->con[r.val];
            assert(pc->type == Con::CAddr);
            emit_addr_sym(pc, f);
            // Store / float load pseudo-instructions need a scratch register.
            if (isstore(i->op) || (isload(i->op) && KBASE(i->cls) == 1))
                fputs(", t6", f);
            return;
        }
        case RSlot: {
            int64_t offset = slot_offset(r, fn);
            assert(offset >= -2048 && offset <= 2047);
            fprintf(f, "%d(fp)", static_cast<int>(offset));
            return;
        }
        case RMem: {
            // Base+const addressing from isel.
            Mem* m = &fn->mem[r.val];
            assert(isreg(m->base));
            assert(req(m->index, Ref::none()));
            int64_t off = m->offset.type == Con::CBits ? m->offset.bits.i : 0;
            assert(off >= -2048 && off <= 2047);
            fprintf(f, "%d(%s)", static_cast<int>(off), kRegName[m->base.val]);
            return;
        }
        default:
            die("invalid address argument");
    }
}

void emit_fmt(const char* s, Ins* i, Fn* fn, FILE* f) {
    int k = i->cls;
    fputc('\t', f);
    for (;;) {
        // Process characters or escape sequences.
        char c;
        while ((c = *s++) != '%') {
            if (!c) {
                fputc('\n', f);
                return;
            }
            fputc(c, f);
        }
        char e = *s++;
        switch (e) {
            case '?':
                fputs(KBASE(k) == 0 ? "t6" : "ft11", f);
                break;
            case 'k':
                if (k != Kl)
                    fputc(kClsChar[k], f);
                break;
            case '=':
                emit_reg_arg(i->to, f);
                break;
            case '0':
                emit_reg_arg(i->arg[0], f);
                break;
            case '1':
                emit_imm_or_reg(i->arg[1], i, fn, f);
                break;
            case 'M': {
                char sel = *s++;
                assert(sel == '0' || sel == '1');
                emit_mem_arg(i->arg[sel - '0'], i, fn, f);
                break;
            }
            default:
                die("invalid escape");
        }
    }
}

// Table-driven emit dispatch.

void emit_from_table(Ins* i, Fn* fn, FILE* f) {
    for (int o = 0;; ++o) {
        if (kAsmFmt[o].op == NOp)
            die("no match for %s(%c) to=t%d/v%u a0=t%d/v%u a1=t%d/v%u || %s", optab[i->op].name,
                "wlsd"[i->cls], rtype(i->to), i -> to.val, rtype(i->arg[0]), i->arg[0].val,
                rtype(i->arg[1]), i->arg[1].val, die_ctx_str(nullptr, i));
        if (kAsmFmt[o].op != static_cast<short>(i->op))
            continue;
        short tc = kAsmFmt[o].cls;
        if (tc == static_cast<short>(i->cls) || tc == Ka || (tc == Ki && KBASE(i->cls) == 0)) {
            emit_fmt(kAsmFmt[o].fmt, i, fn, f);
            return;
        }
    }
}

// Single instruction dispatch.

void emit_ins(Ins* i, Fn* fn, FILE* f) {
    // Store with constant 0.
    if (INRANGE(i->op, Ostoreb, Ostorel) && rtype(i->arg[0]) == RCon) {
        Con* c = &fn->con[i->arg[0].val];
        if (c->type == Con::CBits && c->bits.i == 0) {
            materialize_mem(&i->arg[1], fn, f);
            const char* mn;
            switch (i->op) {
                case Ostoreb:
                    mn = "sb";
                    break;
                case Ostoreh:
                    mn = "sh";
                    break;
                case Ostorew:
                    mn = "sw";
                    break;
                case Ostorel:
                    mn = "sd";
                    break;
                default:
                    die("unreachable");
            }
            fprintf(f, "\t%s zero, ", mn);
            emit_mem_arg(i->arg[1], i, fn, f);
            fputc('\n', f);
            return;
        }
    }

    switch (i->op) {
        default:
            if (isload(i->op))
                materialize_mem(&i->arg[0], fn, f);
            else if (isstore(i->op))
                materialize_mem(&i->arg[1], fn, f);
            emit_from_table(i, fn, f);
            return;

        case Ocopy: {
            if (req(i->to, i->arg[0]))
                return;

            if (rtype(i->to) == RSlot) {
                int rt = rtype(i->arg[0]);
                if (rt == RSlot) {
                    // SLOT -> SLOT bridge.
                    int64_t off_src = slot_offset(i->arg[0], fn);
                    int64_t off_dst = slot_offset(i->to, fn);
                    const char* scratch = KBASE(i->cls) ? "ft11" : "t6";
                    const char* ld;
                    const char* st;
                    switch (i->cls) {
                        case Kw:
                            ld = "lw";
                            st = "sw";
                            break;
                        case Kl:
                            ld = "ld";
                            st = "sd";
                            break;
                        case Ks:
                            ld = "flw";
                            st = "fsw";
                            break;
                        case Kd:
                            ld = "fld";
                            st = "fsd";
                            break;
                        default:
                            die("Ocopy SLOT->SLOT illegal cls=%d", i->cls);
                    }
                    // Check for large offsets.
                    if (off_src < -2048 || off_src > 2047 || off_dst < -2048 || off_dst > 2047)
                        die("Ocopy SLOT->SLOT large offset NYI off_src=%" PRId64 " off_dst=%" PRId64
                            " || %s",
                            off_src, off_dst, die_ctx_str(nullptr, i));
                    fprintf(f, "\t%s %s, %" PRId64 "(fp)\n", ld, scratch, off_src);
                    fprintf(f, "\t%s %s, %" PRId64 "(fp)\n", st, scratch, off_dst);
                    return;
                }
                if (rt == RCon)
                    die("unimplemented Ocopy SLOT-dst cls=%c to=t%d/v%u a0=t%d/v%u || %s",
                        "wlsd"[i->cls], rtype(i->to), i -> to.val, rtype(i->arg[0]), i->arg[0].val,
                        die_ctx_str(nullptr, i));
                assert(isreg(i->arg[0]));
                Ins ii = *i;
                ii.arg[1] = ii.to;
                ii.to = Ref::none();
                switch (ii.cls) {
                    case Kw:
                        ii.op = Ostorew;
                        break;
                    case Kl:
                        ii.op = Ostorel;
                        break;
                    case Ks:
                        ii.op = Ostores;
                        break;
                    case Kd:
                        ii.op = Ostored;
                        break;
                }
                // Revert dopm Ks/Kd to Kw for Ostore*.
                ii.cls = Kw;
                materialize_mem(&ii.arg[1], fn, f);
                emit_from_table(&ii, fn, f);
                return;
            }

            assert(isreg(i->to));
            switch (rtype(i->arg[0])) {
                case RCon:
                    load_con_into(&fn->con[i->arg[0].val], static_cast<int>(i->to.val),
                                  static_cast<int>(i->cls), f);
                    return;
                case RSlot: {
                    Ins ii = *i;
                    ii.op = Oload;
                    materialize_mem(&ii.arg[0], fn, f);
                    emit_from_table(&ii, fn, f);
                    return;
                }
                default:
                    assert(isreg(i->arg[0]));
                    emit_from_table(i, fn, f);
                    return;
            }
        }

        case Onop:
            return;

        case Oaddr: {
            const char* rn = kRegName[i->to.val];
            switch (rtype(i->arg[0])) {
                case RSlot: {
                    int64_t s = slot_offset(i->arg[0], fn);
                    if (-s < 2048) {
                        fprintf(f, "\tadd %s, fp, %" PRId64 "\n", rn, s);
                    } else {
                        fprintf(f, "\tli %s, %" PRId64 "\n", rn, s);
                        fprintf(f, "\tadd %s, fp, %s\n", rn, rn);
                    }
                    return;
                }
                case RCon: {
                    // addr $sym generated from IRGen.
                    Con* c = &fn->con[i->arg[0].val];
                    assert(c->type == Con::CAddr);
                    load_sym_into(c, rn, f);
                    return;
                }
                default:
                    die("invalid Oaddr argument");
            }
        }

        case Ocall:
            switch (rtype(i->arg[0])) {
                case RCon: {
                    Con* con = &fn->con[i->arg[0].val];
                    if (con->type != Con::CAddr || (con->sym.type & Sym::SThr) || con->bits.i)
                        die("invalid call argument");
                    fprintf(f, "\tcall %s\n", str(con->sym.id));
                    return;
                }
                case RTmp:
                    emit_fmt("jalr %0", i, fn, f);
                    return;
                default:
                    die("invalid call argument");
            }

        case Osalloc:
            emit_fmt("sub sp, sp, %0", i, fn, f);
            if (!req(i->to, Ref::none()))
                emit_fmt("mv %=, sp", i, fn, f);
            return;

        case Odbgloc:
            emitdbgloc(i->arg[0].val, i->arg[1].val, f);
            return;
    }
}

// Function prologue / epilogue helpers.

void save_clobbers(Fn* fn, FILE* f) {
    int off = 0;
    for (int* pr = rv64_rclob; *pr >= 0; ++pr) {
        if (!(fn->reg & BIT(*pr)))
            continue;
        fprintf(f, "\t%s %s, %d(sp)\n", *pr < FT0 ? "sd" : "fsd", regname(*pr), off);
        off += 8;
    }
}

void restore_clobbers(Fn* fn, FILE* f) {
    int off = 0;
    for (int* pr = rv64_rclob; *pr >= 0; ++pr) {
        if (!(fn->reg & BIT(*pr)))
            continue;
        fprintf(f, "\t%s %s, %d(sp)\n", *pr < FT0 ? "ld" : "fld", regname(*pr), off);
        off += 8;
    }
}

// Compute total stack frame size.
int compute_frame_size(Fn* fn) {
    int frame = (16 + 4 * fn->slot + 15) & ~15;
    for (int* pr = rv64_rclob; *pr >= 0; ++pr) {
        if (fn->reg & BIT(*pr))
            frame += 8;
    }
    return (frame + 15) & ~15;
}

// Adjust sp by delta.
void emit_sp_adjust(const char* op, const char* base, int delta, FILE* f) {
    if (delta <= 2048) {
        fprintf(f, "\t%s sp, %s, -%d\n", op, base, delta);
    } else {
        fprintf(f, "\tli t6, -%d\n\t%s sp, %s, t6\n", delta, op, base);
    }
}

// Fused cmp+jnz -> single conditional branch.

struct BrCfg {
    const char* mn;  // rv64 branch mnemonic.
    bool swap;       // Swap operands.
};

// Branch mnemonic for each condition code.
constexpr BrCfg kBrCfg[NCmpI] = {
    /* Cieq  */ {"beq", false},  /* Cine  */ {"bne", false},
    /* Cisge */ {"bge", false},  /* Cisgt */ {"blt", true},
    /* Cisle */ {"bge", true},   /* Cislt */ {"blt", false},
    /* Ciuge */ {"bgeu", false}, /* Ciugt */ {"bltu", true},
    /* Ciule */ {"bgeu", true},  /* Ciult */ {"bltu", false},
};

// Logical negation of condition codes.
constexpr int kCmpNeg[NCmpI] = {
    /* Cieq  */ Cine,  /* Cine  */ Cieq,  /* Cisge */ Cislt, /* Cisgt */ Cisle,
    /* Cisle */ Cisgt, /* Cislt */ Cisge, /* Ciuge */ Ciult, /* Ciugt */ Ciule,
    /* Ciule */ Ciugt, /* Ciult */ Ciuge,
};

// Resolve fused branch operand.
const char* branch_operand(Ref r, int ck, Fn* fn, FILE* f, bool* slot_used, Blk* b) {
    if (rtype(r) == RSlot) {
        if (*slot_used)
            die("fused branch: both operands spilled @%d", static_cast<int>(b->id));
        Ins ii = {};
        ii.arg[0] = r;
        emit_fmt(KWIDE(ck) ? "ld t6, %M0" : "lw t6, %M0", &ii, fn, f);
        *slot_used = true;
        return kRegName[T6];
    }
    assert(isreg(r));
    return kRegName[r.val];
}

}  // namespace

// rv64_emitfn: Function entry point.

namespace {
// Global block ID base.
int g_blkid_base = 0;
}  // namespace

void rv64_emitfn(Fn* fn, FILE* f) {
    emitfnlnk(fn->name, &fn->lnk, f);

    // Prologue.
    if (fn->vararg) {
        // TODO: only allocate for a* unused by named arguments.
        fputs("\tadd sp, sp, -64\n", f);
        for (int r = A0; r <= A7; ++r)
            fprintf(f, "\tsd %s, %d(sp)\n", regname(r), 8 * (r - A0));
    }
    fputs("\tsd fp, -16(sp)\n", f);
    fputs("\tsd ra, -8(sp)\n", f);
    fputs("\tadd fp, sp, -16\n", f);

    int frame = compute_frame_size(fn);
    emit_sp_adjust("add", "sp", frame, f);
    save_clobbers(fn, f);

    // Body.
    bool need_label = false;
    for (Blk* b = fn->start; b; b = b->link) {
        if (need_label || b->npred > 1)
            fprintf(f, ".L%d:\n", g_blkid_base + static_cast<int>(b->id));
        for (Ins* i = b->ins; i != b->ins + b->nins; ++i)
            emit_ins(i, fn, f);
        need_label = true;

        switch (b->jmp.type) {
            case Jhlt:
                fputs("\tebreak\n", f);
                break;

            case Jret0:
                if (fn->dynalloc)
                    emit_sp_adjust("add", "fp", frame - 16, f);
                restore_clobbers(fn, f);
                fprintf(f,
                        "\tadd sp, fp, %d\n"
                        "\tld ra, 8(fp)\n"
                        "\tld fp, 0(fp)\n"
                        "\tret\n",
                        16 + fn->vararg * 64);
                break;

            case Jjmp:
            emit_jmp:
                if (b->s1 != b->link)
                    fprintf(f, "\tj .L%d\n", g_blkid_base + static_cast<int>(b->s1->id));
                else
                    need_label = false;
                break;

            case Jjnz: {
                // Ordering optimization for fall-through.
                bool neg = false;
                if (b->link == b->s2) {
                    Blk* s = b->s1;
                    b->s1 = b->s2;
                    b->s2 = s;
                    neg = true;
                }
                if (rtype(b->jmp.arg) == RSlot) {
                    Ins ii = {};
                    ii.arg[0] = b->jmp.arg;
                    emit_fmt("lw t6, %M0", &ii, fn, f);
                    b->jmp.arg = Ref::tmp(static_cast<uint>(T6));
                }
                assert(isreg(b->jmp.arg));
                fprintf(f, "\tb%sz %s, .L%d\n", neg ? "ne" : "eq", kRegName[b->jmp.arg.val],
                        g_blkid_base + static_cast<int>(b->s2->id));
                goto emit_jmp;
            }

            default:
                // Fused integer cmp+jnz.
                if (b->jmp.type >= Jjfieq && b->jmp.type <= Jjfiult) {
                    int cc = static_cast<int>(b->jmp.type) - Jjfieq;
                    int ck = b->jmp.cls;

                    bool neg = false;
                    if (b->link == b->s2) {
                        Blk* s = b->s1;
                        b->s1 = b->s2;
                        b->s2 = s;
                        neg = true;
                    }
                    BrCfg cfg = kBrCfg[neg ? cc : kCmpNeg[cc]];

                    bool slot_used = false;
                    const char* r0 = branch_operand(b->jmp.arg, ck, fn, f, &slot_used, b);
                    const char* r1 = branch_operand(b->jmp.arg2, ck, fn, f, &slot_used, b);
                    if (cfg.swap) {
                        const char* t = r0;
                        r0 = r1;
                        r1 = t;
                    }
                    fprintf(f, "\t%s %s, %s, .L%d\n", cfg.mn, r0, r1,
                            g_blkid_base + static_cast<int>(b->s2->id));
                    goto emit_jmp;
                }
                break;
        }
    }

    g_blkid_base += static_cast<int>(fn->nblk);
    elf_emitfnfin(fn->name, f);
}

// Built-in runtime.

namespace {

void emit_runtime_memset(FILE* f) {
    fprintf(f,
            ".globl __rcc_memset\n"
            "__rcc_memset:\n"
            "\tandi a1, a1, 0xff\n"
            "\tslli t0, a1, 8\n"
            "\tor a1, a1, t0\n"
            "\tslli t0, a1, 16\n"
            "\tor a1, a1, t0\n"
            "\tslli t0, a1, 32\n"
            "\tor a1, a1, t0\n"
            "\tmv t1, a0\n"
            "\tli t2, 64\n"
            ".Lrcc_ms64:\n"
            "\tblt a2, t2, .Lrcc_ms8\n"
            "\tsd a1, 0(t1)\n"
            "\tsd a1, 8(t1)\n"
            "\tsd a1, 16(t1)\n"
            "\tsd a1, 24(t1)\n"
            "\tsd a1, 32(t1)\n"
            "\tsd a1, 40(t1)\n"
            "\tsd a1, 48(t1)\n"
            "\tsd a1, 56(t1)\n"
            "\taddi t1, t1, 64\n"
            "\taddi a2, a2, -64\n"
            "\tj .Lrcc_ms64\n"
            ".Lrcc_ms8:\n"
            "\tli t2, 8\n"
            ".Lrcc_ms8l:\n"
            "\tblt a2, t2, .Lrcc_ms1\n"
            "\tsd a1, 0(t1)\n"
            "\taddi t1, t1, 8\n"
            "\taddi a2, a2, -8\n"
            "\tj .Lrcc_ms8l\n"
            ".Lrcc_ms1:\n"
            "\tbeqz a2, .Lrcc_msd\n"
            "\tsb a1, 0(t1)\n"
            "\taddi t1, t1, 1\n"
            "\taddi a2, a2, -1\n"
            "\tj .Lrcc_ms1\n"
            ".Lrcc_msd:\n"
            "\tret\n"
            ".type __rcc_memset, @function\n"
            ".size __rcc_memset, .-__rcc_memset\n\n");
}

void emit_runtime_memcpy(FILE* f) {
    fprintf(f,
            ".globl __rcc_memcpy\n"
            "__rcc_memcpy:\n"
            "\tmv t1, a0\n"
            ".Lrcc_mc32:\n"
            "\tli t2, 32\n"
            "\tblt a2, t2, .Lrcc_mc8\n"
            "\tld t3, 0(a1)\n"
            "\tld t4, 8(a1)\n"
            "\tld t5, 16(a1)\n"
            "\tld t6, 24(a1)\n"
            "\tsd t3, 0(t1)\n"
            "\tsd t4, 8(t1)\n"
            "\tsd t5, 16(t1)\n"
            "\tsd t6, 24(t1)\n"
            "\taddi a1, a1, 32\n"
            "\taddi t1, t1, 32\n"
            "\taddi a2, a2, -32\n"
            "\tj .Lrcc_mc32\n"
            ".Lrcc_mc8:\n"
            "\tli t2, 8\n"
            ".Lrcc_mc8l:\n"
            "\tblt a2, t2, .Lrcc_mc1\n"
            "\tld t3, 0(a1)\n"
            "\tsd t3, 0(t1)\n"
            "\taddi a1, a1, 8\n"
            "\taddi t1, t1, 8\n"
            "\taddi a2, a2, -8\n"
            "\tj .Lrcc_mc8l\n"
            ".Lrcc_mc1:\n"
            "\tbeqz a2, .Lrcc_mcd\n"
            "\tlb t3, 0(a1)\n"
            "\tsb t3, 0(t1)\n"
            "\taddi a1, a1, 1\n"
            "\taddi t1, t1, 1\n"
            "\taddi a2, a2, -1\n"
            "\tj .Lrcc_mc1\n"
            ".Lrcc_mcd:\n"
            "\tret\n"
            ".type __rcc_memcpy, @function\n"
            ".size __rcc_memcpy, .-__rcc_memcpy\n\n");
}

void emit_runtime_memmove(FILE* f) {
    fprintf(f,
            ".globl __rcc_memmove\n"
            "__rcc_memmove:\n"
            "\tbgeu a0, a1, .Lrcc_mm_chk\n"
            "\tj __rcc_memcpy\n"
            ".Lrcc_mm_chk:\n"
            "\tadd t0, a1, a2\n"
            "\tbgeu a0, t0, __rcc_memcpy\n"
            "\tadd t1, a0, a2\n"
            "\tadd a1, a1, a2\n"
            ".Lrcc_mmb32:\n"
            "\tli t2, 32\n"
            "\tblt a2, t2, .Lrcc_mmb8\n"
            "\taddi t1, t1, -32\n"
            "\taddi a1, a1, -32\n"
            "\tld t3, 0(a1)\n"
            "\tld t4, 8(a1)\n"
            "\tld t5, 16(a1)\n"
            "\tld t6, 24(a1)\n"
            "\tsd t3, 0(t1)\n"
            "\tsd t4, 8(t1)\n"
            "\tsd t5, 16(t1)\n"
            "\tsd t6, 24(t1)\n"
            "\taddi a2, a2, -32\n"
            "\tj .Lrcc_mmb32\n"
            ".Lrcc_mmb8:\n"
            "\tli t2, 8\n"
            ".Lrcc_mmb8l:\n"
            "\tblt a2, t2, .Lrcc_mmb1\n"
            "\taddi t1, t1, -8\n"
            "\taddi a1, a1, -8\n"
            "\tld t3, 0(a1)\n"
            "\tsd t3, 0(t1)\n"
            "\taddi a2, a2, -8\n"
            "\tj .Lrcc_mmb8l\n"
            ".Lrcc_mmb1:\n"
            "\tbeqz a2, .Lrcc_mmd\n"
            "\taddi t1, t1, -1\n"
            "\taddi a1, a1, -1\n"
            "\tlb t3, 0(a1)\n"
            "\tsb t3, 0(t1)\n"
            "\taddi a2, a2, -1\n"
            "\tj .Lrcc_mmb1\n"
            ".Lrcc_mmd:\n"
            "\tret\n"
            ".type __rcc_memmove, @function\n"
            ".size __rcc_memmove, .-__rcc_memmove\n\n");
}

}  // namespace

void rv64_emitfin(Ctx* ctx, FILE* f) {
    fprintf(f, ".text\n");
    emit_runtime_memset(f);
    emit_runtime_memcpy(f);
    emit_runtime_memmove(f);
    elf_emitfin(ctx, f);
}

}  // namespace rcc
