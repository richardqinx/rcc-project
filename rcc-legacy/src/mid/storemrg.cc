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

/// @file storemrg.cc
/// @brief Fuses adjacent 4B stores into 8B storel.
///
/// See docs/storemrg.md for architectural notes.

#include "rcc/all.h"

#include <vector>

namespace rcc {

namespace {

// Read 32-bit integer constant value from Ref r (put into *val_out).
bool get_int_con32(Fn* fn, Ref r, uint32_t* val_out) {
    if (rtype(r) != RCon)
        return false;
    Con* c = &fn->con[r.val];
    if (c->type != Con::CBits || c->flt)
        return false;
    *val_out = static_cast<uint32_t>(c->bits.i);
    return true;
}

bool ref_eq(Ref a, Ref b) {
    return a.type == b.type && a.val == b.val;
}

// Decompose addr into (base, offset).
void decompose_addr(Fn* fn, Ref addr, Ref* base_out, int64_t* off_out) {
    *base_out = addr;
    *off_out = 0;
    if (rtype(addr) != RTmp)
        return;
    Tmp* t = &fn->tmp[addr.val];
    if (!t->def)
        return;
    Ins* def = t->def;
    if (def->op != Oadd)
        return;
    Ref ra = def->arg[0], rb = def->arg[1];
    auto try_split = [&](Ref tmp_r, Ref con_r) -> bool {
        if (rtype(tmp_r) != RTmp || rtype(con_r) != RCon)
            return false;
        Con* c = &fn->con[con_r.val];
        if (c->type != Con::CBits || c->flt)
            return false;
        *base_out = tmp_r;
        *off_out = c->bits.i;
        return true;
    };
    if (try_split(ra, rb))
        return;
    try_split(rb, ra);
}

bool con_is_aligned8(Fn* fn, Ref r) {
    if (rtype(r) != RCon)
        return false;
    Con* c = &fn->con[r.val];
    return c->type == Con::CBits && !c->flt && (c->bits.i & 7) == 0;
}

bool tmp_in_align8(const std::vector<bool>& a8, Ref r) {
    if (rtype(r) != RTmp)
        return false;
    if (r.val >= a8.size())
        return false;
    return a8[r.val];
}

// Optimistic fixpoint: compute aligned8 set.
std::vector<bool> compute_aligned8(Fn* fn) {
    std::vector<bool> a8(static_cast<size_t>(fn->ntmp), true);
    bool changed = true;
    while (changed) {
        changed = false;
        for (Blk* b = fn->start; b; b = b->link) {
            for (Phi* p = b->phi; p; p = p->link) {
                if (rtype(p->to) != RTmp)
                    continue;
                uint tid = p->to.val;
                if (tid >= a8.size() || !a8[tid])
                    continue;
                bool ok = p->narg > 0;
                for (uint k = 0; k < p->narg; k++) {
                    Ref r = p->arg[k];
                    if (tmp_in_align8(a8, r))
                        continue;
                    if (con_is_aligned8(fn, r))
                        continue;
                    ok = false;
                    break;
                }
                if (!ok) {
                    a8[tid] = false;
                    changed = true;
                }
            }
            for (uint k = 0; k < b->nins; k++) {
                Ins* i = &b->ins[k];
                if (rtype(i->to) != RTmp)
                    continue;
                uint tid = i->to.val;
                if (tid >= a8.size() || !a8[tid])
                    continue;
                bool ok = false;
                int op = i->op;
                if (op == Oalloc8 || op == Oalloc16) {
                    ok = true;
                } else if (op == Ocopy) {
                    if (tmp_in_align8(a8, i->arg[0]))
                        ok = true;
                } else if (op == Oadd) {
                    if ((tmp_in_align8(a8, i->arg[0]) && con_is_aligned8(fn, i->arg[1])) ||
                        (tmp_in_align8(a8, i->arg[1]) && con_is_aligned8(fn, i->arg[0])))
                        ok = true;
                }
                // All other ops (Omul / Oload / Opar / Ocall / ...) are unconditionally demoted.
                if (!ok) {
                    a8[tid] = false;
                    changed = true;
                }
            }
        }
    }
    return a8;
}

}  // namespace

void storemrg(Ctx* ctx, Fn* fn) {
    std::vector<bool> a8 = compute_aligned8(fn);

    int merged = 0;
    for (Blk* b = fn->start; b; b = b->link) {
        struct Pending {
            uint idx;
            Ref base;
            int64_t off;
            uint32_t val;  // 32-bit store value
        };
        std::vector<Pending> pending;
        auto flush = [&]() { pending.clear(); };

        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            int op = i->op;

            // Memory barrier: any load / call / other store flushes pending.
            if (op == Ocall || isload(op)) {
                flush();
                continue;
            }

            uint32_t val32 = 0;
            if (op == Ostorew && get_int_con32(fn, i->arg[0], &val32)) {
                Ref base;
                int64_t off;
                decompose_addr(fn, i->arg[1], &base, &off);
                if (tmp_in_align8(a8, base)) {
                    bool matched = false;
                    for (auto it = pending.begin(); it != pending.end(); ++it) {
                        bool prev_is_lower = (it->off + 4 == off) && (it->off & 7) == 0;
                        bool curr_is_lower = (off + 4 == it->off) && (off & 7) == 0;
                        // Must be same base + same 32-bit value + lower offset 8B-aligned pair.
                        if (ref_eq(it->base, base) && it->val == val32 &&
                            (prev_is_lower || curr_is_lower)) {
                            uint lo_idx = prev_is_lower ? it->idx : k;
                            uint hi_idx = prev_is_lower ? k : it->idx;
                            b->ins[lo_idx].op = Ostorel;
                            if (val32 != 0) {
                                // Assemble 64-bit duplicated constant.
                                uint64_t combined = static_cast<uint64_t>(val32) |
                                                    (static_cast<uint64_t>(val32) << 32);
                                b->ins[lo_idx].arg[0] =
                                    getcon(ctx, static_cast<int64_t>(combined), fn);
                            }
                            // When val32==0, RCon{0} has identical bit pattern for Kw/Kl.
                            b->ins[hi_idx] = Ins{};
                            b->ins[hi_idx].op = Onop;
                            pending.erase(it);
                            merged++;
                            matched = true;
                            break;
                        }
                    }
                    if (!matched)
                        pending.push_back({k, base, off, val32});
                    continue;
                }
                // base not in align8 set -> cannot prove no-alias with pending, flush.
                flush();
                continue;
            }

            if (isstore(op)) {
                flush();
                continue;
            }

            // Pure instructions have no memory side-effects, pending untouched.
        }
    }

    if (debug['M']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "\n> storemrg %s: merged %d storew pairs → storel\n", fn->name, merged);
    }
}

}  // namespace rcc
