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

/// @file scalar_repl.cc
/// @brief Converts in-loop memory accumulators into a register + phi node.
///
/// See docs/scalar_repl.md for architectural notes.

#include "rcc/all.h"

#include <cstring>
#include <vector>

namespace rcc {

namespace {

Ins make_nop_ins() {
    Ins i = {};
    i.op = Onop;
    return i;
}

struct LoopInfo {
    Blk* header;
    std::vector<Blk*> body;
    Blk* preheader;
    Blk* latch;
    Blk* exit_blk;
    Blk* exit_pred;  // exit_blk's only pred inside the body
};

bool blk_in(Blk* x, const std::vector<Blk*>& body) {
    for (Blk* b : body)
        if (b == x)
            return true;
    return false;
}

bool find_loops(Fn* fn, std::vector<LoopInfo>& out) {
    out.clear();
    for (Blk* b = fn->start; b; b = b->link)
        b->visit = ~0u;

    for (uint i = 0; i < fn->nblk; i++) {
        Blk* hd = fn->rpo[i];
        bool has_back = false;
        for (uint k = 0; k < hd->npred; k++)
            if (hd->pred[k]->id >= i) {
                has_back = true;
                break;
            }
        if (!has_back)
            continue;

        LoopInfo L{hd, {hd}, nullptr, nullptr, nullptr, nullptr};
        std::vector<Blk*> stack;
        for (uint k = 0; k < hd->npred; k++)
            if (hd->pred[k]->id >= i)
                stack.push_back(hd->pred[k]);
        while (!stack.empty()) {
            Blk* n = stack.back();
            stack.pop_back();
            if (n->id < hd->id || n->visit == hd->id)
                continue;
            n->visit = hd->id;
            L.body.push_back(n);
            for (uint k = 0; k < n->npred; k++)
                stack.push_back(n->pred[k]);
        }

        Blk* phdr = nullptr;
        Blk* latch = nullptr;
        bool unique = true;
        for (uint k = 0; k < hd->npred; k++) {
            Blk* p = hd->pred[k];
            if (blk_in(p, L.body)) {
                if (latch) {
                    unique = false;
                    break;
                }
                latch = p;
            } else {
                if (phdr) {
                    unique = false;
                    break;
                }
                phdr = p;
            }
        }
        if (!unique || !phdr || !latch)
            continue;
        L.preheader = phdr;
        L.latch = latch;

        Blk* exit_blk = nullptr;
        bool exit_unique = true;
        for (Blk* b : L.body) {
            Blk* succs[2] = {b->s1, b->s2};
            for (Blk* s : succs) {
                if (!s || blk_in(s, L.body))
                    continue;
                if (exit_blk && exit_blk != s) {
                    exit_unique = false;
                    break;
                }
                exit_blk = s;
            }
            if (!exit_unique)
                break;
        }
        if (!exit_unique || !exit_blk)
            continue;
        int from_loop = 0;
        Blk* exit_pred = nullptr;
        for (uint k = 0; k < exit_blk->npred; k++)
            if (blk_in(exit_blk->pred[k], L.body)) {
                from_loop++;
                exit_pred = exit_blk->pred[k];
            }
        if (from_loop != 1)
            continue;
        L.exit_blk = exit_blk;
        L.exit_pred = exit_pred;

        out.push_back(std::move(L));
    }
    return !out.empty();
}

// Recursively check address invariance.
bool is_pure_arith(int op) {
    switch (op) {
        case Oadd:
        case Osub:
        case Omul:
        case Oand:
        case Oor:
        case Oxor:
        case Oshl:
        case Oshr:
        case Osar:
        case Oextsw:
        case Oextuw:
        case Oextsh:
        case Oextuh:
        case Oextsb:
        case Oextub:
        case Ocopy:
            return true;
        default:
            return false;
    }
}

// Find phi node for t in a blk (t->def doesn't point to phi, must scan).
Phi* find_phi(Fn* fn, Ref r) {
    if (rtype(r) != RTmp)
        return nullptr;
    Tmp* t = &fn->tmp[r.val];
    if (t->bid == ~0u)
        return nullptr;
    Blk* b = fn->rpo[t->bid];
    for (Phi* p = b->phi; p; p = p->link)
        if (req(p->to, r))
            return p;
    return nullptr;
}

// Each incoming phi arg must be invariant. Cannot do optimistic cycle back-edge.
bool phi_invariant(Fn* fn, Phi* p, const LoopInfo& L) {
    for (uint k = 0; k < p->narg; k++) {
        Ref a = p->arg[k];
        if (req(a, p->to))
            continue;
        if (rtype(a) == RCon)
            continue;
        if (rtype(a) != RTmp)
            return false;
        Tmp* t = &fn->tmp[a.val];
        if (t->bid == ~0u)
            continue;
        if (blk_in(fn->rpo[t->bid], L.body))
            return false;
    }
    return true;
}

bool addr_invariant_rec(Fn* fn, Ref addr, const LoopInfo& L, int depth) {
    if (rtype(addr) == RCon)
        return true;
    if (rtype(addr) != RTmp)
        return false;
    Tmp* t = &fn->tmp[addr.val];
    if (t->bid == ~0u)
        return true;
    if (!blk_in(fn->rpo[t->bid], L.body))
        return true;
    if (depth <= 0)
        return false;
    if (Phi* p = find_phi(fn, addr))
        return phi_invariant(fn, p, L);
    Ins* d = t->def;
    if (!d || !is_pure_arith(static_cast<int>(d->op)))
        return false;
    if (!addr_invariant_rec(fn, d->arg[0], L, depth - 1))
        return false;
    if (!req(d->arg[1], Ref::none()) && !addr_invariant_rec(fn, d->arg[1], L, depth - 1))
        return false;
    return true;
}

bool addr_invariant(Fn* fn, Ref addr, const LoopInfo& L) {
    return addr_invariant_rec(fn, addr, L, 8);
}

// Structural equivalence: two Ref chains compute the same value.
bool addr_same_value(Fn* fn, Ref a, Ref b, int depth = 8) {
    if (req(a, b))
        return true;
    if (rtype(a) == RCon && rtype(b) == RCon) {
        Con* ca = &fn->con[a.val];
        Con* cb = &fn->con[b.val];
        if (ca->type != cb->type)
            return false;
        if (ca->type == Con::CAddr && !symeq(ca->sym, cb->sym))
            return false;
        return ca->bits.i == cb->bits.i;
    }
    if (rtype(a) != RTmp || rtype(b) != RTmp)
        return false;
    if (depth <= 0)
        return false;
    Tmp* ta = &fn->tmp[a.val];
    Tmp* tb = &fn->tmp[b.val];
    Ins* da = ta->def;
    Ins* db = tb->def;
    if (!da || !db)
        return false;
    if (da->op != db->op)
        return false;
    if (!is_pure_arith(static_cast<int>(da->op)))
        return false;
    if (!addr_same_value(fn, da->arg[0], db->arg[0], depth - 1))
        return false;
    if (req(da->arg[1], Ref::none()) && req(db->arg[1], Ref::none()))
        return true;
    return addr_same_value(fn, da->arg[1], db->arg[1], depth - 1);
}

// Clone invariant address chain to preheader.
Ref clone_invariant_to_preheader(Ctx* ctx, Fn* fn, Ref r, const LoopInfo& L,
                                 std::vector<std::pair<uint, Ref>>& cache, int depth = 8) {
    if (rtype(r) == RCon)
        return r;
    if (rtype(r) != RTmp)
        return Ref::none();
    Tmp* t = &fn->tmp[r.val];
    if (t->bid == ~0u || !blk_in(fn->rpo[t->bid], L.body))
        return r;  // Already defined outside loop, reuse directly
    if (depth <= 0)
        return Ref::none();
    for (auto& kv : cache)
        if (kv.first == r.val)
            return kv.second;

    Phi* p = find_phi(fn, r);
    if (p) {
        // Look for preheader-side arg in phi.blk[]
        for (uint k = 0; k < p->narg; k++)
            if (p->blk[k] == L.preheader) {
                Ref v = clone_invariant_to_preheader(ctx, fn, p->arg[k], L, cache, depth - 1);
                if (rtype(v) == -1)
                    return Ref::none();
                cache.push_back(std::make_pair(static_cast<uint>(r.val), v));
                return v;
            }
        return Ref::none();
    }
    Ins* d = t->def;
    if (!d || !is_pure_arith(static_cast<int>(d->op)))
        return Ref::none();
    Ref a0 = clone_invariant_to_preheader(ctx, fn, d->arg[0], L, cache, depth - 1);
    if (rtype(a0) == -1)
        return Ref::none();
    Ref a1 = Ref::none();
    if (!req(d->arg[1], Ref::none())) {
        a1 = clone_invariant_to_preheader(ctx, fn, d->arg[1], L, cache, depth - 1);
        if (rtype(a1) == -1)
            return Ref::none();
    }
    Ref nt = newtmp(ctx, "sr.addr", static_cast<int>(d->cls), fn);
    Ins ni{};
    ni.op = d->op;
    ni.cls = d->cls;
    ni.to = nt;
    ni.arg[0] = a0;
    ni.arg[1] = a1;
    addins(ctx, &L.preheader->ins, &L.preheader->nins, &ni);
    cache.push_back(std::make_pair(static_cast<uint>(r.val), nt));
    return nt;
}

// Globally replace all ref==from with to (in phi.arg / ins.arg / jmp.arg / jmp.arg2).
void rewrite_ref(Fn* fn, Ref from, Ref to) {
    for (Blk* b = fn->start; b; b = b->link) {
        for (Phi* p = b->phi; p; p = p->link)
            for (uint k = 0; k < p->narg; k++)
                if (req(p->arg[k], from))
                    p->arg[k] = to;
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
            if (req(i->arg[0], from))
                i->arg[0] = to;
            if (req(i->arg[1], from))
                i->arg[1] = to;
        }
        if (req(b->jmp.arg, from))
            b->jmp.arg = to;
        if (req(b->jmp.arg2, from))
            b->jmp.arg2 = to;
    }
}

void prepend_ins(Ctx* ctx, Blk* b, const Ins& ins) {
    vgrow(ctx, &b->ins, b->nins + 1);
    std::memmove(b->ins + 1, b->ins, b->nins * sizeof(Ins));
    b->ins[0] = ins;
    b->nins += 1;
}

// Try one promotion on L. Returns true on hit; outer loop recomputes use/dom/alias.
bool try_promote(Ctx* ctx, Fn* fn, const LoopInfo& L) {
    for (Blk* b : L.body) {
        for (Ins* iload = b->ins; iload < &b->ins[b->nins]; iload++) {
            if (!isload(iload->op))
                continue;
            Ref addr = iload->arg[0];
            if (!addr_invariant(fn, addr, L))
                continue;
            int sz = loadsz(iload);

            std::vector<Ins*> matched_loads;
            Blk* store_blk = nullptr;
            Ins* matched_store = nullptr;
            int store_count = 0;
            bool ok = true;

            // Three-tier classification: structurally identical -> matched, NoAlias -> skip,
            // MayAlias -> bail.
            auto classify = [&](Ref jaddr, int jsz) -> int {
                // ret: 0=matched, 1=skip(NoAlias), 2=bail
                if (jsz != sz)
                    return 2;  // Different size cannot be promoted (conservative)
                if (addr_same_value(fn, addr, jaddr))
                    return 0;
                int delta;
                int r = alias(addr, 0, sz, jaddr, jsz, &delta, fn);
                if (r == NoAlias)
                    return 1;
                if (r == MustAlias && delta == 0)
                    return 0;
                return 2;
            };

            for (Blk* bb : L.body) {
                for (Ins* j = bb->ins; j < &bb->ins[bb->nins]; j++) {
                    if (j->op == Ocall || j->op == Oblit0 || j->op == Oblit1) {
                        ok = false;
                        break;
                    }
                    if (isload(j->op)) {
                        int jsz = loadsz(j);
                        // Different size but NoAlias can still be skipped
                        if (jsz != sz) {
                            int delta;
                            int r = alias(addr, 0, sz, j->arg[0], jsz, &delta, fn);
                            if (r != NoAlias) {
                                ok = false;
                                break;
                            }
                            continue;
                        }
                        int c = classify(j->arg[0], jsz);
                        if (c == 0)
                            matched_loads.push_back(j);
                        else if (c == 2) {
                            ok = false;
                            break;
                        }
                    } else if (isstore(j->op)) {
                        int jsz = storesz(j);
                        if (jsz != sz) {
                            int delta;
                            int r = alias(addr, 0, sz, j->arg[1], jsz, &delta, fn);
                            if (r != NoAlias) {
                                ok = false;
                                break;
                            }
                            continue;
                        }
                        int c = classify(j->arg[1], jsz);
                        if (c == 0) {
                            store_count++;
                            if (store_count > 1) {
                                ok = false;
                                break;
                            }
                            matched_store = j;
                            store_blk = bb;
                        } else if (c == 2) {
                            ok = false;
                            break;
                        }
                    }
                }
                if (!ok)
                    break;
            }
            if (!ok || store_count != 1)
                continue;
            // Store must be must-execute.
            if (!dom(store_blk, L.latch))
                continue;
            if (!dom(store_blk, L.exit_pred))
                continue;

            int cls = static_cast<int>(iload->cls);

            // Clone address chain to preheader, then issue load.
            std::vector<std::pair<uint, Ref>> addr_cache;
            Ref pre_addr = clone_invariant_to_preheader(ctx, fn, addr, L, addr_cache);
            if (rtype(pre_addr) == -1)
                continue;  // Clone failed (rare corner case)
            Ref init_ref = newtmp(ctx, "sr.init", cls, fn);
            Ins pre_ld{};
            pre_ld.op = iload->op;
            pre_ld.cls = static_cast<uint>(cls);
            pre_ld.to = init_ref;
            pre_ld.arg[0] = pre_addr;
            pre_ld.arg[1] = Ref::none();
            addins(ctx, &L.preheader->ins, &L.preheader->nins, &pre_ld);

            // Insert phi at header top.
            Ref back_val = matched_store->arg[0];
            Ref phi_ref = newtmp(ctx, "sr.phi", cls, fn);
            Phi* p = static_cast<Phi*>(alloc(ctx, sizeof(Phi)));
            std::memset(p, 0, sizeof *p);
            p->to = phi_ref;
            p->cls = static_cast<short>(cls);
            p->narg = 2;
            p->arg = static_cast<Ref*>(vnew(ctx, 2, sizeof(Ref), PFn));
            p->blk = static_cast<Blk**>(vnew(ctx, 2, sizeof(Blk*), PFn));
            p->arg[0] = init_ref;
            p->blk[0] = L.preheader;
            p->arg[1] = back_val;
            p->blk[1] = L.latch;
            p->link = L.header->phi;
            L.header->phi = p;

            // Change all matched_loads def tmps to phi_ref; original loads become Onop.
            for (Ins* ml : matched_loads) {
                rewrite_ref(fn, ml->to, phi_ref);
                *ml = make_nop_ins();
            }

            // Sink store at top of exit_blk.
            Ins sink{};
            sink.op = matched_store->op;
            sink.cls = matched_store->cls;
            sink.to = Ref::none();
            sink.arg[0] = phi_ref;
            sink.arg[1] = pre_addr;
            prepend_ins(ctx, L.exit_blk, sink);

            // Original store becomes Onop.
            *matched_store = make_nop_ins();

            return true;
        }
    }
    return false;
}

}  // namespace

void scalar_repl(Ctx* ctx, Fn* fn) {
    int total = 0;
    for (int iter = 0; iter < 16; iter++) {
        std::vector<LoopInfo> loops;
        if (!find_loops(fn, loops))
            break;
        bool changed = false;
        for (const LoopInfo& L : loops) {
            if (try_promote(ctx, fn, L)) {
                changed = true;
                total++;
                break;
            }
        }
        if (!changed)
            break;
        filluse(ctx, fn);
        filldom(fn);
        fillalias(fn);
    }
    if (debug['L']) {
        FILE* out = ctx->dbg_buf ? ctx->dbg_buf : stderr;
        fprintf(out, "\n> scalar-repl promoted %d location(s) in %s\n", total, fn->name);
    }
}

}  // namespace rcc
