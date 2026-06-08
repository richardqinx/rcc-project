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

/// @file backend/emit.cc
/// @brief Target-independent assembly/data/debug section output tools.

#include "rcc/all.h"

#include <cinttypes>
#include <cstring>
#include <vector>

namespace rcc {

// Section types.

namespace {

enum Sec : int { kSecText = 0, kSecData = 1, kSecBss = 2 };

// Given (sec, thread), return the name/instruction string after .section.
// When thread=true, SecText is unreachable.
const char* section_name(int sec, bool thread) {
    if (!thread) {
        switch (sec) {
            case kSecText:
                return ".text";
            case kSecData:
                return ".data";
            case kSecBss:
                return ".bss";
        }
    } else {
        switch (sec) {
            case kSecText:
                return ".abort \"unreachable\"";
            case kSecData:
                return ".section .tdata,\"awT\"";
            case kSecBss:
                return ".section .tbss,\"awT\"";
        }
    }
    return nullptr;  // unreachable
}

// Write ".globl / label / section switch" header. Apple branch has TLV trampolines.
void write_lnk(char* n, Lnk* l, int sec_kind, FILE* f) {
    const char* pfx = (n[0] == '"') ? "" : T.assym;
    const char* sfx = "";

    if (T.apple && l->thread) {
        l->sec = const_cast<char*>("__DATA");
        l->secf = const_cast<char*>("__thread_data,thread_local_regular");
        sfx = "$tlv$init";
        fputs(".section __DATA,__thread_vars,thread_local_variables\n", f);
        fprintf(f, "%s%s:\n", pfx, n);
        fprintf(f,
                "\t.quad __tlv_bootstrap\n"
                "\t.quad 0\n"
                "\t.quad %s%s%s\n\n",
                pfx, n, sfx);
    }

    if (l->sec) {
        fprintf(f, ".section %s", l->sec);
        if (l->secf)
            fprintf(f, ",%s", l->secf);
    } else {
        fputs(section_name(sec_kind, l->thread != 0), f);
    }
    fputc('\n', f);

    if (l->align)
        fprintf(f, ".balign %d\n", l->align);
    if (l->export_)
        fprintf(f, ".globl %s%s\n", pfx, n);
    fprintf(f, "%s%s%s:\n", pfx, n, sfx);
}

}  // namespace

void emitfnlnk(char* n, Lnk* l, FILE* f) {
    write_lnk(n, l, kSecText, f);
}

// Data sections.

namespace {
int64_t g_data_zero = 0;

struct DataDecl {
    const char* decl;
    int64_t mask;
};

constexpr DataDecl kDataDecl[] = {
    /* DStart */ {nullptr, 0},
    /* DEnd   */ {nullptr, 0},
    /* DB     */ {"\t.byte", 0xffLL},
    /* DH     */ {"\t.short", 0xffffLL},
    /* DW     */ {"\t.int", 0xffffffffLL},
    /* DL     */ {"\t.quad", -1LL},
    /* DZ     */ {nullptr, 0},
};

}  // namespace

void emitdat(Dat* d, FILE* f) {
    switch (d->type) {
        case Dat::DStart:
            g_data_zero = 0;
            return;

        case Dat::DEnd:
            if (d->lnk->common) {
                if (g_data_zero == -1)
                    die("invalid common data definition");
                const char* p = (d->name[0] == '"') ? "" : T.assym;
                fprintf(f, ".comm %s%s,%" PRId64, p, d->name, g_data_zero);
                if (d->lnk->align)
                    fprintf(f, ",%d", d->lnk->align);
                fputc('\n', f);
            } else if (g_data_zero != -1) {
                write_lnk(d->name, d->lnk, kSecBss, f);
                fprintf(f, "\t.fill %" PRId64 ",1,0\n", g_data_zero);
            }
            return;

        case Dat::DZ:
            if (g_data_zero != -1)
                g_data_zero += d->u.num;
            else
                fprintf(f, "\t.fill %" PRId64 ",1,0\n", d->u.num);
            return;

        case Dat::DB:
        case Dat::DH:
        case Dat::DW:
        case Dat::DL:
            break;
    }

    // Switch to .data and flush accumulated zeros on first non-zero content.
    if (g_data_zero != -1) {
        write_lnk(d->name, d->lnk, kSecData, f);
        if (g_data_zero > 0)
            fprintf(f, "\t.fill %" PRId64 ",1,0\n", g_data_zero);
        g_data_zero = -1;
    }

    const DataDecl& dd = kDataDecl[d->type];
    if (d->isstr) {
        if (d->type != Dat::DB)
            err(const_cast<char*>("strings only supported for 'b' currently"));
        fprintf(f, "\t.ascii %s\n", d->u.str);
    } else if (d->isref) {
        const char* p = (d->u.ref.name[0] == '"') ? "" : T.assym;
        fprintf(f, "%s %s%s%+" PRId64 "\n", dd.decl, p, d->u.ref.name, d->u.ref.off);
    } else {
        fprintf(f, "%s %" PRId64 "\n", dd.decl, d->u.num & dd.mask);
    }
}

// FP constant pool (stashbits + emitfin).

struct Asmbits {
    bits n;
    int size;
    Asmbits* link;
};

int stashbits(Ctx* ctx, bits n, int size) {
    assert(size == 4 || size == 8 || size == 16);
    Asmbits** pb;
    Asmbits* b;
    int i = 0;
    for (pb = &ctx->emit_stash; (b = *pb); pb = &b->link, ++i) {
        if (size <= b->size && b->n == n)
            return i;
    }
    b = static_cast<Asmbits*>(emalloc(sizeof(Asmbits)));
    b->n = n;
    b->size = size;
    b->link = nullptr;
    *pb = b;
    return i;
}

namespace {

// Layout by .quad / .quad 0 (16B) / .quad (8B) / .int (4B).
void emit_fp_pool(Ctx* ctx, FILE* f, const char* sec[3]) {
    if (!ctx->emit_stash)
        return;
    fprintf(f, "/* floating point constants */\n");

    union {
        int32_t i;
        float f;
    } u;

    // Three passes from largest to smallest (16, 8, 4).
    for (int lg = 4; lg >= 2; --lg) {
        int width = 1 << lg;
        int i = 0;
        for (Asmbits* b = ctx->emit_stash; b; b = b->link, ++i) {
            if (b->size != width)
                continue;
            fprintf(f,
                    ".section %s\n"
                    ".p2align %d\n"
                    "%sfp%d:",
                    sec[lg - 2], lg, T.asloc, i);
            if (lg == 4) {
                fprintf(f, "\n\t.quad %" PRId64 "\n\t.quad 0\n\n", static_cast<int64_t>(b->n));
            } else if (lg == 3) {
                double dv;
                memcpy(&dv, &b->n, sizeof(dv));
                fprintf(f, "\n\t.quad %" PRId64 " /* %f */\n\n", static_cast<int64_t>(b->n), dv);
            } else /* lg == 2 */ {
                u.i = static_cast<int32_t>(b->n);
                fprintf(f, "\n\t.int %" PRId32 " /* %f */\n\n", u.i, static_cast<double>(u.f));
            }
        }
    }

    // Free linked list.
    while (Asmbits* b = ctx->emit_stash) {
        ctx->emit_stash = b->link;
        free(b);
    }
}

}  // namespace

void elf_emitfin(Ctx* ctx, FILE* f) {
    static const char* sec[3] = {".rodata", ".rodata", ".rodata"};
    emit_fp_pool(ctx, f, sec);
    fprintf(f, ".section .note.GNU-stack,\"\",@progbits\n");
}

void elf_emitfnfin(char* fn, FILE* f) {
    fprintf(f, ".type %s, @function\n", fn);
    fprintf(f, ".size %s, .-%s\n", fn, fn);
}

void macho_emitfin(Ctx* ctx, FILE* f) {
    static const char* sec[3] = {
        "__TEXT,__literal4,4byte_literals",
        "__TEXT,__literal8,8byte_literals",
        "__TEXT,__literal16,16byte_literals",
    };
    emit_fp_pool(ctx, f, sec);
}

void pe_emitfin(Ctx* ctx, FILE* f) {
    static const char* sec[3] = {".rodata", ".rodata", ".rodata"};
    emit_fp_pool(ctx, f, sec);
}

// DWARF .file / .loc.

namespace {
std::vector<uint32_t> g_dbg_files;  // intern id sequence.
uint32_t g_dbg_curfile = 0;         // 1-based (gas requires > 0).
}  // namespace

void emitdbgfile(char* fn, FILE* f) {
    uint32_t id = intern(fn);
    for (size_t i = 0; i < g_dbg_files.size(); ++i) {
        if (g_dbg_files[i] == id) {
            g_dbg_curfile = static_cast<uint32_t>(i + 1);
            return;
        }
    }
    g_dbg_files.push_back(id);
    g_dbg_curfile = static_cast<uint32_t>(g_dbg_files.size());
    fprintf(f, ".file %u %s\n", g_dbg_curfile, fn);
}

void emitdbgloc(uint line, uint col, FILE* f) {
    if (col != 0)
        fprintf(f, "\t.loc %u %u %u\n", g_dbg_curfile, line, col);
    else
        fprintf(f, "\t.loc %u %u\n", g_dbg_curfile, line);
}

}  // namespace rcc
