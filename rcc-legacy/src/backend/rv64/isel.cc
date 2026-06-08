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

/// @file isel.cc
/// @brief RISC-V 64 instruction selection.

#include "rcc/rv64.h"

#include <cinttypes>
#include <climits>
#include <cstdio>

namespace rcc {
namespace {

// Operand role classification.

// is_mem_arg: Does the arg slot pointed to by r have "memory operand" semantics?
bool is_mem_arg(Ref* r, int op, Ins* i) {
    if (isload(op) || op == Ocall)
        return r == &i->arg[0];
    if (isstore(op))
        return r == &i->arg[1];
    return false;
}

// is_imm_arg: Is r the immediate slot for this op?
bool is_imm_arg(Ref* r, int op, Ins* i) {
    return rv64_op[op].imm && r == &i->arg[1];
}

// Compare op -> (sign, swap, neg) tuple.

struct CmpCfg {
    uint8_t sign;  // 1=signed (Ocsltl), 0=unsigned (Ocultl)
    uint8_t swap;  // 1=swap arg0/arg1
    uint8_t neg;   // 1=negate i.to
};

constexpr CmpCfg kIntOrderCmp[8] = {
    /* Cisge */ {1, 0, 1},  // a >= b  ⇔  !(a < b)
    /* Cisgt */ {1, 1, 0},  // a >  b  ⇔  (b < a)
    /* Cisle */ {1, 1, 1},  // a <= b  ⇔  !(b < a)
    /* Cislt */ {1, 0, 0},  // a <  b
    /* Ciuge */ {0, 0, 1},
    /* Ciugt */ {0, 1, 0},
    /* Ciule */ {0, 1, 1},
    /* Ciult */ {0, 0, 0},
};

// Utilities.

// boolean negate: emit `xor pr, src, 1`
void negate_to(Ctx* ctx, Ref* pr, Fn* fn) {
    Ref src = newtmp(ctx, "isel", Kw, fn);
    emit(ctx, Oxor, Kw, *pr, src, getcon(ctx, 1, fn));
    *pr = src;
}

// fixarg: operand fixup.

// Fix constant operand.
void fix_con_arg(Ctx* ctx, Ref* r, int k, Ins* i, int op, Fn* fn) {
    Con* c = &fn->con[r->val];

    // CAddr as mem operand: no materialization needed.
    if (c->type == Con::CAddr && is_mem_arg(r, op, i))
        return;

    // Integer immediate within I-form range.
    if (KBASE(k) == 0 && c->type == Con::CBits && is_imm_arg(r, op, i)) {
        if (-2048 <= c->bits.i && c->bits.i < 2048)
            return;
    }

    // Integer store value of 0: emit `s* zero, ...` without materialization.
    if (i && c->type == Con::CBits && c->bits.i == 0 && r == &i->arg[0] &&
        INRANGE(op, Ostoreb, Ostorel)) {
        return;
    }

    Ref dst = newtmp(ctx, "isel", k, fn);

    if (KBASE(k) == 1) {
        // Floating-point constants must be stashed to ro data section.
        assert(c->type == Con::CBits);
        int slot_id = stashbits(ctx, static_cast<bits>(c->bits.i), KWIDE(k) ? 8 : 4);

        vgrow(ctx, &fn->con, static_cast<ulong>(++fn->ncon));
        Con* nc = &fn->con[fn->ncon - 1];
        *nc = {};
        nc->type = Con::CAddr;

        char buf[32];
        snprintf(buf, sizeof(buf), "\"%sfp%d\"", T.asloc, slot_id);
        nc->sym.id = intern(buf);

        emit(ctx, Oload, k, dst, Ref::con(static_cast<uint>(nc - fn->con)), Ref::none());
    } else {
        // Integer constants out of imm range use Ocopy.
        emit(ctx, Ocopy, k, dst, *r, Ref::none());
    }

    *r = dst;
}

// Operations that do not require sext.w when Kw is input.
bool skip_sext_for_kw(int op) {
    switch (op) {
        // *w-form arithmetic.
        case Oadd:
        case Osub:
        case Omul:
        case Odiv:
        case Orem:
        case Oudiv:
        case Ourem:
        case Osar:
        case Oshr:
        case Oshl:
        // Narrow stores.
        case Ostoreb:
        case Ostoreh:
        case Ostorew:
        // Extensions.
        case Oextsb:
        case Oextub:
        case Oextsh:
        case Oextuh:
        case Oextsw:
        case Oextuw:
        // int->float conversions.
        case Oswtof:
        case Ouwtof:
            return true;
        default:
            return false;
    }
}

// Fix temporary operand.
void fix_tmp_arg(Ctx* ctx, Ref* r, int k, Ins* i, int op, Fn* fn) {
    if (isreg(*r))
        return;

    int s = fn->tmp[r->val].slot;
    if (s != -1) {
        // Tmp with slot allocated.
        if (is_mem_arg(r, op, i)) {
            *r = Ref::slot(s);
        } else {
            Ref dst = newtmp(ctx, "isel", k, fn);
            emit(ctx, Oaddr, k, dst, Ref::slot(s), Ref::none());
            *r = dst;
        }
        return;
    }

    // Tmp is Kl but used as Kw.
    if (k == Kw && fn->tmp[r->val].cls == Kl) {
        if (!skip_sext_for_kw(op)) {
            Ref dst = newtmp(ctx, "isel", k, fn);
            emit(ctx, Oextsw, Kl, dst, *r, Ref::none());
            *r = dst;
        }
        // Skip sext.w.
        return;
    }

    assert(k == fn->tmp[r->val].cls);
}

void fixarg(Ctx* ctx, Ref* r, int k, Ins* i, Fn* fn) {
    int op = i ? static_cast<int>(i->op) : Ocopy;
    switch (rtype(*r)) {
        case RCon:
            fix_con_arg(ctx, r, k, i, op, fn);
            break;
        case RTmp:
            fix_tmp_arg(ctx, r, k, i, op, fn);
            break;
        default:
            // RSlot/RType/RCall/RMem/RInt/RNone don't need fixing.
            break;
    }
}

// Fixarg both operands of the instruction.
void fixarg2(Ctx* ctx, int k, Fn* fn) {
    Ins* h = ctx->curi;
    fixarg(ctx, &h->arg[0], k, h, fn);
    fixarg(ctx, &h->arg[1], k, h, fn);
}

// Compare selection.

void selcmp(Ctx* ctx, Ins i, int k, int op, Fn* fn) {
    assert(op >= 0 && op < NCmp);

    // (1) Integer eq/ne.
    if (op == Cieq || op == Cine) {
        Ref r = newtmp(ctx, "isel", k, fn);
        emit(ctx, op == Cieq ? Oreqz : Ornez, static_cast<int>(i.cls), i.to, r, Ref::none());
        emit(ctx, Oxor, k, r, i.arg[0], i.arg[1]);
        fixarg2(ctx, k, fn);
        return;
    }

    // (2) Integer ordered comparisons.
    if (op < NCmpI) {
        const CmpCfg cfg = kIntOrderCmp[op - Cisge];
        i.op = static_cast<uint>(cfg.sign ? Ocsltl : Ocultl);
        if (cfg.swap) {
            Ref t = i.arg[0];
            i.arg[0] = i.arg[1];
            i.arg[1] = t;
        }
        if (cfg.neg)
            negate_to(ctx, &i.to, fn);
        emiti(ctx, i);
        fixarg2(ctx, k, fn);
        return;
    }

    // (3) Floating point comparisons.
    int fop = op - NCmpI;

    // Cfo (ordered) / Cfuo (unordered).
    if (fop == Cfo || fop == Cfuo) {
        if (fop == Cfuo)
            negate_to(ctx, &i.to, fn);
        Ref r0 = newtmp(ctx, "isel", static_cast<int>(i.cls), fn);
        Ref r1 = newtmp(ctx, "isel", static_cast<int>(i.cls), fn);
        int ceq_op = KWIDE(k) ? Oceqd : Oceqs;
        emit(ctx, Oand, static_cast<int>(i.cls), i.to, r0, r1);
        emit(ctx, ceq_op, static_cast<int>(i.cls), r0, i.arg[0], i.arg[0]);
        fixarg2(ctx, k, fn);
        emit(ctx, ceq_op, static_cast<int>(i.cls), r1, i.arg[1], i.arg[1]);
        fixarg2(ctx, k, fn);
        return;
    }

    // Cfne: a != b -> !(a == b).
    if (fop == Cfne) {
        i.op = static_cast<uint>(KWIDE(k) ? Oceqd : Oceqs);
        negate_to(ctx, &i.to, fn);
    }
    // Remaining Cfeq/Cfge/Cfgt/Cfle/Cflt.

    emiti(ctx, i);
    fixarg2(ctx, k, fn);
}

// Load/store constant offset folding.

Ref new_offset_mem(Ctx* ctx, Fn* fn, Ref base, int64_t off) {
    vensure(ctx, &fn->mem, static_cast<ulong>(fn->nmem + 1), sizeof(Mem), PFn);
    Mem* m = &fn->mem[fn->nmem];
    *m = {};
    m->offset.type = Con::CBits;
    m->offset.bits.i = off;
    m->base = base;
    m->index = Ref::none();
    m->scale = 1;
    return Ref::mem(static_cast<uint>(fn->nmem++));
}

// Reverse fold Oaddr -> CAddr.
[[maybe_unused]] void fold_addr_into_mem(Ctx* ctx, Ref* paddr, uint bid, Fn* fn) {
    if (rtype(*paddr) == RTmp) {
        Tmp* t = &fn->tmp[paddr->val];
        Ins* def = t->def;
        if (!def || static_cast<int>(def->op) != Oaddr)
            return;
        if (t->nuse != 1 || t->bid != bid)
            return;
        if (rtype(def->arg[0]) != RCon)
            return;
        Con* c = &fn->con[def->arg[0].val];
        if (c->type != Con::CAddr)
            return;
        *paddr = def->arg[0];
        def->op = Onop;
        return;
    }
    if (rtype(*paddr) == RMem) {
        Mem* m = &fn->mem[paddr->val];
        if (rtype(m->base) != RTmp)
            return;
        if (!req(m->index, Ref::none()))
            return;
        if (m->offset.type != Con::CBits)
            return;
        Tmp* t = &fn->tmp[m->base.val];
        Ins* def = t->def;
        if (!def || static_cast<int>(def->op) != Oaddr)
            return;
        if (t->nuse != 1 || t->bid != bid)
            return;
        if (rtype(def->arg[0]) != RCon)
            return;
        Con* c = &fn->con[def->arg[0].val];
        if (c->type != Con::CAddr)
            return;
        // Create new CAddr combining bits.
        vgrow(ctx, &fn->con, static_cast<ulong>(fn->ncon + 1));
        Con* nc = &fn->con[fn->ncon];
        *nc = *c;
        nc->bits.i = c->bits.i + m->offset.bits.i;
        uint cidx = static_cast<uint>(fn->ncon++);
        *paddr = Ref::con(cidx);
        def->op = Onop;
        return;
    }
}

void fold_mem_offset(Ctx* ctx, Ref* paddr, uint bid, Fn* fn) {
    if (rtype(*paddr) != RTmp)
        return;
    Tmp* t = &fn->tmp[paddr->val];
    Ins* def = t->def;
    if (!def || static_cast<int>(def->op) != Oadd || static_cast<int>(def->cls) != Kl)
        return;
    if (t->nuse != 1 || t->bid != bid)
        return;

    // Oadd is commutative.
    Ref base, koff;
    if (rtype(def->arg[0]) == RTmp && rtype(def->arg[1]) == RCon) {
        base = def->arg[0];
        koff = def->arg[1];
    } else if (rtype(def->arg[0]) == RCon && rtype(def->arg[1]) == RTmp) {
        base = def->arg[1];
        koff = def->arg[0];
    } else {
        return;
    }

    Con* c = &fn->con[koff.val];
    if (c->type != Con::CBits)
        return;
    int64_t off = c->bits.i;
    if (off < -2048 || off > 2047)
        return;

    // base must be a normal tmp (value is address).
    if (fn->tmp[base.val].slot != -1)
        return;

    *paddr = new_offset_mem(ctx, fn, base, off);
    def->op = Onop;
}

// Single instruction selection.

void sel(Ctx* ctx, Ins i, Blk* b, Fn* fn) {
    // Oalloc{4,8,16} non-constant sizes.
    if (INRANGE(i.op, Oalloc, Oalloc1)) {
        Ins* head = ctx->curi - 1;
        salloc(ctx, i.to, i.arg[0], fn);
        fixarg(ctx, &head->arg[0], Kl, head, fn);
        return;
    }

    int ck, cc;
    if (iscmp(static_cast<int>(i.op), &ck, &cc)) {
        selcmp(ctx, i, ck, cc, fn);
        return;
    }

    // Consume Onop.
    if (i.op == Onop)
        return;

    // Oaddr from IRGen.
    if (i.op == Oaddr) {
        emiti(ctx, i);
        return;
    }

    // Load/store constant offset folding.
    int op = static_cast<int>(i.op);
    Ref* paddr = nullptr;
    if (isload(op))
        paddr = &i.arg[0];
    else if (isstore(op))
        paddr = &i.arg[1];
    if (paddr) {
        fold_mem_offset(ctx, paddr, b->id, fn);
    }

    emiti(ctx, i);
    Ins* h = ctx->curi;  // capture head.
    fixarg(ctx, &h->arg[0], argcls(&i, 0), h, fn);
    fixarg(ctx, &h->arg[1], argcls(&i, 1), h, fn);
}

// Jump selection.

// cmp+jnz fusion.
inline short cmp_to_jjfi(int cc) {
    assert(cc >= 0 && cc < NCmpI);
    return static_cast<short>(Jjfieq + cc);
}

void seljmp(Ctx* ctx, Blk* b, Fn* fn) {
    if (b->jmp.type != Jjnz)
        return;

    if (rtype(b->jmp.arg) == RTmp) {
        Tmp* t = &fn->tmp[b->jmp.arg.val];
        Ins* def = t->def;
        if (t->nuse == 1 && def && t->bid == b->id) {
            int ck, cc;
            if (iscmp(static_cast<int>(def->op), &ck, &cc) && cc < NCmpI) {
                b->jmp.type = cmp_to_jjfi(cc);
                b->jmp.cls = static_cast<short>(ck);
                b->jmp.arg = def->arg[0];
                b->jmp.arg2 = def->arg[1];
                def->op = Onop;
                fixarg(ctx, &b->jmp.arg, ck, nullptr, fn);
                fixarg(ctx, &b->jmp.arg2, ck, nullptr, fn);
                return;
            }
        }
    }
    fixarg(ctx, &b->jmp.arg, Kw, nullptr, fn);
}

// Pass A: fast alloc -> slot.

void assign_fast_alloc_slots(Fn* fn) {
    Blk* b = fn->start;

    int align_bytes = 4;
    for (int op = Oalloc; op <= Oalloc1; ++op, align_bytes *= 2) {
        Ins* ie = b->ins + b->nins;
        for (Ins* i = b->ins; i < ie; ++i) {
            if (static_cast<int>(i->op) != op)
                continue;
            if (rtype(i->arg[0]) != RCon)
                continue;

            int64_t sz = fn->con[i->arg[0].val].bits.i;
            if (sz < 0 || sz >= INT_MAX - 15)
                err(const_cast<char*>("invalid alloc size %" PRId64), sz);

            sz = (sz + align_bytes - 1) & -align_bytes;
            sz /= 4;
            if (sz > INT_MAX - fn->slot)
                die("alloc too large");

            fn->tmp[i->to.val].slot = fn->slot;
            fn->slot += static_cast<int>(sz);

            *i = Ins{};
            i->op = Onop;
        }
    }
}

}  // namespace

// rv64_isel: Main entry point.

void rv64_isel(Ctx* ctx, Fn* fn) {
    assign_fast_alloc_slots(fn);

    for (Blk* b = fn->start; b; b = b->link) {
        ctx->curi = ctx->insb + NIns;

        // Phi nodes of successors.
        Blk* succ[3] = {b->s1, b->s2, nullptr};
        for (Blk** ps = succ; *ps; ++ps) {
            for (Phi* p = (*ps)->phi; p; p = p->link) {
                uint n = 0;
                while (p->blk[n] != b) {
                    ++n;
                    assert(n < p->narg);
                }
                fixarg(ctx, &p->arg[n], static_cast<int>(p->cls), nullptr, fn);
            }
        }

        seljmp(ctx, b, fn);

        // Reverse iteration for emit prepending.
        for (Ins* i = b->ins + b->nins; i != b->ins;) {
            --i;
            sel(ctx, *i, b, fn);
        }

        idup(ctx, b, ctx->curi, static_cast<ulong>(ctx->insb + NIns - ctx->curi));
    }

    if (debug['I']) {
        fprintf(ctx->dbg_buf, "\n> After instruction selection:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
