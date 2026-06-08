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

/// @file fold.cc
/// @brief Constant folding utilities -- called by GVN (dedupins / assoccon), no standalone driver.
///
/// See docs/fold.md for architectural notes.

#include "rcc/all.h"

#include <cstdint>
#include <cstring>

namespace rcc {

namespace {

inline bool iscon(const Con* c, int w, uint64_t k) {
    if (c->type != Con::CBits)
        return false;
    int64_t v = w ? c->bits.i : static_cast<int32_t>(c->bits.i);
    return static_cast<uint64_t>(v) == (w ? k : static_cast<uint32_t>(k));
}

union Bits64 {
    int64_t s;
    uint64_t u;
    float fs;
    double fd;
};

inline Bits64 read_bits(const Con* c) {
    Bits64 b;
    b.s = c->bits.i;
    return b;
}

int addr_combine(int op, const Con* cl, const Con* cr, int* typ_out, Sym* sym_out) {
    int ctyp = Con::CBits;
    Sym sym;
    std::memset(&sym, 0, sizeof sym);

    bool al = (cl->type == Con::CAddr);
    bool ar = (cr->type == Con::CAddr);

    if (op == Oadd) {
        if (al && ar)
            return 1;
        if (al || ar) {
            ctyp = Con::CAddr;
            sym = al ? cl->sym : cr->sym;
        }
    } else if (op == Osub) {
        if (al && ar) {
            if (!symeq(cl->sym, cr->sym))
                return 1;
            // Subtract same sym.
        } else if (al) {
            ctyp = Con::CAddr;
            sym = cl->sym;
        } else if (ar) {
            return 1;  // Number - address is invalid
        }
    } else if (op == Ocast) {
        if (al) {
            ctyp = Con::CAddr;
            sym = cl->sym;
        }
    } else if (al || ar) {
        return 1;
    }
    *typ_out = ctyp;
    *sym_out = sym;
    return 0;
}

uint64_t foldcmp_int(int cmp, int w, Bits64 l, Bits64 r) {
    if (!w) {
        l.u = static_cast<uint64_t>(static_cast<int32_t>(l.u));
        r.u = static_cast<uint64_t>(static_cast<int32_t>(r.u));
    }
    switch (cmp) {
        case Cieq:
            return l.u == r.u;
        case Cine:
            return l.u != r.u;
        case Ciule:
            return l.u <= r.u;
        case Ciult:
            return l.u < r.u;
        case Ciuge:
            return l.u >= r.u;
        case Ciugt:
            return l.u > r.u;
        case Cisle:
            return l.s <= r.s;
        case Cislt:
            return l.s < r.s;
        case Cisge:
            return l.s >= r.s;
        case Cisgt:
            return l.s > r.s;
        default:
            die("foldcmp_int: bad cmp %d", cmp);
    }
}

template <typename F>
uint64_t foldcmp_flt(int cmp, F l, F r) {
    switch (cmp) {
        case Cfeq:
            return l == r;
        case Cfne:
            return l != r;
        case Cfle:
            return l <= r;
        case Cflt:
            return l < r;
        case Cfge:
            return l >= r;
        case Cfgt:
            return l > r;
        case Cfo:
            return (l < r) || (l >= r);
        case Cfuo:
            return !((l < r) || (l >= r));
        default:
            die("foldcmp_flt: bad cmp %d", cmp);
    }
}

void foldflt(Con* res, int op, int w, const Con* cl, const Con* cr) {
    if (cl->type != Con::CBits || cr->type != Con::CBits)
        err(const_cast<char*>("invalid address operand for '%s'"), optab[op].name);

    res->type = Con::CBits;
    res->sym = Sym{};
    std::memset(&res->bits, 0, sizeof res->bits);
    res->flt = 0;

    if (w) {
        double ld = cl->bits.d, rd = cr->bits.d, xd;
        switch (op) {
            case Oadd:
                xd = ld + rd;
                break;
            case Osub:
                xd = ld - rd;
                break;
            case Oneg:
                xd = -ld;
                break;
            case Odiv:
                xd = ld / rd;
                break;
            case Omul:
                xd = ld * rd;
                break;
            case Oswtof:
                xd = static_cast<double>(static_cast<int32_t>(cl->bits.i));
                break;
            case Ouwtof:
                xd = static_cast<double>(static_cast<uint32_t>(cl->bits.i));
                break;
            case Osltof:
                xd = static_cast<double>(static_cast<int64_t>(cl->bits.i));
                break;
            case Oultof:
                xd = static_cast<double>(static_cast<uint64_t>(cl->bits.i));
                break;
            case Oexts:
                xd = static_cast<double>(cl->bits.s);
                break;
            case Ocast:
                xd = ld;
                break;
            default:
                die("foldflt: bad op Kd %d", op);
        }
        res->bits.d = xd;
        res->flt = 2;
    } else {
        float ls = cl->bits.s, rs = cr->bits.s, xs;
        switch (op) {
            case Oadd:
                xs = ls + rs;
                break;
            case Osub:
                xs = ls - rs;
                break;
            case Oneg:
                xs = -ls;
                break;
            case Odiv:
                xs = ls / rs;
                break;
            case Omul:
                xs = ls * rs;
                break;
            case Oswtof:
                xs = static_cast<float>(static_cast<int32_t>(cl->bits.i));
                break;
            case Ouwtof:
                xs = static_cast<float>(static_cast<uint32_t>(cl->bits.i));
                break;
            case Osltof:
                xs = static_cast<float>(static_cast<int64_t>(cl->bits.i));
                break;
            case Oultof:
                xs = static_cast<float>(static_cast<uint64_t>(cl->bits.i));
                break;
            case Otruncd:
                xs = static_cast<float>(cl->bits.d);
                break;
            case Ocast:
                xs = ls;
                break;
            default:
                die("foldflt: bad op Ks %d", op);
        }
        res->bits.s = xs;
        res->flt = 1;
    }
}

}  // namespace

int foldint(Con* res, int op, int w, Con* cl, Con* cr) {
    // Address combination check.
    int ctyp;
    Sym sym;
    if (addr_combine(op, cl, cr, &ctyp, &sym))
        return 1;

    Bits64 l = read_bits(cl);
    Bits64 r = read_bits(cr);

    // Div / rem.
    if (op == Odiv || op == Orem || op == Oudiv || op == Ourem) {
        if (iscon(cr, w, 0))
            return 1;
        if (op == Odiv || op == Orem) {
            uint64_t intmin = w ? static_cast<uint64_t>(INT64_MIN)
                                : static_cast<uint64_t>(static_cast<uint32_t>(INT32_MIN));
            if (iscon(cr, w, static_cast<uint64_t>(-1)) && iscon(cl, w, intmin))
                return 1;
        }
    }

    // Dispatch.
    uint64_t x;

    if (op >= Ocmpw && op <= Ocmpw1) {
        x = foldcmp_int(op - Ocmpw, /*w=*/0, l, r);
    } else if (op >= Ocmpl && op <= Ocmpl1) {
        x = foldcmp_int(op - Ocmpl, /*w=*/1, l, r);
    } else if (op >= Ocmps && op <= Ocmps1) {
        x = foldcmp_flt<float>(op - Ocmps, cl->bits.s, cr->bits.s);
    } else if (op >= Ocmpd && op <= Ocmpd1) {
        x = foldcmp_flt<double>(op - Ocmpd, cl->bits.d, cr->bits.d);
    } else {
        // Shift count mask.
        const uint64_t shmask = static_cast<uint64_t>(31 | (w << 5));
        switch (op) {
            case Oadd:
                x = l.u + r.u;
                break;
            case Osub:
                x = l.u - r.u;
                break;
            case Oneg:
                x = -l.u;
                break;
            case Omul:
                x = l.u * r.u;
                break;
            case Odiv:
                x = w ? static_cast<uint64_t>(l.s / r.s)
                      : static_cast<uint64_t>(static_cast<int32_t>(l.s) /
                                              static_cast<int32_t>(r.s));
                break;
            case Orem:
                x = w ? static_cast<uint64_t>(l.s % r.s)
                      : static_cast<uint64_t>(static_cast<int32_t>(l.s) %
                                              static_cast<int32_t>(r.s));
                break;
            case Oudiv:
                x = w ? l.u / r.u : static_cast<uint32_t>(l.u) / static_cast<uint32_t>(r.u);
                break;
            case Ourem:
                x = w ? l.u % r.u : static_cast<uint32_t>(l.u) % static_cast<uint32_t>(r.u);
                break;
            case Oand:
                x = l.u & r.u;
                break;
            case Oor:
                x = l.u | r.u;
                break;
            case Oxor:
                x = l.u ^ r.u;
                break;
            // Signed right shift.
            case Osar:
                x = static_cast<uint64_t>((w ? l.s : static_cast<int32_t>(l.s)) >> (r.u & shmask));
                break;
            case Oshr:
                x = (w ? l.u : static_cast<uint32_t>(l.u)) >> (r.u & shmask);
                break;
            case Oshl:
                x = l.u << (r.u & shmask);
                break;

            case Oextsb:
                x = static_cast<uint64_t>(static_cast<int8_t>(l.u));
                break;
            case Oextub:
                x = static_cast<uint8_t>(l.u);
                break;
            case Oextsh:
                x = static_cast<uint64_t>(static_cast<int16_t>(l.u));
                break;
            case Oextuh:
                x = static_cast<uint16_t>(l.u);
                break;
            case Oextsw:
                x = static_cast<uint64_t>(static_cast<int32_t>(l.u));
                break;
            case Oextuw:
                x = static_cast<uint32_t>(l.u);
                break;

            case Ostosi:
                x = w ? static_cast<uint64_t>(static_cast<int64_t>(cl->bits.s))
                      : static_cast<uint64_t>(static_cast<int32_t>(cl->bits.s));
                break;
            case Ostoui:
                x = w ? static_cast<uint64_t>(cl->bits.s)
                      : static_cast<uint64_t>(static_cast<uint32_t>(cl->bits.s));
                break;
            case Odtosi:
                x = w ? static_cast<uint64_t>(static_cast<int64_t>(cl->bits.d))
                      : static_cast<uint64_t>(static_cast<int32_t>(cl->bits.d));
                break;
            case Odtoui:
                x = w ? static_cast<uint64_t>(cl->bits.d)
                      : static_cast<uint64_t>(static_cast<uint32_t>(cl->bits.d));
                break;
            case Ocast:
                x = l.u;
                break;
            default:
                die("foldint: unsupported op %d", op);
        }
    }

    // Write back.
    res->type = static_cast<decltype(res->type)>(ctyp);
    res->sym = sym;
    std::memset(&res->bits, 0, sizeof res->bits);
    res->bits.i = static_cast<int64_t>(x);
    res->flt = 0;
    return 0;
}

namespace {

Ref opfold(Ctx* ctx, int op, int cls, Con* cl, Con* cr, Fn* fn) {
    Con c;
    std::memset(&c, 0, sizeof c);

    if (cls == Kw || cls == Kl) {
        if (foldint(&c, op, cls == Kl, cl, cr))
            return Ref::none();
    } else {
        foldflt(&c, op, cls == Kd, cl, cr);
    }

    if (!KWIDE(cls))
        c.bits.i &= 0xffffffff;

    Ref r = newcon(ctx, &c, fn);
    assert((cls != Ks && cls != Kd) || c.flt);
    return r;
}

}  // namespace

Ref foldref(Ctx* ctx, Fn* fn, Ins* i) {
    if (rtype(i->to) != RTmp)
        return Ref::none();
    if (!optab[i->op].canfold)
        return Ref::none();
    if (rtype(i->arg[0]) != RCon)
        return Ref::none();

    Ref rr = i->arg[1];
    if (req(rr, Ref::none()))
        rr = Ref::con_z();
    if (rtype(rr) != RCon)
        return Ref::none();

    Con* cl = &fn->con[i->arg[0].val];
    Con* cr = &fn->con[rr.val];
    return opfold(ctx, i->op, i->cls, cl, cr, fn);
}

}  // namespace rcc
