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

/// @file lsr.cc
/// @brief Loop Strength Reduction (LSR).
///
/// See docs/lsr.md for architectural notes.

#include "rcc/all.h"
#include "rcc/loopinfo.h"

#include <cstring>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

constexpr int kFixpointIters = 4;

// IVs created by reduce_mul / insert_addr_iv.
struct CreatedIV {
    Ref to;        // header phi.to
    Ref init_ref;  // phi preheader incoming edge Ref
    int64_t step;
    int cls;
    int src_biv;
    int64_t c;
};

uint64_t ref_key(Ref r) {
    return (static_cast<uint64_t>(static_cast<uint>(rtype(r))) << 32) |
           static_cast<uint32_t>(r.val);
}

int find_biv(const std::vector<Biv>& bivs, Ref r) {
    for (size_t k = 0; k < bivs.size(); k++)
        if (req(bivs[k].to, r))
            return static_cast<int>(k);
    return -1;
}

struct MulCand {
    Blk* b;
    uint idx;
    int biv;
    int64_t c;
    bool is_kl;
};

// Returns number of hits.
int reduce_mul(Ctx* ctx, Fn* fn, const Loop& L, std::vector<Biv>& bivs,
               std::vector<CreatedIV>& created_ivs) {
    std::vector<MulCand> cands;
    for (Blk* b : L.body) {
        for (uint ix = 0; ix < b->nins; ix++) {
            Ins* m = &b->ins[ix];
            if (m->op != Omul)
                continue;

            int64_t c = 0;
            Ref tref = Ref::none();
            if (ref_is_const(fn, m->arg[1], c) && rtype(m->arg[0]) == RTmp)
                tref = m->arg[0];
            else if (ref_is_const(fn, m->arg[0], c) && rtype(m->arg[1]) == RTmp)
                tref = m->arg[1];
            else
                continue;
            if (c == 0 || c == 1)
                continue;

            int bi = -1;
            bool is_kl = false;

            int d = find_biv(bivs, tref);
            if (d >= 0 && static_cast<int>(m->cls) == bivs[static_cast<size_t>(d)].cls) {
                bi = d;
                is_kl = (m->cls == Kl);
            }
            if (bi < 0 && m->cls == Kl) {
                Ins* ext = def_ins(fn, tref);
                if (ext && ext->op == Oextsw) {
                    int e = find_biv(bivs, ext->arg[0]);
                    if (e >= 0 && bivs[static_cast<size_t>(e)].cls == Kw) {
                        bi = e;
                        is_kl = true;
                    }
                }
            }

            if (bi < 0)
                continue;
            cands.push_back({b, ix, bi, c, is_kl});
        }
    }
    if (cands.empty())
        return 0;

    struct Made {
        int biv;
        int64_t c;
        bool is_kl;
        Ref j;
        int64_t init;
        int64_t step;
    };
    std::vector<Made> made;

    auto get_iv = [&](int bi, int64_t c, bool is_kl) -> Ref {
        for (auto& md : made)
            if (md.biv == bi && md.c == c && md.is_kl == is_kl)
                return md.j;

        const Biv& iv = bivs[static_cast<size_t>(bi)];
        int cls = is_kl ? Kl : Kw;
        int64_t j_init = iv.init * c;
        int64_t j_step = iv.step * c;
        Ref jt = newtmp(ctx, "lsr", cls, fn);
        Ref jn = newtmp(ctx, "lsr", cls, fn);

        Ins add{};
        add.op = Oadd;
        add.cls = static_cast<uint>(cls) & 3u;
        add.to = jn;
        add.arg[0] = jt;
        add.arg[1] = getcon(ctx, j_step, fn);
        addins(ctx, &L.latch->ins, &L.latch->nins, &add);

        Phi* p = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
        std::memset(p, 0, sizeof(Phi));
        p->to = jt;
        p->cls = static_cast<short>(cls);
        p->narg = 2;
        p->arg = static_cast<Ref*>(vnew(ctx, 2, sizeof(Ref), PFn));
        p->blk = static_cast<Blk**>(vnew(ctx, 2, sizeof(Blk*), PFn));
        p->arg[0] = getcon(ctx, j_init, fn);
        p->blk[0] = L.preheader;
        p->arg[1] = jn;
        p->blk[1] = L.latch;
        p->link = L.header->phi;
        L.header->phi = p;

        made.push_back({bi, c, is_kl, jt, j_init, j_step});
        // Fallback: add derived IV to bivs.
        bivs.push_back({jt, cls, j_init, j_step});
        // Record for LFTR.
        Ref j_init_ref = getcon(ctx, j_init, fn);
        created_ivs.push_back({jt, j_init_ref, j_step, cls, bi, c});
        return jt;
    };

    for (auto& cd : cands) {
        Ref j = get_iv(cd.biv, cd.c, cd.is_kl);
        Ins* m = &cd.b->ins[cd.idx];  // get_iv might realloc latch->ins, re-fetch here
        m->op = Ocopy;
        m->arg[0] = j;
        m->arg[1] = Ref::none();
        // m->cls unchanged (matches j)
    }
    return static_cast<int>(cands.size());
}

