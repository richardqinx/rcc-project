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

/// @file print.cc
/// @brief IR text output: Fn / Blk / Phi / Ins / Jmp / Con / Ref

#include "rcc/all.h"
#include "rcc/irgen.h"

#include <cinttypes>

namespace rcc {

namespace {

/// K class characters: Kw='w' Kl='l' Ks='s' Kd='d'. Others are treated as errors.
char cls_char(int k) {
    switch (k) {
        case Kw:
            return 'w';
        case Kl:
            return 'l';
        case Ks:
            return 's';
        case Kd:
            return 'd';
        default:
            return '?';
    }
}

const char* jmp_name(int j) {
    static constexpr auto tab = [] {
        std::array<const char*, NJmp> a{};
#define X(n) a[J##n] = #n;
        JMPS(X)
#undef X
        return a;
    }();
    if (j <= Jxxx || j >= NJmp)
        return "??";
    return tab[static_cast<size_t>(j)];
}

/// Mem (= Addr) internal addressing component output: [ offset + base + scale*index ]
void print_mem(Mem* m, Fn* fn, FILE* f);

bool op_needs_cls_suffix(int op) {
    switch (op) {
        case Oarg:
        case Oswap:
        case Oxcmp:
        case Oacmp:
        case Oacmn:
        case Oafcmp:
        case Oxtest:
        case Oxdiv:
        case Oxidiv:
            return true;
        default:
            return false;
    }
}

}  // namespace

// printcon

static void printcon(Con* c, FILE* f) {
    switch (c->type) {
        case Con::CUndef:
            return;
        case Con::CAddr:
            if (c->sym.type & Sym::SExt)
                fputs("extern ", f);
            if (c->sym.type & Sym::SThr)
                fputs("thread ", f);
            fprintf(f, "$%s", str(c->sym.id));
            if (c->bits.i)
                fprintf(f, "%+" PRId64, c->bits.i);
            return;
        case Con::CBits:
            if (c->flt == 1)
                fprintf(f, "s_%f", static_cast<double>(c->bits.s));
            else if (c->flt == 2)
                fprintf(f, "d_%lf", c->bits.d);
            else
                fprintf(f, "%" PRId64, c->bits.i);
            return;
    }
}

// printref

void printref(Ref r, Fn* fn, FILE* f) {
    switch (rtype(r)) {
        case RTmp:
            if (r.val < Tmp0)
                fprintf(f, "R%u", r.val);
            else
                fprintf(f, "%%%s", fn->tmp[r.val].name);
            return;

        case RCon:
            if (req(r, Ref::undef()))
                fputs("UNDEF", f);
            else
                printcon(&fn->con[r.val], f);
            return;

        case RSlot:
            fprintf(f, "S%d", rsval(r));
            return;

        case RCall:
            fprintf(f, "%04x", r.val);
            return;

        case RType:
            fprintf(f, ":%s", typ[r.val].name);
            return;

        case RMem:
            print_mem(&fn->mem[r.val], fn, f);
            return;

        case RInt:
            fprintf(f, "%d", rsval(r));
            return;

        case -1:
            fputc('R', f);
            return;
    }
}

namespace {

void print_mem(Mem* m, Fn* fn, FILE* f) {
    fputc('[', f);
    bool need_plus = false;
    if (m->offset.type != Con::CUndef) {
        printcon(&m->offset, f);
        need_plus = true;
    }
    if (!req(m->base, Ref::none())) {
        if (need_plus)
            fputs(" + ", f);
        printref(m->base, fn, f);
        need_plus = true;
    }
    if (!req(m->index, Ref::none())) {
        if (need_plus)
            fputs(" + ", f);
        fprintf(f, "%d * ", m->scale);
        printref(m->index, fn, f);
    }
    fputc(']', f);
}

// Phi / Ins / Jmp single-line output

void print_phi_line(Phi* p, Fn* fn, FILE* f) {
    fputc('\t', f);
    printref(p->to, fn, f);
    fprintf(f, " =%c phi ", cls_char(p->cls));
    assert(p->narg && "phi has no arg");
    for (uint n = 0; n < p->narg; n++) {
        fprintf(f, "@%s ", p->blk[n]->name);
        printref(p->arg[n], fn, f);
        if (n + 1 < p->narg)
            fputs(", ", f);
    }
    fputc('\n', f);
}

void print_ins_line(Ins* i, Fn* fn, FILE* f) {
    fputc('\t', f);
    if (!req(i->to, Ref::none())) {
        printref(i->to, fn, f);
        fprintf(f, " =%c ", cls_char(static_cast<int>(i->cls)));
    }
    assert(optab[i->op].name && "missing op name in optab");
    fputs(optab[i->op].name, f);

    if (req(i->to, Ref::none()) && op_needs_cls_suffix(i->op))
        fputc(cls_char(static_cast<int>(i->cls)), f);

    if (!req(i->arg[0], Ref::none())) {
        fputc(' ', f);
        printref(i->arg[0], fn, f);
    }
    if (!req(i->arg[1], Ref::none())) {
        fputs(", ", f);
        printref(i->arg[1], fn, f);
    }
    fputc('\n', f);
}

/// Terminating instructions: ret / jmp / jnz / floating-point conditional jump / hlt
void print_jmp_line(Blk* b, Fn* fn, FILE* f) {
    int jt = b->jmp.type;

    if (isret(jt)) {
        fputs("\tret", f);
        if (jt != Jret0 || !req(b->jmp.arg, Ref::none())) {
            fputc(' ', f);
            printref(b->jmp.arg, fn, f);
        }
        fputc('\n', f);
        return;
    }

    if (jt == Jhlt) {
        fputs("\thlt\n", f);
        return;
    }

    if (jt == Jjmp) {
        // If fall-through to next block, jmp can be omitted (QBE output style)
        if (b->s1 != b->link)
            fprintf(f, "\tjmp @%s\n", b->s1->name);
        return;
    }

    // jnz or jf* series conditional jumps: two successors
    fprintf(f, "\t%s ", jmp_name(jt));
    if (jt == Jjnz) {
        printref(b->jmp.arg, fn, f);
        fputs(", ", f);
    }
    assert(b->s1 && b->s2 && "branch missing successor");
    fprintf(f, "@%s, @%s\n", b->s1->name, b->s2->name);
}

void print_call(Ins* call, Ins** args, size_t nargs, Fn* fn, FILE* f) {
    fputc('\t', f);
    if (!req(call->to, Ref::none())) {
        printref(call->to, fn, f);
        fprintf(f, " =%c ", cls_char(static_cast<int>(call->cls)));
    }
    fputs("call ", f);
    printref(call->arg[0], fn, f);
    fputc('(', f);
    for (size_t k = 0; k < nargs; k++) {
        if (k)
            fputs(", ", f);
        Ins* a = args[k];
        fprintf(f, "%c ", cls_char(static_cast<int>(a->cls)));
        printref(a->arg[0], fn, f);
    }
    fputs(")\n", f);
}

void print_block(Blk* b, Fn* fn, FILE* f) {
    fprintf(f, "@%s\n", b->name);
    for (Phi* p = b->phi; p; p = p->link)
        print_phi_line(p, fn, f);

    // Scan: skip Opar at entry block start (already in signature); Oarg sequence is merged and
    // printed when encountering Ocall, and call itself is output at the merge point.
    Ins* pending_args[NIns];
    size_t npending = 0;
    for (uint k = 0; k < b->nins; k++) {
        Ins* i = &b->ins[k];
        if (i->op == Onop)
            continue;  // promote/coalesce marks instructions as nop to "delete" them, QBE text
                       // doesn't accept nop keyword
        if (i->op == Opar)
            continue;
        if (i->op == Oarg) {
            assert(npending < NIns && "too many call args");
            pending_args[npending++] = i;
            continue;
        }
        if (i->op == Ocall) {
            print_call(i, pending_args, npending, fn, f);
            npending = 0;
            continue;
        }
        // There should be no dangling Oarg before normal instructions
        assert(npending == 0 && "orphan arg before non-call op");
        print_ins_line(i, fn, f);
    }
    assert(npending == 0 && "orphan arg at end of block");

    print_jmp_line(b, fn, f);
}

void print_signature(Fn* fn, FILE* f) {
    fputs("export\nfunction ", f);

    int ret_cls = Kx;
    for (Blk* b = fn->start; b; b = b->link) {
        int jt = b->jmp.type;
        if (!isret(jt))
            continue;
        if (jt >= Jretw && jt <= Jretd)
            ret_cls = jt - Jretw;  // Jretw..Jretd align with Kw..Kd
        break;
    }
    if (ret_cls >= 0)
        fprintf(f, "%c ", cls_char(ret_cls));
    fprintf(f, "$%s(", fn->name);

    // Scan Opar in entry to collect parameters
    Blk* entry = fn->start;
    bool first = true;
    if (entry) {
        for (uint k = 0; k < entry->nins; k++) {
            Ins* i = &entry->ins[k];
            if (i->op != Opar)  // Opar are contiguous at entry head, break on first non-Opar
                break;
            if (!first)
                fputs(", ", f);
            fprintf(f, "%c ", cls_char(static_cast<int>(i->cls)));
            printref(i->to, fn, f);
            first = false;
        }
    }
    fputs(") {\n", f);
}

}  // namespace

// printfn - Top-level assembly

void printfn(Fn* fn, FILE* f) {
    print_signature(fn, f);
    for (Blk* b = fn->start; b; b = b->link)
        print_block(b, fn, f);
    fputs("}\n", f);
}

// print_globals - Data segment

namespace {

constexpr size_t ELEM_BYTES = 4;  // Kw / Ks are both 4 bytes

void print_global_cell(int elem_cls, const std::variant<int32_t, float>& v, FILE* f) {
    if (elem_cls == Ks) {
        // QBE floating-point constant literal s_<float>
        float fv = std::holds_alternative<float>(v) ? std::get<float>(v)
                                                    : static_cast<float>(std::get<int32_t>(v));
        fprintf(f, "s s_%f", static_cast<double>(fv));
    } else {
        int32_t iv = std::holds_alternative<int32_t>(v) ? std::get<int32_t>(v)
                                                        : static_cast<int32_t>(std::get<float>(v));
        fprintf(f, "w %d", iv);
    }
}

}  // namespace

void print_globals(const std::vector<GlobalData>& globals, FILE* f) {
    for (const auto& g : globals) {
        fprintf(f, "data $%s = align 4 { ", g.name.c_str());
        if (g.is_zeroinit) {
            fprintf(f, "z %zu", g.total_elems * ELEM_BYTES);
        } else {
            for (size_t i = 0; i < g.init_vals.size(); i++) {
                if (i)
                    fputs(", ", f);
                print_global_cell(g.elem_cls, g.init_vals[i], f);
            }
        }
        fputs(" }\n", f);
    }
}

}  // namespace rcc
