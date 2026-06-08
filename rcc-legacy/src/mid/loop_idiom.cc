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

/// @file loop_idiom.cc
/// @brief Recognize zero-fill / array-copy inner loops -> rewrite to runtime Ocall.
///
/// See docs/loop_idiom.md for architectural notes.

#include "rcc/all.h"
#include "rcc/loopinfo.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

// store op -> byte width; non-store returns 0
int store_bytes(int op) {
    switch (op) {
        case Ostoreb:
            return 1;
        case Ostoreh:
            return 2;
        case Ostorew:
        case Ostores:
            return 4;
        case Ostorel:
        case Ostored:
            return 8;
        default:
            return 0;
    }
}

int load_bytes(int op, int cls) {
    switch (op) {
        case Oloadsb:
        case Oloadub:
            return 1;
        case Oloadsh:
        case Oloaduh:
            return 2;
        case Oloadsw:
        case Oloaduw:
            return 4;
        case Oload:
            return (cls == Kl || cls == Kd) ? 8 : 4;
        default:
            return 0;
    }
}

// Check byte splat.
bool is_byte_splat(Fn* fn, Ref v, uint8_t* byte_out) {
    if (rtype(v) != RCon)
        return false;
    Con* c = &fn->con[v.val];
    if (c->type != Con::CBits || c->flt)
        return false;
    uint64_t bits = static_cast<uint64_t>(c->bits.i);
    uint8_t b = bits & 0xff;
    uint64_t splat = static_cast<uint64_t>(b);
    splat |= splat << 8;
    splat |= splat << 16;
    splat |= splat << 32;
    if (bits != splat)
        return false;
    *byte_out = b;
    return true;
}

// Trace Ocopy chain back to a non-Ocopy root Ref.
Ref base_root(Fn* fn, Ref r) {
    for (int hop = 0; hop < 16; hop++) {
        Ins* d = def_ins(fn, r);
        if (!d || d->op != Ocopy)
            break;
        r = d->arg[0];
    }
    return r;
}

// Find IV increment index.
int find_iv_inc_idx(Blk* latch, Ref next) {
    for (uint k = 0; k < latch->nins; k++)
        if (req(latch->ins[k].to, next))
            return static_cast<int>(k);
    return -1;
}

enum class Idiom { None, Memset, Memcpy, Memmove };

struct Match {
    Idiom kind;
    Ref base_dst;
    Ref base_src;   // memcpy/memmove only
    uint8_t value;  // memset only
    Ref end_dst;    // for size = end - base
    Blk* after;
};

