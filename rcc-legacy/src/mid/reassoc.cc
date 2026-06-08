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

/// @file reassoc.cc
/// @brief Reassociation pass (LLVM rank-based style).
///
/// See docs/reassoc.md for architectural notes.

#include "rcc/all.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rcc {

namespace {

enum Family { FAdd, FMul, FAnd, FOr, FXor, FNone };

Family family_of(uint op) {
    switch (op) {
        case Oadd:
        case Osub:
            return FAdd;
        case Omul:
            return FMul;
        case Oand:
            return FAnd;
        case Oor:
            return FOr;
        case Oxor:
            return FXor;
        default:
            return FNone;
    }
}

// Binary op for the family.
int family_binop(Family f) {
    switch (f) {
        case FAdd:
            return Oadd;
        case FMul:
            return Omul;
        case FAnd:
            return Oand;
        case FOr:
            return Oor;
        case FXor:
            return Oxor;
        default:
            return Onop;
    }
}

struct Leaf {
    Ref ref;
    int sign;  // Only meaningful for FAdd, otherwise always +1
};

struct Reassoc {
    Ctx* ctx;
    Fn* fn;
    std::unordered_set<Ins*> absorbed;
    std::unordered_map<Ins*, std::vector<Ins>>
        prelude;  // root ins -> intermediate instructions in chain
    int n_trees = 0, n_folded = 0, n_addsub = 0, n_chainmax = 0;

    bool is_family_node(Ins* d, Family fam, int cls) {
        return d && family_of(d->op) == fam && static_cast<int>(d->cls) == cls;
    }

    int64_t mask_val(int cls, int64_t v) {
        return (cls == Kw) ? static_cast<int64_t>(static_cast<int32_t>(v)) : v;
    }

    // Flatten nodes.
    void collect(Ins* root, Family fam, int cls, std::vector<Leaf>& out) {
        std::vector<Leaf> stk;
        if (fam == FAdd) {
            stk.push_back({root->arg[0], +1});
            stk.push_back({root->arg[1], (root->op == Osub) ? -1 : +1});
        } else {
            stk.push_back({root->arg[0], +1});
            stk.push_back({root->arg[1], +1});
        }
        while (!stk.empty()) {
            Leaf cur = stk.back();
            stk.pop_back();
            if (rtype(cur.ref) == RTmp && cur.ref.val != 0) {
                Tmp* t = &fn->tmp[cur.ref.val];
                Ins* d = t->def;
                if (d && t->nuse == 1 && is_family_node(d, fam, cls) && !absorbed.count(d)) {
                    absorbed.insert(d);
                    if (fam == FAdd) {
                        stk.push_back({d->arg[0], cur.sign});
                        stk.push_back({d->arg[1], (d->op == Osub) ? -cur.sign : cur.sign});
                    } else {
                        stk.push_back({d->arg[0], cur.sign});
                        stk.push_back({d->arg[1], cur.sign});
                    }
                    continue;
                }
            }
            out.push_back(cur);
        }
    }

    // rank nodes.
    static uint64_t rank_of(Ref r) {
        if (rtype(r) == RCon)
            return (static_cast<uint64_t>(UINT32_MAX) << 32) | r.val;
        return (static_cast<uint64_t>(r.val) << 32) | r.val;
    }

    void sort_terms(std::vector<Leaf>& v) {
        std::sort(v.begin(), v.end(),
                  [](const Leaf& a, const Leaf& b) { return rank_of(a.ref) < rank_of(b.ref); });
    }

    // Build left-leaning chain.
    Ref build_full(const std::vector<Ref>& refs, int bop, int cls, std::vector<Ins>& pre) {
        Ref acc = refs[0];
        for (size_t k = 1; k < refs.size(); k++) {
            Ref t = newtmp(ctx, "rea", cls, fn);
            Ins ni;
            ni.op = static_cast<uint>(bop);
            ni.cls = static_cast<uint>(cls);
            ni.to = t;
            ni.arg[0] = acc;
            ni.arg[1] = refs[k];
            pre.push_back(ni);
            acc = t;
        }
        return acc;
    }

    void make_copy(Ins* root, Ref r) {
        root->op = Ocopy;
        root->arg[0] = r;
        root->arg[1] = Ref::none();
    }

