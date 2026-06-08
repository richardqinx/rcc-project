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

/// @file mulopt.cc
/// @brief Constant integer multiplication strength reduction.
///
/// See docs/mulopt.md for architectural notes.

#include "rcc/all.h"

#include <cstdint>

namespace rcc {

namespace {

enum Kind {
    KZero,
    KCopy,
    KShl,
    KShlAddShl,
    KShlSubShl,
};

struct Rewrite {
    Kind kind;
    int sh1;  // Primary shift for KShl / KShlAddShl / KShlSubShl
    int sh2;  // Secondary shift for KShlAddShl / KShlSubShl (0 means use x itself)
};

// Classify patterns.
bool classify(uint64_t ac, int max_shift, Rewrite& out) {
    if (ac == 0) {
        out.kind = KZero;
        return true;
    }
    if (ac == 1) {
        out.kind = KCopy;
        return true;
    }
    if ((ac & (ac - 1)) == 0) {
        int k = __builtin_ctzll(ac);
        if (k >= max_shift)
            return false;
        out.kind = KShl;
        out.sh1 = k;
        return true;
    }
    int a = 63 - __builtin_clzll(ac);  // floor log2(ac), 0 < ac, ac is not pow2
    if (a >= max_shift)
        return false;
    uint64_t hi = static_cast<uint64_t>(1) << a;

    // pow2 + pow2.
    uint64_t low = ac - hi;
    if ((low & (low - 1)) == 0) {
        int b = __builtin_ctzll(low);
        out.kind = KShlAddShl;
        out.sh1 = a;
        out.sh2 = b;
        return true;
    }

    // pow2 - pow2.
    if (a + 1 < max_shift) {
        uint64_t hi2 = hi << 1;
        uint64_t diff = hi2 - ac;
        // Check diff.
        if ((diff & (diff - 1)) == 0) {
            int b = __builtin_ctzll(diff);
            out.kind = KShlSubShl;
            out.sh1 = a + 1;
            out.sh2 = b;
            return true;
        }
    }
    return false;
}

// emit() prepends.
bool lower_mul(Ctx* ctx, Fn* fn, Ins* i, Blk* b, bool* rebuilt) {
    if (i->op != Omul)
        return false;
    if (i->cls != Kw && i->cls != Kl)
        return false;
    if (rtype(i->arg[1]) != RCon)
        return false;
    Con* c = &fn->con[i->arg[1].val];
    if (c->type != Con::CBits)
        return false;

    int cls = i->cls;
    int64_t cv;
    int max_shift;
    if (cls == Kw) {
        cv = static_cast<int32_t>(
            c->bits.i);  // Narrow to i32 to avoid interference from Kl residual bits
        max_shift = 32;
    } else {
        cv = c->bits.i;
        max_shift = 64;
    }

    bool neg = (cv < 0);
    uint64_t ac = static_cast<uint64_t>(cv);
    if (neg)
        ac = -ac;  // wrap-around produces 2^31 / 2^63 for INT_MIN / INT64_MIN respectively

    Rewrite r;
    if (!classify(ac, max_shift, r))
        return false;

    Ref dst = i->to;
    Ref x = i->arg[0];

    // Move unprocessed tail.
    if (!*rebuilt) {
        ctx->curi = ctx->insb + NIns;
        ulong tail = static_cast<ulong>((b->ins + b->nins) - (i + 1));
        ctx->curi -= tail;
        icpy(ctx->curi, i + 1, tail);
        *rebuilt = true;
    }

    switch (r.kind) {
        case KZero:
            // 0 * x = 0.
            emit(ctx, Ocopy, cls, dst, getcon(ctx, 0, fn), Ref::none());
            break;
        case KCopy:
            if (neg)
                emit(ctx, Oneg, cls, dst, x, Ref::none());
            else
                emit(ctx, Ocopy, cls, dst, x, Ref::none());
            break;
        case KShl: {
            if (neg) {
                Ref t = newtmp(ctx, "mulopt", cls, fn);
                emit(ctx, Oneg, cls, dst, t, Ref::none());
                emit(ctx, Oshl, cls, t, x, getcon(ctx, r.sh1, fn));
            } else {
                emit(ctx, Oshl, cls, dst, x, getcon(ctx, r.sh1, fn));
            }
            break;
        }
        case KShlAddShl: {
            // sum.
            Ref sum = neg ? newtmp(ctx, "mulopt", cls, fn) : dst;
            Ref t1 = newtmp(ctx, "mulopt", cls, fn);
            Ref t2 = (r.sh2 == 0) ? x : newtmp(ctx, "mulopt", cls, fn);
            if (neg)
                emit(ctx, Oneg, cls, dst, sum, Ref::none());
            emit(ctx, Oadd, cls, sum, t1, t2);
            if (r.sh2 != 0)
                emit(ctx, Oshl, cls, t2, x, getcon(ctx, r.sh2, fn));
            emit(ctx, Oshl, cls, t1, x, getcon(ctx, r.sh1, fn));
            break;
        }
        case KShlSubShl: {
            Ref diff = neg ? newtmp(ctx, "mulopt", cls, fn) : dst;
            Ref t1 = newtmp(ctx, "mulopt", cls, fn);
            Ref t2 = (r.sh2 == 0) ? x : newtmp(ctx, "mulopt", cls, fn);
            if (neg)
                emit(ctx, Oneg, cls, dst, diff, Ref::none());
            emit(ctx, Osub, cls, diff, t1, t2);
            if (r.sh2 != 0)
                emit(ctx, Oshl, cls, t2, x, getcon(ctx, r.sh2, fn));
            emit(ctx, Oshl, cls, t1, x, getcon(ctx, r.sh1, fn));
            break;
        }
    }
    return true;
}

}  // namespace

void mulopt(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        bool rebuilt = false;
        for (Ins* i = b->ins + b->nins; i != b->ins;) {
            --i;
            if (lower_mul(ctx, fn, i, b, &rebuilt))
                continue;
            if (rebuilt)
                emiti(ctx, *i);
        }
        if (rebuilt)
            idup(ctx, b, ctx->curi, static_cast<ulong>(ctx->insb + NIns - ctx->curi));
    }
}

}  // namespace rcc