bool try_match_loop(Fn* fn, const Loop& L, Match* out) {
    // Validate loop structure.
    if (L.body.size() > 2)
        return false;
    // Disallow nesting.

    // Primary pointer IV (dst pointer for memset/memcpy)
    PointerIV iv_dst{};
    Phi* phi_dst = nullptr;
    int found = 0;
    for (Phi* ph = L.header->phi; ph; ph = ph->link) {
        PointerIV cand{};
        if (pick_pointer_iv(fn, L, ph, &cand)) {
            iv_dst = cand;
            phi_dst = ph;
            found++;
        }
    }
    if (found == 0)
        return false;
    // 1 IV -> memset candidate; 2 -> memcpy candidate; >2 -> reject
    PointerIV iv_src{};
    Phi* phi_src = nullptr;
    if (found >= 2) {
        if (found != 2)
            return false;
        // Rescan to get the two
        std::vector<std::pair<PointerIV, Phi*>> ivs;
        for (Phi* ph = L.header->phi; ph; ph = ph->link) {
            PointerIV cand{};
            if (pick_pointer_iv(fn, L, ph, &cand))
                ivs.push_back({cand, ph});
        }
        if (ivs.size() != 2)
            return false;
        // Will decide which is dst when scanning store in body
        iv_dst = ivs[0].first;
        phi_dst = ivs[0].second;
        iv_src = ivs[1].first;
        phi_src = ivs[1].second;
    }

    // exit cmp must be based on iv_dst (memset) or either one (memcpy)
    Ref end_dst;
    Blk* after = nullptr;
    if (!check_exit_cmp(fn, L, iv_dst.phi_to, &end_dst, &after)) {
        if (phi_src && check_exit_cmp(fn, L, iv_src.phi_to, &end_dst, &after)) {
            // Reversed: cmp based on src, swap dst/src naming
            std::swap(iv_dst, iv_src);
            std::swap(phi_dst, phi_src);
        } else {
            return false;
        }
    }
    if (iv_dst.step != iv_src.step && phi_src)
        return false;

    // Find IV self-update ins (add in latch)
    int iv_dst_inc = find_iv_inc_idx(L.latch, iv_dst.next);
    int iv_src_inc = phi_src ? find_iv_inc_idx(L.latch, iv_src.next) : -1;
    if (iv_dst_inc < 0)
        return false;
    if (phi_src && iv_src_inc < 0)
        return false;

    // Body scan.
    Ins* load_ins = nullptr;
    Ins* store_ins = nullptr;
    for (Blk* bb : L.body) {
        for (uint k = 0; k < bb->nins; k++) {
            Ins* i = &bb->ins[k];
            int op = i->op;
            // Allowed: exit cmp (in header), IV self-update (in latch)
            if (bb == L.header && req(i->to, L.header->jmp.arg))
                continue;  // exit cmp
            if (bb == L.latch &&
                (static_cast<int>(k) == iv_dst_inc || static_cast<int>(k) == iv_src_inc))
                continue;
            // Disallowed: call / alloc / FP / div / vastart / vaarg / dbgloc / sel / blit
            if (op == Ocall || op == Oalloc4 || op == Oalloc8 || op == Oalloc16 || op == Ovaarg ||
                op == Ovastart || op == Odbgloc || op == Osel0 || op == Osel1 || op == Oblit0 ||
                op == Oblit1 || op == Osalloc)
                return false;
            if (op == Oudiv || op == Ourem || op == Odiv || op == Orem)
                return false;
            if (op == Ocopy && (i->cls == Ks || i->cls == Kd))
                return false;
            // load / store
            int lb = load_bytes(op, static_cast<int>(i->cls));
            int sb = store_bytes(op);
            if (lb > 0) {
                if (load_ins)
                    return false;  // Reject multiple loads
                load_ins = i;
                continue;
            }
            if (sb > 0) {
                if (store_ins)
                    return false;
                store_ins = i;
                continue;
            }
            // Other allowed.
            if (op == Oadd || op == Osub || op == Omul || op == Oand || op == Oor || op == Oxor ||
                op == Oshl || op == Oshr || op == Osar || op == Oextsw || op == Oextuw ||
                op == Oextsh || op == Oextuh || op == Oextsb || op == Oextub || op == Ocopy ||
                op == Onop)
                continue;
            // Other cmp (besides LFTR cmp): conservatively reject
            return false;
        }
    }

    if (!store_ins)
        return false;
    int sw = store_bytes(store_ins->op);
    if (sw != iv_dst.step)
        return false;
    if (!req(store_ins->arg[1], iv_dst.phi_to))
        return false;

    if (has_outside_use(fn, L))
        return false;

    if (load_ins) {
        // memcpy path
        if (!phi_src)
            return false;
        int lw = load_bytes(load_ins->op, static_cast<int>(load_ins->cls));
        if (lw != sw)
            return false;
        if (!req(load_ins->arg[0], iv_src.phi_to))
            return false;
        if (!req(store_ins->arg[0], load_ins->to))
            return false;
        // Noalias check.
        Ref root_dst = base_root(fn, iv_dst.base);
        Ref root_src = base_root(fn, iv_src.base);
        out->kind = req(root_dst, root_src) ? Idiom::Memmove : Idiom::Memcpy;
        out->base_dst = iv_dst.base;
        out->base_src = iv_src.base;
        out->value = 0;
        out->end_dst = end_dst;
        out->after = after;
        return true;
    }

    // memset path
    if (phi_src)
        return false;  // Second IV present but no load, abnormal shape
    uint8_t bv = 0;
    if (!is_byte_splat(fn, store_ins->arg[0], &bv))
        return false;
    out->kind = Idiom::Memset;
    out->base_dst = iv_dst.base;
    out->base_src = Ref::none();
    out->value = bv;
    out->end_dst = end_dst;
    out->after = after;
    return true;
}