    void make_bin(Ins* root, int op, Ref a0, Ref a1) {
        root->op = static_cast<uint>(op);
        root->arg[0] = a0;
        root->arg[1] = a1;
    }

    // Add/Sub Reconstruction
    void rebuild_addsub(Ins* root, int cls, std::vector<Leaf>& leaves) {
        // Constant folding.
        int64_t cacc = 0;
        std::vector<Leaf> terms;
        int nconst = 0;
        for (const Leaf& lf : leaves) {
            int64_t v;
            if (isconbits(fn, lf.ref, &v)) {
                cacc += lf.sign * v;
                nconst++;
            } else {
                terms.push_back(lf);
            }
        }
        cacc = mask_val(cls, cacc);
        if (nconst >= 2)
            n_folded++;

        sort_terms(terms);
        // Net sum of signs.
        std::vector<Leaf> canon;
        for (size_t k = 0; k < terms.size();) {
            size_t j = k;
            int64_t net = 0;
            while (j < terms.size() && req(terms[j].ref, terms[k].ref)) {
                net += terms[j].sign;
                j++;
            }
            int s = (net > 0) ? +1 : -1;
            for (int64_t c = 0; c < (net < 0 ? -net : net); c++)
                canon.push_back({terms[k].ref, s});
            k = j;
        }

        // Merge constants.
        std::vector<Ref> pos, neg;
        for (const Leaf& lf : canon) {
            if (lf.sign > 0)
                pos.push_back(lf.ref);
            else
                neg.push_back(lf.ref);
        }
        if (cacc != 0)
            pos.push_back(getcon(ctx, cacc, fn));

        n_addsub++;

        if (pos.empty() && neg.empty()) {
            make_copy(root, getcon(ctx, cacc, fn));  // cacc==0 here -> copy 0
            return;
        }
        std::vector<Ins>& pre = prelude[root];
        if (neg.empty()) {
            if (pos.size() == 1) {
                make_copy(root, pos[0]);
            } else {
                Ref acc = build_full(std::vector<Ref>(pos.begin(), pos.end() - 1), Oadd, cls, pre);
                make_bin(root, Oadd, acc, pos.back());
            }
        } else if (pos.empty()) {
            Ref negr = build_full(neg, Oadd, cls, pre);
            make_bin(root, Oneg, negr, Ref::none());
        } else {
            Ref posr = build_full(pos, Oadd, cls, pre);
            Ref negr = build_full(neg, Oadd, cls, pre);
            make_bin(root, Osub, posr, negr);
        }
        size_t len = pos.size() + neg.size();
        if (static_cast<int>(len) > n_chainmax)
            n_chainmax = static_cast<int>(len);
    }

    // Mul/And/Or/Xor Reconstruction
    void rebuild_logic(Ins* root, Family fam, int cls, std::vector<Leaf>& leaves) {
        int bop = family_binop(fam);
        int64_t mask = (cls == Kw) ? 0xffffffffLL : ~0LL;
        int64_t allones = mask;

        // Identity starting point
        int64_t acc;
        switch (fam) {
            case FMul:
                acc = 1;
                break;
            case FAnd:
                acc = allones;
                break;
            default:
                acc = 0;  // Or / Xor
                break;
        }
        int nconst = 0;
        std::vector<Leaf> terms;
        for (const Leaf& lf : leaves) {
            int64_t v;
            if (isconbits(fn, lf.ref, &v)) {
                nconst++;
                v &= mask;
                switch (fam) {
                    case FMul:
                        acc *= v;
                        break;
                    case FAnd:
                        acc &= v;
                        break;
                    case FOr:
                        acc |= v;
                        break;
                    default:
                        acc ^= v;
                        break;
                }
            } else {
                terms.push_back(lf);
            }
        }
        acc &= mask;
        if (nconst >= 2)
            n_folded++;

        // Short circuit.
        if ((fam == FMul && acc == 0 && nconst > 0) || (fam == FAnd && acc == 0 && nconst > 0)) {
            make_copy(root, getcon(ctx, 0, fn));
            return;
        }
        if (fam == FOr && acc == allones && nconst > 0) {
            make_copy(root, getcon(ctx, mask_val(cls, allones), fn));
            return;
        }

        sort_terms(terms);
        // Cancellation.
        std::vector<Ref> refs;
        if (fam == FAnd || fam == FOr) {
            for (size_t k = 0; k < terms.size(); k++)
                if (k == 0 || !req(terms[k].ref, terms[k - 1].ref))
                    refs.push_back(terms[k].ref);
        } else if (fam == FXor) {
            for (size_t k = 0; k < terms.size();) {
                size_t j = k;
                while (j < terms.size() && req(terms[j].ref, terms[k].ref))
                    j++;
                if ((j - k) & 1)
                    refs.push_back(terms[k].ref);
                k = j;
            }
        } else {
            for (const Leaf& lf : terms)
                refs.push_back(lf.ref);
        }

        // Whether to merge constant: only add if not identity.
        bool const_is_id = (fam == FMul && acc == 1) || (fam == FAnd && acc == allones) ||
                           ((fam == FOr || fam == FXor) && acc == 0);
        if (!const_is_id || refs.empty())
            refs.push_back(getcon(ctx, mask_val(cls, acc), fn));

        if (refs.size() == 1) {
            make_copy(root, refs[0]);
        } else {
            std::vector<Ins>& pre = prelude[root];
            Ref acc_ref = build_full(std::vector<Ref>(refs.begin(), refs.end() - 1), bop, cls, pre);
            make_bin(root, bop, acc_ref, refs.back());
        }
        if (static_cast<int>(refs.size()) > n_chainmax)
            n_chainmax = static_cast<int>(refs.size());
    }