// Return phi input.
Ref phi_arg_from(Phi* p, Blk* from) {
    for (uint k = 0; k < p->narg; k++)
        if (p->blk[k] == from)
            return p->arg[k];
    return Ref::none();
}

Phi* find_header_phi(Blk* hd, Ref to) {
    for (Phi* p = hd->phi; p; p = p->link)
        if (req(p->to, to))
            return p;
    return nullptr;
}

bool is_pure_arith(int op) {
    return op == Oadd || op == Osub || op == Omul || op == Oextsw || op == Ocopy;
}

// Check load invariance.
bool load_invariant_in_loop(Fn* fn, const Loop& L, Ins* load) {
    int lsz = loadsz(load);
    if (lsz <= 0)
        return false;
    for (Blk* bb : L.body) {
        for (uint i = 0; i < bb->nins; i++) {
            Ins* s = &bb->ins[i];
            if (s->op == Ocall)
                return false;  // call may clobber any global
            if (!isstore(s->op))
                continue;
            int ssz = storesz(s);
            int delta_unused = 0;
            // storeX value, addr → arg[0]=value, arg[1]=addr
            int a = alias(load->arg[0], 0, lsz, s->arg[1], ssz, &delta_unused, fn);
            if (a != NoAlias)
                return false;
        }
    }
    return true;
}

// SCEV result.
struct Scev {
    int64_t step;
    int src;
};