const char* runtime_name(Idiom k) {
    switch (k) {
        case Idiom::Memset:
            return "__rcc_memset";
        case Idiom::Memcpy:
            return "__rcc_memcpy";
        case Idiom::Memmove:
            return "__rcc_memmove";
        default:
            return "";
    }
}

Ref make_runtime_callee(Ctx* ctx, Fn* fn, const char* name) {
    Con c;
    std::memset(&c, 0, sizeof(c));
    c.type = Con::CAddr;
    c.sym.type = Sym::SGlo;
    c.sym.id = intern(const_cast<char*>(name));
    return newcon(ctx, &c, fn);
}

void execute_rewrite(Ctx* ctx, Fn* fn, const Loop& L, const Match& m) {
    Blk* pre = L.preheader;

    // size = end - base (Osub Kl)
    Ref size_ref = newtmp(ctx, "midiom", Kl, fn);
    Ins sub_ins{};
    sub_ins.op = Osub;
    sub_ins.cls = Kl;
    sub_ins.to = size_ref;
    sub_ins.arg[0] = m.end_dst;
    sub_ins.arg[1] = m.base_dst;
    addins(ctx, &pre->ins, &pre->nins, &sub_ins);

    // Oarg dst (Kl)
    Ins arg0{};
    arg0.op = Oarg;
    arg0.cls = Kl;
    arg0.to = Ref::none();
    arg0.arg[0] = m.base_dst;
    arg0.arg[1] = Ref::none();
    addins(ctx, &pre->ins, &pre->nins, &arg0);

    // Oarg val/src
    Ins arg1{};
    arg1.op = Oarg;
    arg1.to = Ref::none();
    arg1.arg[1] = Ref::none();
    if (m.kind == Idiom::Memset) {
        arg1.cls = Kw;
        arg1.arg[0] = getcon(ctx, static_cast<int64_t>(m.value), fn);
    } else {
        arg1.cls = Kl;
        arg1.arg[0] = m.base_src;
    }
    addins(ctx, &pre->ins, &pre->nins, &arg1);

    // Oarg size (Kl)
    Ins arg2{};
    arg2.op = Oarg;
    arg2.cls = Kl;
    arg2.to = Ref::none();
    arg2.arg[0] = size_ref;
    arg2.arg[1] = Ref::none();
    addins(ctx, &pre->ins, &pre->nins, &arg2);

    // Ocall (returns void -> cls=Kw, to=none)
    Ins call_ins{};
    call_ins.op = Ocall;
    call_ins.cls = Kw;
    call_ins.to = Ref::none();
    call_ins.arg[0] = make_runtime_callee(ctx, fn, runtime_name(m.kind));
    call_ins.arg[1] = Ref::none();
    addins(ctx, &pre->ins, &pre->nins, &call_ins);

    fn->leaf = 0;

    // change preheader to jump directly to after
    pre->jmp.type = Jjmp;
    pre->jmp.arg = Ref::none();
    pre->jmp.arg2 = Ref::none();
    pre->s1 = m.after;
    pre->s2 = nullptr;
}

}  // namespace

void loop_idiom(Ctx* ctx, Fn* fn) {
    std::vector<Loop> loops;
    find_loops(fn, loops);
    if (loops.empty())
        return;

    // Sort innermost first.
    std::sort(loops.begin(), loops.end(),
              [](const Loop& a, const Loop& b) { return a.body.size() < b.body.size(); });

    std::unordered_set<uint> consumed_blk;
    for (const Loop& L : loops) {
        bool clash = false;
        for (Blk* bb : L.body)
            if (consumed_blk.count(bb->id)) {
                clash = true;
                break;
            }
        if (clash)
            continue;
        Match m{};
        if (!try_match_loop(fn, L, &m))
            continue;
        execute_rewrite(ctx, fn, L, m);
        for (Blk* bb : L.body)
            consumed_blk.insert(bb->id);
    }
}

}  // namespace rcc
