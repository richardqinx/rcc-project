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

/// @file backend/simpl.cc
/// @brief Frontend simplification pass.

#include "rcc/all.h"

#include <cinttypes>
#include <climits>
#include <cstdlib>

namespace rcc {

// Memory block copy expansion: Oblit pair -> explicit load/store chain.
namespace {

struct BlitTier {
    int store_op;
    int load_op;
    int load_cls;
    int width;
};

constexpr BlitTier kBlitTiers[] = {
    {Ostorel, Oload, Kl, 8},
    {Ostorew, Oload, Kw, 4},
    {Ostoreh, Oloaduh, Kw, 2},
    {Ostoreb, Oloadub, Kw, 1},
};

void blit(Ctx* ctx, Ref sd[2], int signed_sz, Fn* fn) {
    bool fwd = signed_sz >= 0;
    int sz = std::abs(signed_sz);
    int off = fwd ? sz : 0;

    for (const BlitTier& t : kBlitTiers) {
        while (sz >= t.width) {
            if (fwd)
                off -= t.width;

            Ref val = newtmp(ctx, "blt", Kl, fn);
            Ref dst_addr = newtmp(ctx, "blt", Kl, fn);
            Ref off_con = getcon(ctx, off, fn);

            // Reverse emit: store <- dst_addr <- load <- src_addr.
            emit(ctx, t.store_op, 0, Ref::none(), val, dst_addr);
            emit(ctx, Oadd, Kl, dst_addr, sd[1], off_con);

            Ref src_addr = newtmp(ctx, "blt", Kl, fn);
            emit(ctx, t.load_op, t.load_cls, val, src_addr, Ref::none());
            emit(ctx, Oadd, Kl, src_addr, sd[0], off_con);

            if (!fwd)
                off += t.width;
            sz -= t.width;
        }
    }
}

inline bool is_pow2(uint64_t v) {
    return v != 0 && (v & (v - 1)) == 0;
}

// Single instruction simplification: Oblit1 / Oudiv / Ourem.
void simplify_one(Ctx* ctx, Ins** pi, bool* rebuilt, Blk* b, Fn* fn) {
    Ins* i = *pi;

    switch (i->op) {
        case Oblit1: {
            assert(i > b->ins);
            assert((i - 1)->op == Oblit0);

            // First rebuild trigger: move unprocessed tail to insb.
            if (!*rebuilt) {
                ctx->curi = ctx->insb + NIns;
                ulong tail = static_cast<ulong>((b->ins + b->nins) - (i + 1));
                ctx->curi -= tail;
                icpy(ctx->curi, i + 1, tail);
                *rebuilt = true;
            }

            blit(ctx, (i - 1)->arg, rsval(i->arg[0]), fn);
            *pi = i - 1;  // Retreat pointer to Oblit0.
            return;
        }

        case Oudiv:
        case Ourem: {
            // Unsigned integer + constant divisor + pow2.
            if (KBASE(i->cls) != 0)
                break;
            if (rtype(i->arg[1]) != RCon)
                break;
            Con* c = &fn->con[i->arg[1].val];
            if (c->type != Con::CBits)
                break;
            if (!is_pow2(static_cast<uint64_t>(c->bits.i)))
                break;

            int n = lowest_bit(static_cast<bits>(c->bits.i));
            if (i->op == Ourem) {
                i->op = Oand;
                i->arg[1] = getcon(ctx, (1LL << n) - 1, fn);
            } else {
                i->op = Oshr;
                i->arg[1] = getcon(ctx, n, fn);
            }
            break;
        }

        default:
            break;
    }

    if (*rebuilt)
        emiti(ctx, *i);
}

}  // namespace

void simpl(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        bool rebuilt = false;
        for (Ins* i = b->ins + b->nins; i != b->ins;) {
            --i;
            simplify_one(ctx, &i, &rebuilt, b, fn);
        }
        if (rebuilt)
            idup(ctx, b, ctx->curi, static_cast<ulong>(ctx->insb + NIns - ctx->curi));
    }
}

// salloc: Dynamic alloca expanded on stack. Called by rv64_isel.

void salloc(Ctx* ctx, Ref rt, Ref rs, Fn* fn) {
    fn->dynalloc = 1;

    if (rtype(rs) == RCon) {
        int64_t sz = fn->con[rs.val].bits.i;
        if (sz < 0 || sz >= INT_MAX - 15)
            err(const_cast<char*>("invalid alloc size %" PRId64), sz);
        sz = (sz + 15) & -16;
        emit(ctx, Osalloc, Kl, rt, getcon(ctx, sz, fn), Ref::none());
        return;
    }

    // Non-constant size path.
    if (rtype(rs) == RTmp && fn->tmp[rs.val].slot != -1)
        err(const_cast<char*>("unlikely alloc argument %%%s for %%%s"), fn->tmp[rs.val].name,
            fn->tmp[rt.val].name);

    Ref r0 = newtmp(ctx, "isel", Kl, fn);
    Ref r1 = newtmp(ctx, "isel", Kl, fn);
    emit(ctx, Osalloc, Kl, rt, r0, Ref::none());
    emit(ctx, Oand, Kl, r0, r1, getcon(ctx, -16, fn));
    emit(ctx, Oadd, Kl, r1, rs, getcon(ctx, 15, fn));
}

}  // namespace rcc
