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

/// @file divopt.cc
/// @brief Constant integer division/modulo strength reduction: rewrite `x / c`, `x % c`
///        into mul + shift + sign correction sequence when c is a compile-time constant.
///
/// See docs/divopt.md for architectural notes.

#include "rcc/all.h"

#include <cstdint>

namespace rcc {

namespace {

struct Magic {
    int32_t M;
    int s;
    bool need_add;
};

Magic compute_magic_s32(uint32_t ad) {
    const uint32_t t = 0x80000000u;  // 2^31
    uint32_t anc = t - 1 - t % ad;
    uint32_t p = 31;
    uint32_t q1 = t / anc, r1 = t - q1 * anc;
    uint32_t q2 = t / ad, r2 = t - q2 * ad;
    uint32_t delta;
    do {
        ++p;
        q1 *= 2;
        r1 *= 2;
        if (r1 >= anc) {
            ++q1;
            r1 -= anc;
        }
        q2 *= 2;
        r2 *= 2;
        if (r2 >= ad) {
            ++q2;
            r2 -= ad;
        }
        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));

    Magic m;
    uint32_t M_u = q2 + 1;
    m.M = static_cast<int32_t>(M_u);
    m.s = static_cast<int>(p - 32);
    m.need_add = (M_u >= 0x80000000u);
    return m;
}

void emit_pow2_div_into(Ctx* ctx, Fn* fn, Ref dst, Ref x, int k) {
    Ref sign = newtmp(ctx, "divopt", Kw, fn);
    Ref mask = newtmp(ctx, "divopt", Kw, fn);
    Ref tmp = newtmp(ctx, "divopt", Kw, fn);

    emit(ctx, Osar, Kw, dst, tmp, getcon(ctx, k, fn));
    emit(ctx, Oadd, Kw, tmp, x, mask);
    emit(ctx, Oshr, Kw, mask, sign, getcon(ctx, 32 - k, fn));
    emit(ctx, Osar, Kw, sign, x, getcon(ctx, 31, fn));
}

void emit_magic_div_into(Ctx* ctx, Fn* fn, Ref dst, Ref x, int32_t d_abs, bool d_neg) {
    Magic m = compute_magic_s32(static_cast<uint32_t>(d_abs));
    int64_t M64 = static_cast<int64_t>(m.M);  // Sign extend i32 to i64

    Ref xl = newtmp(ctx, "divopt", Kl, fn);
    Ref prod = newtmp(ctx, "divopt", Kl, fn);
    Ref hi = newtmp(ctx, "divopt", Kl, fn);
    Ref hi2 = m.need_add ? newtmp(ctx, "divopt", Kl, fn) : hi;
    Ref q1 = (m.s > 0) ? newtmp(ctx, "divopt", Kl, fn) : hi2;
    Ref sg = newtmp(ctx, "divopt", Kl, fn);
    Ref qpre = d_neg ? newtmp(ctx, "divopt", Kw, fn) : dst;

    // Reverse emit:
    if (d_neg)
        emit(ctx, Oneg, Kw, dst, qpre, Ref::none());
    emit(ctx, Osub, Kw, qpre, q1, sg);
    emit(ctx, Osar, Kl, sg, xl, getcon(ctx, 63, fn));
    if (m.s > 0)
        emit(ctx, Osar, Kl, q1, hi2, getcon(ctx, m.s, fn));
    if (m.need_add)
        emit(ctx, Oadd, Kl, hi2, hi, xl);
    emit(ctx, Osar, Kl, hi, prod, getcon(ctx, 32, fn));
    emit(ctx, Omul, Kl, prod, xl, getcon(ctx, M64, fn));
    emit(ctx, Oextsw, Kl, xl, x, Ref::none());
}

bool lower_div(Ctx* ctx, Fn* fn, Ins* i, Blk* b, bool* rebuilt) {
    if (i->op != Odiv && i->op != Orem)
        return false;
    if (i->cls != Kw)
        return false;
    if (rtype(i->arg[1]) != RCon)
        return false;
    Con* c = &fn->con[i->arg[1].val];
    if (c->type != Con::CBits)
        return false;
    int32_t d = static_cast<int32_t>(c->bits.i);
    if (d == 0 || d == static_cast<int32_t>(0x80000000))
        return false;  // Skip d=0 (leave to hardware) and d=INT_MIN (|d| overflows i32)

    bool is_rem = (i->op == Orem);
    Ref dst = i->to;
    Ref x = i->arg[0];

    // First hit: move unprocessed tail to end of insb (same as simpl.cc)
    if (!*rebuilt) {
        ctx->curi = ctx->insb + NIns;
        ulong tail = static_cast<ulong>((b->ins + b->nins) - (i + 1));
        ctx->curi -= tail;
        icpy(ctx->curi, i + 1, tail);
        *rebuilt = true;
    }

    // d = +-1 degenerate: rem -> 0; div(1)=x; div(-1)=-x.
    if (d == 1 || d == -1) {
        if (is_rem)
            emit(ctx, Ocopy, Kw, dst, getcon(ctx, 0, fn), Ref::none());
        else if (d == 1)
            emit(ctx, Ocopy, Kw, dst, x, Ref::none());
        else
            emit(ctx, Oneg, Kw, dst, x, Ref::none());
        return true;
    }

    // For rem: emit tail r = x - q*d first (reverse order), then div sequence.
    Ref q_dst = is_rem ? newtmp(ctx, "divopt", Kw, fn) : dst;
    if (is_rem) {
        Ref qd = newtmp(ctx, "divopt", Kw, fn);
        emit(ctx, Osub, Kw, dst, x, qd);
        emit(ctx, Omul, Kw, qd, q_dst, getcon(ctx, static_cast<int64_t>(d), fn));
    }

    int32_t ad = (d < 0) ? -d : d;
    if ((ad & (ad - 1)) == 0) {
        // |d| is power of 2, k >= 1
        int k = __builtin_ctz(static_cast<uint32_t>(ad));
        if (d < 0) {
            Ref q_pos = newtmp(ctx, "divopt", Kw, fn);
            emit(ctx, Oneg, Kw, q_dst, q_pos, Ref::none());
            emit_pow2_div_into(ctx, fn, q_pos, x, k);
        } else {
            emit_pow2_div_into(ctx, fn, q_dst, x, k);
        }
    } else {
        emit_magic_div_into(ctx, fn, q_dst, x, ad, d < 0);
    }
    return true;
}

}  // namespace

void divopt(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        bool rebuilt = false;
        for (Ins* i = b->ins + b->nins; i != b->ins;) {
            --i;
            if (lower_div(ctx, fn, i, b, &rebuilt))
                continue;
            if (rebuilt)
                emiti(ctx, *i);
        }
        if (rebuilt)
            idup(ctx, b, ctx->curi, static_cast<ulong>(ctx->insb + NIns - ctx->curi));
    }
}

}  // namespace rcc