    void process_root(Ins* root) {
        Family fam = family_of(root->op);
        int cls = static_cast<int>(root->cls);
        size_t before = absorbed.size();
        std::vector<Leaf> leaves;
        collect(root, fam, cls, leaves);
        size_t delta = absorbed.size() - before;

        // Act on trees.
        int nconst = 0;
        for (const Leaf& lf : leaves) {
            int64_t v;
            if (isconbits(fn, lf.ref, &v))
                nconst++;
        }
        if (delta == 0 && nconst < 2)
            return;

        n_trees++;
        if (fam == FAdd)
            rebuild_addsub(root, cls, leaves);
        else
            rebuild_logic(root, fam, cls, leaves);
    }

    void run() {
        // Collect root candidates.
        std::vector<Ins*> roots;
        for (Blk* b = fn->start; b; b = b->link) {
            for (uint k = 0; k < b->nins; k++) {
                Ins* i = &b->ins[k];
                if (rtype(i->to) != RTmp)
                    continue;
                Family fam = family_of(i->op);
                if (fam == FNone)
                    continue;
                if (KBASE(i->cls) != 0)
                    continue;  // integer only
                // Check root.
                Tmp* t = &fn->tmp[i->to.val];
                if (t->nuse == 1 && t->use && t->use[0].type == Use::UIns) {
                    Ins* parent = t->use[0].u.ins;
                    if (is_family_node(parent, fam, static_cast<int>(i->cls)))
                        continue;
                }
                roots.push_back(i);
            }
        }

        // Process each root.
        for (Ins* r : roots) {
            if (absorbed.count(r))
                continue;
            process_root(r);
        }

        // Mark absorbed intermediate nodes as Onop.
        for (Ins* a : absorbed)
            a->op = Onop;

        // Reconstruct.
        for (Blk* b = fn->start; b; b = b->link) {
            Ins* nins = static_cast<Ins*>(vnew(ctx, 0, sizeof(Ins), PFn));
            uint cnt = 0;
            for (uint k = 0; k < b->nins; k++) {
                Ins* i = &b->ins[k];
                if (i->op == Onop)
                    continue;
                auto it = prelude.find(i);
                if (it != prelude.end())
                    for (Ins& p : it->second)
                        addins(ctx, &nins, &cnt, &p);
                addins(ctx, &nins, &cnt, i);
            }
            b->ins = nins;
            b->nins = cnt;
        }

        if (debug['E']) {
            FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
            fprintf(out, "> reassoc %s: trees=%d folded=%d addsub=%d chain_max=%d\n", fn->name,
                    n_trees, n_folded, n_addsub, n_chainmax);
        }
    }
};

}  // namespace

void reassoc(Ctx* ctx, Fn* fn) {
    // Pre/post conditions.
    Reassoc r;
    r.ctx = ctx;
    r.fn = fn;
    r.run();
}

}  // namespace rcc