int insert_addr_iv(Ctx* ctx, Fn* fn, const Loop& L, const std::vector<Biv>& bivs,
                   std::vector<CreatedIV>& created_ivs) {
    // Invariance check (memoized)
    std::unordered_map<uint64_t, int> inv_memo;  // -1 uncomputed, 0 false, 1 true
    std::function<bool(Ref)> is_inv = [&](Ref r) -> bool {
        if (rtype(r) == RCon)
            return true;
        if (rtype(r) != RTmp)
            return false;
        uint64_t k = ref_key(r);
        auto it = inv_memo.find(k);
        if (it != inv_memo.end())
            return it->second == 1;
        inv_memo[k] =
            -1;  // Mark to prevent recursion cycles; non-self-loop phi returning here is false
        bool result = false;
        do {
            uint bid = fn->tmp[r.val].bid;
            if (L.body_bid.find(bid) == L.body_bid.end()) {
                // Def not in loop -> invariant
                result = true;
                break;
            }
            Ins* d = fn->tmp[r.val].def;
            if (!d) {
                // phi def check.
                if (bid != L.header->id) {
                    result = false;
                    break;
                }
                Phi* p = find_header_phi(L.header, r);
                if (!p) {
                    result = false;
                    break;
                }
                bool all_ok = true;
                for (uint i = 0; i < p->narg; i++) {
                    if (req(p->arg[i], p->to))
                        continue;  // self-loop
                    if (!is_inv(p->arg[i])) {
                        all_ok = false;
                        break;
                    }
                }
                result = all_ok;
                break;
            }
            if (isload(d->op)) {
                result = load_invariant_in_loop(fn, L, d);
                break;
            }
            if (!is_pure_arith(d->op)) {
                result = false;
                break;
            }
            bool a0_ok = req(d->arg[0], Ref::none()) || is_inv(d->arg[0]);
            bool a1_ok = req(d->arg[1], Ref::none()) || is_inv(d->arg[1]);
            result = a0_ok && a1_ok;
        } while (false);
        inv_memo[k] = result ? 1 : 0;
        return result;
    };

    // SCEV returns {step, src_biv}.
    std::unordered_map<uint64_t, std::optional<Scev>> scev_memo;
    std::function<std::optional<Scev>(Ref)> scev = [&](Ref r) -> std::optional<Scev> {
        if (rtype(r) == RCon)
            return Scev{0, -1};
        if (rtype(r) != RTmp)
            return std::nullopt;
        uint64_t k = ref_key(r);
        auto it = scev_memo.find(k);
        if (it != scev_memo.end())
            return it->second;
        scev_memo[k] = std::nullopt;  // prevent recursion cycle
        std::optional<Scev> ans = std::nullopt;

        auto combine = [](Scev a, Scev b, int sign) -> std::optional<Scev> {
            int64_t step = a.step + sign * b.step;
            int src;
            if (a.step == 0 && b.step == 0)
                src = -1;
            else if (a.step == 0)
                src = b.src;
            else if (b.step == 0)
                src = a.src;
            else if (a.src == b.src)
                src = a.src;
            else
                return std::nullopt;
            if (step == 0)
                src = -1;
            return Scev{step, src};
        };

        do {
            int bi = find_biv(bivs, r);
            if (bi >= 0) {
                ans = Scev{bivs[static_cast<size_t>(bi)].step, bi};
                break;
            }
            if (is_inv(r)) {
                ans = Scev{0, -1};
                break;
            }
            Ins* d = fn->tmp[r.val].def;
            if (!d)
                break;  // non-BIV head phi / non-invariant phi -> fail
            int64_t cv = 0;
            switch (d->op) {
                case Ocopy:
                    ans = scev(d->arg[0]);
                    break;
                case Oextsw: {
                    ans = scev(d->arg[0]);  // sext passthrough, assumes no int32 wrap
                    break;
                }
                case Oadd: {
                    auto s0 = scev(d->arg[0]);
                    auto s1 = scev(d->arg[1]);
                    if (s0.has_value() && s1.has_value())
                        ans = combine(*s0, *s1, +1);
                    break;
                }
                case Osub: {
                    auto s0 = scev(d->arg[0]);
                    auto s1 = scev(d->arg[1]);
                    if (s0.has_value() && s1.has_value())
                        ans = combine(*s0, *s1, -1);
                    break;
                }
                case Omul: {
                    // One side linear, one side constant
                    if (ref_is_const(fn, d->arg[1], cv)) {
                        auto s = scev(d->arg[0]);
                        if (s.has_value()) {
                            int64_t step = s->step * cv;
                            ans = Scev{step, step == 0 ? -1 : s->src};
                        }
                    } else if (ref_is_const(fn, d->arg[0], cv)) {
                        auto s = scev(d->arg[1]);
                        if (s.has_value()) {
                            int64_t step = s->step * cv;
                            ans = Scev{step, step == 0 ? -1 : s->src};
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        } while (false);
        scev_memo[k] = ans;
        return ans;
    };

    // Materialize init.
    std::unordered_map<uint64_t, Ref> mat_memo;
    std::function<Ref(Ref)> materialize = [&](Ref r) -> Ref {
        if (rtype(r) == RCon)
            return r;
        uint64_t k = ref_key(r);
        auto it = mat_memo.find(k);
        if (it != mat_memo.end())
            return it->second;

        // BIV -> init constant
        int bi = find_biv(bivs, r);
        if (bi >= 0) {
            Ref c = getcon(ctx, bivs[static_cast<size_t>(bi)].init, fn);
            mat_memo[k] = c;
            return c;
        }

        // Decide based on def block.
        uint bid = fn->tmp[r.val].bid;
        if (L.body_bid.find(bid) == L.body_bid.end()) {
            mat_memo[k] = r;
            return r;
        }
        Ins* d = fn->tmp[r.val].def;
        if (!d) {
            // phi in body header.
            if (bid != L.header->id) {
                mat_memo[k] =
                    r;  // Fallback: shouldn't reach here (filtered by scev), but prevents crash
                return r;
            }
            Phi* p = find_header_phi(L.header, r);
            if (!p) {
                mat_memo[k] = r;
                return r;
            }
            Ref pe = phi_arg_from(p, L.preheader);
            Ref sub = materialize(pe);
            mat_memo[k] = sub;
            return sub;
        }

        // Ins in body: Ocopy/Oextsw/Oadd/Osub/Omul - copy one to preheader
        Ref a0 = req(d->arg[0], Ref::none()) ? Ref::none() : materialize(d->arg[0]);
        Ref a1 = req(d->arg[1], Ref::none()) ? Ref::none() : materialize(d->arg[1]);

        if (d->op == Ocopy) {
            mat_memo[k] = a0;
            return a0;
        }
        if (d->op == Oextsw && rtype(a0) == RCon) {
            mat_memo[k] = a0;  // Kl constant itself is fine
            return a0;
        }

        int cls = static_cast<int>(d->cls);
        Ref nt = newtmp(ctx, "lsr", cls, fn);
        Ins nins{};
        nins.op = d->op;
        nins.cls = static_cast<uint>(cls) & 3u;
        nins.to = nt;
        nins.arg[0] = a0;
        nins.arg[1] = a1;
        addins(ctx, &L.preheader->ins, &L.preheader->nins, &nins);
        mat_memo[k] = nt;
        return nt;
    };

    // Scan candidates.
    auto resolve_copy = [&](Ref r) -> Ref {
        // Trace Ocopy chain.
        for (int hop = 0; hop < 8; hop++) {
            Ins* d = def_ins(fn, r);
            if (!d || d->op != Ocopy)
                break;
            r = d->arg[0];
        }
        return r;
    };
    struct AddrCand {
        Blk* b;
        uint idx;
        int64_t step;
        int src_biv;
        uint64_t dedup_key;
    };
    std::vector<AddrCand> cands;
    for (Blk* b : L.body) {
        for (uint ix = 0; ix < b->nins; ix++) {
            Ins* m = &b->ins[ix];
            if (m->op != Oadd || m->cls != Kl)
                continue;
            int64_t cb = 0;
            if (ref_is_const(fn, m->arg[0], cb) || ref_is_const(fn, m->arg[1], cb))
                continue;  // CBits step -> BIV self-increment, skip
            auto s = scev(m->to);
            if (!s.has_value() || s->step == 0)
                continue;
            Ref a0r = resolve_copy(m->arg[0]);
            Ref a1r = resolve_copy(m->arg[1]);
            uint64_t k0 = ref_key(a0r);
            uint64_t k1 = ref_key(a1r);
            if (k0 > k1)
                std::swap(k0, k1);
            uint64_t dk = (k0 * 1315423911ull) ^ (k1 + 0x9E3779B97F4A7C15ull);
            cands.push_back({b, ix, s->step, s->src, dk});
        }
    }
    if (cands.empty())
        return 0;

    // Deduplication + Rewrite
    struct IVRec {
        uint64_t key;
        Ref p;
    };
    std::vector<IVRec> ivs;

    auto find_iv = [&](uint64_t key) -> Ref {
        for (auto& r : ivs)
            if (r.key == key)
                return r.p;
        return Ref::none();
    };

    int hits = 0;
    for (auto& cd : cands) {
        Ref p = find_iv(cd.dedup_key);
        if (req(p, Ref::none())) {
            // Materialize base (init at preheader)
            Ins* m0 = &cd.b->ins[cd.idx];
            Ref orig_to = m0->to;
            Ref p_init = materialize(orig_to);

            int cls = Kl;
            Ref pt = newtmp(ctx, "lsr", cls, fn);
            Ref pn = newtmp(ctx, "lsr", cls, fn);

            // latch: pn = pt + step
            Ins add{};
            add.op = Oadd;
            add.cls = static_cast<uint>(cls) & 3u;
            add.to = pn;
            add.arg[0] = pt;
            add.arg[1] = getcon(ctx, cd.step, fn);
            addins(ctx, &L.latch->ins, &L.latch->nins, &add);

            // header: pt = phi(p_init @ pre, pn @ latch)
            Phi* ph = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
            std::memset(ph, 0, sizeof(Phi));
            ph->to = pt;
            ph->cls = static_cast<short>(cls);
            ph->narg = 2;
            ph->arg = static_cast<Ref*>(vnew(ctx, 2, sizeof(Ref), PFn));
            ph->blk = static_cast<Blk**>(vnew(ctx, 2, sizeof(Blk*), PFn));
            ph->arg[0] = p_init;
            ph->blk[0] = L.preheader;
            ph->arg[1] = pn;
            ph->blk[1] = L.latch;
            ph->link = L.header->phi;
            L.header->phi = ph;

            ivs.push_back({cd.dedup_key, pt});
            p = pt;

            // Record CreatedIV.
            int64_t c = 0;
            if (cd.src_biv >= 0) {
                int64_t bs = bivs[static_cast<size_t>(cd.src_biv)].step;
                if (bs != 0 && cd.step % bs == 0)
                    c = cd.step / bs;
            }
            created_ivs.push_back({pt, p_init, cd.step, Kl, cd.src_biv, c});
        }

        // Re-fetch pointer.
        Ins* m = &cd.b->ins[cd.idx];
        m->op = Ocopy;
        m->arg[0] = p;
        m->arg[1] = Ref::none();
        // m->cls remains Kl
        hits++;
    }
    return hits;
}

// Exit cmp mapping.
bool classify_cmp(int op, int& op_w, int& op_w_flip, int& op_l, int& op_l_flip) {
    auto setpair = [&](int w, int w_flip, int l, int l_flip) {
        op_w = w;
        op_w_flip = w_flip;
        op_l = l;
        op_l_flip = l_flip;
    };
    switch (op) {
        case Oceqw:
        case Oceql:
            setpair(Oceqw, Oceqw, Oceql, Oceql);
            return true;
        case Ocnew:
        case Ocnel:
            setpair(Ocnew, Ocnew, Ocnel, Ocnel);
            return true;
        case Ocsltw:
        case Ocsltl:
            setpair(Ocsltw, Ocsgtw, Ocsltl, Ocsgtl);
            return true;
        case Ocslew:
        case Ocslel:
            setpair(Ocslew, Ocsgew, Ocslel, Ocsgel);
            return true;
        case Ocsgtw:
        case Ocsgtl:
            setpair(Ocsgtw, Ocsltw, Ocsgtl, Ocsltl);
            return true;
        case Ocsgew:
        case Ocsgel:
            setpair(Ocsgew, Ocslew, Ocsgel, Ocslel);
            return true;
        default:
            return false;
    }
}

int replace_exit_test(Ctx* ctx, Fn* fn, const Loop& L, const std::vector<Biv>& bivs,
                      const std::vector<CreatedIV>& created_ivs) {
    if (L.header->jmp.type != Jjnz)
        return 0;
    Ref cond = L.header->jmp.arg;
    if (rtype(cond) != RTmp)
        return 0;

    // cmp must be in header, and nuse==1 (otherwise modifying it affects other places)
    Ins* cmp = nullptr;
    uint cmp_idx = 0;
    for (uint i = 0; i < L.header->nins; i++) {
        if (req(L.header->ins[i].to, cond)) {
            cmp = &L.header->ins[i];
            cmp_idx = i;
            break;
        }
    }
    if (!cmp)
        return 0;
    if (fn->tmp[cond.val].nuse != 1)
        return 0;

    int op_w, op_w_flip, op_l, op_l_flip;
    if (!classify_cmp(static_cast<int>(cmp->op), op_w, op_w_flip, op_l, op_l_flip))
        return 0;

    // Find BIV side + invariant side
    int bi = find_biv(bivs, cmp->arg[0]);
    Ref n_ref;
    bool biv_at_0;
    if (bi >= 0) {
        n_ref = cmp->arg[1];
        biv_at_0 = true;
    } else {
        bi = find_biv(bivs, cmp->arg[1]);
        if (bi < 0)
            return 0;
        n_ref = cmp->arg[0];
        biv_at_0 = false;
    }
    // If the other side is also a BIV, LFTR skips (comparison between two IVs)
    if (find_biv(bivs, n_ref) >= 0)
        return 0;

    // LFTR Guard.
    {
        Ref biv_to = bivs[static_cast<size_t>(bi)].to;
        Phi* biv_phi = find_header_phi(L.header, biv_to);
        Ins* self_inc = nullptr;
        if (biv_phi) {
            for (uint k = 0; k < biv_phi->narg; k++) {
                if (biv_phi->blk[k] == L.latch) {
                    Ref a = biv_phi->arg[k];
                    if (rtype(a) == RTmp && a.val < static_cast<uint>(fn->ntmp))
                        self_inc = fn->tmp[a.val].def;
                    break;
                }
            }
        }
        if (biv_to.val >= static_cast<uint>(fn->ntmp))
            return 0;
        Tmp* bt = &fn->tmp[biv_to.val];
        int extra = 0;
        for (uint u = 0; u < bt->nuse; u++) {
            const Use& uu = bt->use[u];
            if (uu.type == Use::UIns) {
                if (uu.u.ins == cmp)
                    continue;
                if (uu.u.ins == self_inc)
                    continue;
                // Orphans check.
                Ref user_to = uu.u.ins->to;
                if (rtype(user_to) == RTmp && user_to.val < static_cast<uint>(fn->ntmp) &&
                    fn->tmp[user_to.val].nuse == 0)
                    continue;
                extra++;
            } else if (uu.type == Use::UPhi) {
                // Count phi use.
                extra++;
            } else if (uu.type == Use::UJmp) {
                extra++;
            }
        }
        if (extra > 0)
            return 0;
    }

    // Materialize to preheader.
    std::unordered_map<uint64_t, int> inv_memo;
    std::function<bool(Ref)> is_inv = [&](Ref r) -> bool {
        if (rtype(r) == RCon)
            return true;
        if (rtype(r) != RTmp)
            return false;
        uint64_t k = ref_key(r);
        auto it = inv_memo.find(k);
        if (it != inv_memo.end())
            return it->second == 1;
        inv_memo[k] = -1;
        bool result = false;
        do {
            uint bid = fn->tmp[r.val].bid;
            if (L.body_bid.find(bid) == L.body_bid.end()) {
                result = true;
                break;
            }
            Ins* d = fn->tmp[r.val].def;
            if (!d) {
                if (bid != L.header->id) {
                    result = false;
                    break;
                }
                Phi* p = find_header_phi(L.header, r);
                if (!p) {
                    result = false;
                    break;
                }
                bool all_ok = true;
                for (uint i = 0; i < p->narg; i++) {
                    if (req(p->arg[i], p->to))
                        continue;
                    if (!is_inv(p->arg[i])) {
                        all_ok = false;
                        break;
                    }
                }
                result = all_ok;
                break;
            }
            if (isload(d->op)) {
                result = load_invariant_in_loop(fn, L, d);
                break;
            }
            if (!is_pure_arith(d->op)) {
                result = false;
                break;
            }
            bool a0_ok = req(d->arg[0], Ref::none()) || is_inv(d->arg[0]);
            bool a1_ok = req(d->arg[1], Ref::none()) || is_inv(d->arg[1]);
            result = a0_ok && a1_ok;
        } while (false);
        inv_memo[k] = result ? 1 : 0;
        return result;
    };

    if (!is_inv(n_ref))
        return 0;

    // Materialize n_ref to preheader
    std::unordered_map<uint64_t, Ref> mat_memo;
    std::function<Ref(Ref)> materialize = [&](Ref r) -> Ref {
        if (rtype(r) == RCon)
            return r;
        uint64_t k = ref_key(r);
        auto it = mat_memo.find(k);
        if (it != mat_memo.end())
            return it->second;
        uint bid = fn->tmp[r.val].bid;
        if (L.body_bid.find(bid) == L.body_bid.end()) {
            mat_memo[k] = r;
            return r;
        }
        Ins* d = fn->tmp[r.val].def;
        if (!d) {
            // phi in body (invariant): take preheader edge
            if (bid != L.header->id) {
                mat_memo[k] = r;
                return r;
            }
            Phi* p = find_header_phi(L.header, r);
            if (!p) {
                mat_memo[k] = r;
                return r;
            }
            Ref pe = phi_arg_from(p, L.preheader);
            Ref sub = materialize(pe);
            mat_memo[k] = sub;
            return sub;
        }
        Ref a0 = req(d->arg[0], Ref::none()) ? Ref::none() : materialize(d->arg[0]);
        Ref a1 = req(d->arg[1], Ref::none()) ? Ref::none() : materialize(d->arg[1]);
        if (d->op == Ocopy) {
            mat_memo[k] = a0;
            return a0;
        }
        if (d->op == Oextsw && rtype(a0) == RCon) {
            mat_memo[k] = a0;
            return a0;
        }
        int cls = static_cast<int>(d->cls);
        Ref nt = newtmp(ctx, "lsr.lftr", cls, fn);
        Ins ni{};
        ni.op = d->op;
        ni.cls = static_cast<uint>(cls) & 3u;
        ni.to = nt;
        ni.arg[0] = a0;
        ni.arg[1] = a1;
        addins(ctx, &L.preheader->ins, &L.preheader->nins, &ni);
        mat_memo[k] = nt;
        return nt;
    };

    Ref n_at_pre = materialize(n_ref);

    // Choose replacement IV: src_biv == bi and c != 0, pick smallest |c|.
    int best = -1;
    int64_t best_abs = 0;
    for (size_t i = 0; i < created_ivs.size(); i++) {
        const CreatedIV& iv = created_ivs[i];
        if (iv.src_biv != bi)
            continue;
        if (iv.c == 0)
            continue;
        int64_t a = iv.c < 0 ? -iv.c : iv.c;
        if (best < 0 || a < best_abs) {
            best = static_cast<int>(i);
            best_abs = a;
        }
    }
    if (best < 0)
        return 0;

    const CreatedIV& iv = created_ivs[static_cast<size_t>(best)];
    int64_t i0 = bivs[static_cast<size_t>(bi)].init;
    int64_t cc = iv.c;
    int iv_cls = iv.cls;  // Kw or Kl, determines cls of preheader arithmetic and new cmp

    // n_at_pre's cls must be compatible with IV:
    // - iv Kl: if n Kw -> extsw; if n Kl -> use directly
    // - iv Kw: if n Kw -> use directly; if n Kl -> unsafe (truncation), skip
    int n_cls = Kl;
    if (rtype(n_at_pre) == RTmp)
        n_cls = static_cast<int>(fn->tmp[n_at_pre.val].cls);
    else
        n_cls =
            iv_cls;  // RCon treated as target cls (CBits 64-bit constant, getcon reused directly)
    Ref n_use;
    if (iv_cls == Kl && n_cls == Kw) {
        n_use = newtmp(ctx, "lsr.lftr", Kl, fn);
        Ins ni{};
        ni.op = Oextsw;
        ni.cls = static_cast<uint>(Kl) & 3u;
        ni.to = n_use;
        ni.arg[0] = n_at_pre;
        ni.arg[1] = Ref::none();
        addins(ctx, &L.preheader->ins, &L.preheader->nins, &ni);
    } else if (iv_cls == n_cls || rtype(n_at_pre) == RCon) {
        n_use = n_at_pre;
    } else {
        return 0;  // iv Kw, n Kl is unsafe
    }

    Ref i0_c = getcon(ctx, i0, fn);
    Ref c_c = getcon(ctx, cc, fn);

    auto emit_pre = [&](int op, int cls, Ref a0, Ref a1) -> Ref {
        Ref nt = newtmp(ctx, "lsr.lftr", cls, fn);
        Ins ni{};
        ni.op = static_cast<uint>(op) & 0x3fffffffu;
        ni.cls = static_cast<uint>(cls) & 3u;
        ni.to = nt;
        ni.arg[0] = a0;
        ni.arg[1] = a1;
        addins(ctx, &L.preheader->ins, &L.preheader->nins, &ni);
        return nt;
    };
    Ref diff = emit_pre(Osub, iv_cls, n_use, i0_c);
    Ref scaled = emit_pre(Omul, iv_cls, diff, c_c);
    Ref pend = emit_pre(Oadd, iv_cls, iv.init_ref, scaled);

    // Rewrite cmp (in-place; preheader's addins might realloc, so re-fetch is safe)
    cmp = &L.header->ins[cmp_idx];
    int final_op = (iv_cls == Kl) ? (cc < 0 ? op_l_flip : op_l) : (cc < 0 ? op_w_flip : op_w);
    cmp->op = static_cast<uint>(final_op) & 0x3fffffffu;
    cmp->cls = static_cast<uint>(iv_cls) & 3u;
    if (biv_at_0) {
        cmp->arg[0] = iv.to;
        cmp->arg[1] = pend;
    } else {
        cmp->arg[0] = pend;
        cmp->arg[1] = iv.to;
    }
    return 1;
}

bool run_loop(Ctx* ctx, Fn* fn, const Loop& L, int& reduced_total, int& lifted_total,
              int& retested_total) {
    std::vector<Biv> bivs;
    detect_bivs(fn, L, bivs);
    if (bivs.empty())
        return false;

    std::vector<CreatedIV> created_ivs;
    int reduced = reduce_mul(ctx, fn, L, bivs, created_ivs);
    int lifted = insert_addr_iv(ctx, fn, L, bivs, created_ivs);
    int retested = 0;
    if (!created_ivs.empty()) {
        // tmp.nuse might be stale after insert_addr_iv rewrites mul->Ocopy. LFTR guard relies on
        // nuse, so update once.
        filluse(ctx, fn);
        retested = replace_exit_test(ctx, fn, L, bivs, created_ivs);
    }

    reduced_total += reduced;
    lifted_total += lifted;
    retested_total += retested;
    return (reduced + lifted + retested) > 0;
}

// LSR cleanup: resolve all RTmp references along Ocopy chains to their roots.
// insert_addr_iv rewrites address additions as Ocopy %new_iv, leaving isolated Ocopys.
// Downstream passes expect canonical IR. Walk uses and resolve RTmp defs if they are Ocopy.
// Keep Ocopy itself; nuse becomes 0, and DCE cleans it up.
void propagate_copies(Fn* fn) {
    auto resolve = [&](Ref r) -> Ref {
        for (int hop = 0; hop < 16; hop++) {
            if (rtype(r) != RTmp)
                break;
            uint v = r.val;
            if (v >= static_cast<uint>(fn->ntmp))
                break;
            Ins* d = fn->tmp[v].def;
            if (!d || d->op != Ocopy)
                break;
            if (rtype(d->arg[0]) == -1)
                break;
            r = d->arg[0];
        }
        return r;
    };
    for (Blk* b = fn->start; b; b = b->link) {
        for (Phi* p = b->phi; p; p = p->link)
            for (uint k = 0; k < p->narg; k++)
                p->arg[k] = resolve(p->arg[k]);
        for (uint k = 0; k < b->nins; k++) {
            Ins* i = &b->ins[k];
            if (i->op == Ocopy)
                continue;  // Do not resolve Ocopy's own arg to preserve chain info
            for (int a = 0; a < 2; a++)
                i->arg[a] = resolve(i->arg[a]);
        }
        b->jmp.arg = resolve(b->jmp.arg);
        b->jmp.arg2 = resolve(b->jmp.arg2);
    }
}

}  // namespace

void lsr(Ctx* ctx, Fn* fn) {
    // alias info used by is_inv to check loop-invariant loads.
    // LSR only produces phi/add/mul/copy/sub, doesn't touch load/store, so no need to recalculate
    // inside fixpoint iter.
    fillalias(fn);

    int iter = 0;
    int reduced_total = 0;
    int lifted_total = 0;
    int retested_total = 0;
    for (; iter < kFixpointIters; iter++) {
        std::vector<Loop> loops;
        find_loops(fn, loops);
        if (loops.empty())
            break;

        bool changed = false;
        for (auto& L : loops)
            if (run_loop(ctx, fn, L, reduced_total, lifted_total, retested_total))
                changed = true;
        if (!changed)
            break;
        // CFG unchanged; new Ins def requires filluse to write Tmp.def for next round's
        // scev/detect_bivs.
        filluse(ctx, fn);
    }

    // After fixpoint, resolve Ocopy chains left by insert_addr_iv globally.
    // Downstream passes won't see intermediate Ocopy references. Isolated Ocopy ins cleaned by DCE.
    propagate_copies(fn);

    if (debug['V']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out,
                "\n> LSR %s: reduced=%d mul, lifted=%d addr, retested=%d exit, in %d round%s\n",
                fn->name, reduced_total, lifted_total, retested_total, iter, iter == 1 ? "" : "s");
        printfn(fn, out);
    }
}

}  // namespace rcc
