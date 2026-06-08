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

/// @file backend/spill.cc
/// @brief Register spilling pass.

#include "rcc/all.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <vector>

namespace rcc {

// fillcost

namespace {

// loopiter callback: aggregate gen/nlive of loop body to loop header.
void aggreg(Blk* hd, Blk* b) {
    bsunion(hd->gen, b->gen);
    for (int k = 0; k < 2; ++k)
        if (b->nlive[k] > hd->nlive[k])
            hd->nlive[k] = b->nlive[k];
}

// Single Ref use accumulation.
void tmpuse(Ref r, int use, int loop, Fn* fn) {
    if (rtype(r) == RMem) {
        Mem* m = &fn->mem[r.val];
        tmpuse(m->base, 1, loop, fn);
        tmpuse(m->index, 1, loop, fn);
        return;
    }
    if (rtype(r) == RTmp && r.val >= Tmp0) {
        Tmp* t = &fn->tmp[r.val];
        t->nuse += static_cast<uint>(use);
        t->ndef += static_cast<uint>(!use);
        t->cost += static_cast<uint>(loop);
    }
}

void accumulate_phi_costs(Blk* b, Fn* fn) {
    for (Phi* p = b->phi; p; p = p->link) {
        Tmp* t = &fn->tmp[p->to.val];
        tmpuse(p->to, 0, 0, fn);
        for (uint a = 0; a < p->narg; ++a) {
            int n = p->blk[a]->loop;
            t->cost += static_cast<uint>(n);
            tmpuse(p->arg[a], 1, n, fn);
        }
    }
}

void accumulate_ins_costs(Blk* b, Fn* fn) {
    int n = b->loop;
    for (Ins* i = b->ins; i < &b->ins[b->nins]; ++i) {
        tmpuse(i->to, 0, n, fn);
        tmpuse(i->arg[0], 1, n, fn);
        tmpuse(i->arg[1], 1, n, fn);
    }
    tmpuse(b->jmp.arg, 1, n, fn);
    tmpuse(b->jmp.arg2, 1, n, fn);  // Second operand for Jjfi/Jjff.
}

void dump_loop_info(Ctx* ctx, Fn* fn) {
    FILE* f = ctx->dbg_buf;
    fprintf(f, "\n> Loop information:\n");
    for (Blk* b = fn->start; b; b = b->link) {
        uint a;
        for (a = 0; a < b->npred; ++a)
            if (b->id <= b->pred[a]->id)
                break;
        if (a != b->npred) {
            fprintf(f, "\t%-10s", b->name);
            fprintf(f, " (% 3d ", b->nlive[0]);
            fprintf(f, "% 3d) ", b->nlive[1]);
            dumpts(b->gen, fn->tmp, f);
        }
    }
}

}  // namespace

void fillcost(Ctx* ctx, Fn* fn) {
    loopiter(fn, aggreg);

    if (debug['S'])
        dump_loop_info(ctx, fn);

    // Register cost = UINT_MAX (never spill); normal tmp accumulates from 0.
    for (Tmp* t = fn->tmp; t - fn->tmp < fn->ntmp; ++t) {
        t->cost = (t - fn->tmp < Tmp0) ? UINT_MAX : 0;
        t->nuse = 0;
        t->ndef = 0;
    }

    for (Blk* b = fn->start; b; b = b->link) {
        accumulate_phi_costs(b, fn);
        accumulate_ins_costs(b, fn);
    }

    if (debug['S']) {
        FILE* f = ctx->dbg_buf;
        fprintf(f, "\n> Spill costs:\n");
        for (int n = Tmp0; n < fn->ntmp; ++n)
            fprintf(f, "\t%-10s %d\n", fn->tmp[n].name, fn->tmp[n].cost);
        fprintf(f, "\n");
    }
}

// spill

namespace {

// Per-call state.
struct SpillCtx {
    Ctx* ctx;  // Back reference to ctx.
    Tmp* tmp;  // Cache of fn->tmp.
    int ntmp;
    int locs;               // fn->slot entrance snapshot.
    int slot4;              // Next 4B slot.
    int slot8;              // Next 8B slot.
    BSet mask[2];           // GPR and FPR bitsets.
    BSet b2_scratch;        // Scratch buffer for limit2.
    std::vector<int> tarr;  // Scratch buffer for limit.
};

// Allocate stack slot on demand.
Ref alloc_spill_slot(SpillCtx& s, int t) {
    assert(t >= Tmp0 && "cannot spill register");
    int existing = s.tmp[t].slot;
    if (existing != -1)
        return Ref::slot(existing);

    int pos;
    if (KWIDE(s.tmp[t].cls)) {
        pos = s.slot8;
        if (s.slot4 == s.slot8)
            s.slot4 += 2;
        s.slot8 += 2;
    } else {
        pos = s.slot4;
        if (s.slot4 == s.slot8) {
            s.slot8 += 2;
            s.slot4 += 1;
        } else {
            // 4B hole filled, jump to slot8.
            s.slot4 = s.slot8;
        }
    }
    pos += s.locs;
    s.tmp[t].slot = pos;
    return Ref::slot(pos);
}

// Limit bitset to at most k elements.
void limit_one(SpillCtx& s, BSet* b, int k, BSet* f) {
    int nt = static_cast<int>(bscount(b));
    if (nt <= k)
        return;

    s.tarr.resize(static_cast<size_t>(nt));

    // Pull elements from b to tarr, clear b.
    int collected = 0;
    int t = 0;
    while (bsiter(b, &t)) {
        bsclr(b, static_cast<uint>(t));
        s.tarr[static_cast<size_t>(collected++)] = t;
        ++t;
    }
    assert(collected == nt);

    if (nt > 1) {
        Tmp* tmp = s.tmp;
        if (f) {
            BSet* fst = f;
            std::stable_sort(s.tarr.begin(), s.tarr.begin() + nt, [fst, tmp](int x, int y) {
                int fx = bshas(fst, static_cast<uint>(x));
                int fy = bshas(fst, static_cast<uint>(y));
                if (fx != fy)
                    return fx > fy;
                return tmp[x].cost > tmp[y].cost;
            });
        } else {
            std::stable_sort(s.tarr.begin(), s.tarr.begin() + nt,
                             [tmp](int x, int y) { return tmp[x].cost > tmp[y].cost; });
        }
    }

    int i = 0;
    for (; i < k && i < nt; ++i)
        bsset(b, static_cast<uint>(s.tarr[static_cast<size_t>(i)]));
    for (; i < nt; ++i)
        alloc_spill_slot(s, s.tarr[static_cast<size_t>(i)]);
}

// Limit GPR/FPR separately.
void limit_two(SpillCtx& s, BSet* b1, int k1, int k2, BSet* f) {
    BSet* b2 = &s.b2_scratch;
    bscopy(b2, b1);
    bsinter(b1, &s.mask[0]);
    bsinter(b2, &s.mask[1]);
    limit_one(s, b1, T.ngpr - k1, f);
    {
        int cnt = static_cast<int>(bscount(b2));
        if (cnt > s.ctx->spill_fp_live_peak)
            s.ctx->spill_fp_live_peak = cnt;
    }
    limit_one(s, b2, T.nfpr - k2, f);
    bsunion(b1, b2);
}

// Set register hint for tmp in u.
void sethint(SpillCtx& s, BSet* u, bits r) {
    int t = Tmp0;
    while (bsiter(u, &t)) {
        s.tmp[phicls(t, s.tmp)].hint.m |= r;
        ++t;
    }
}

// Emit Oload for tmp in u \ v.
void reloads(Ctx* ctx, SpillCtx& s, BSet* u, BSet* v) {
    int t = Tmp0;
    while (bsiter(u, &t)) {
        if (!bshas(v, static_cast<uint>(t)))
            emit(ctx, Oload, s.tmp[t].cls, Ref::tmp(static_cast<uint>(t)), alloc_spill_slot(s, t),
                 Ref::none());
        ++t;
    }
}

// Emit Ostore to SLOT(s) if s != -1.
void store_to_slot(Ctx* ctx, SpillCtx& spc, Ref r, int slot_id) {
    if (slot_id == -1)
        return;
    int cls = spc.tmp[r.val].cls;
    emit(ctx, Ostorew + cls, 0, Ref::none(), r, Ref::slot(slot_id));
}

inline bool is_regcopy(Ins* i) {
    return i->op == Ocopy && isreg(i->arg[0]);
}

// Process parallel moves (contiguous reg->tmp copies).
Ins* spill_par_moves(Ctx* ctx, SpillCtx& s, Blk* b, Ins* i, BSet* v) {
    BSet u[1];
    bsinit(ctx, u, static_cast<uint>(s.ntmp));

    // Backward scan of contiguous regcpy segment.
    Ins* i1 = ++i;
    do {
        --i;
        int t = i->to.val;
        if (!req(i->to, Ref::none()))
            if (bshas(v, static_cast<uint>(t))) {
                bsclr(v, static_cast<uint>(t));
                store_to_slot(ctx, s, i->to, s.tmp[t].slot);
            }
        bsset(v, i->arg[0].val);
    } while (i != b->ins && is_regcopy(i - 1));

    bscopy(u, v);

    bits r;
    if (i != b->ins && (i - 1)->op == Ocall) {
        // Call site: return registers are dead, argregs must be preserved.
        Ins* call = i - 1;
        v->t[0] &= ~T.retregs(call->arg[1], nullptr);
        limit_two(s, v, T.nrsave[0], T.nrsave[1], nullptr);
        r = 0;
        for (int n = 0; T.rsave[n] >= 0; ++n)
            r |= BIT(T.rsave[n]);
        v->t[0] |= T.argregs(call->arg[1], nullptr);
    } else {
        limit_two(s, v, 0, 0, nullptr);
        r = v->t[0];
    }
    sethint(s, v, r);
    reloads(ctx, s, u, v);

    // Re-emit original regcpy in the segment.
    do
        emiti(ctx, *--i1);
    while (i1 != i);
    return i;
}

// Loop header path merge.
void merge_pred(SpillCtx& s, BSet* u, Blk* bu, BSet* v, Blk* bv) {
    if (bu->loop <= bv->loop) {
        bsunion(u, v);
        return;
    }
    int t = 0;
    while (bsiter(v, &t)) {
        if (s.tmp[t].slot == -1)
            bsset(u, static_cast<uint>(t));
        ++t;
    }
}

// Compute exit set v.
void compute_exit_set(SpillCtx& s, Blk* b, BSet* u, BSet* v, BSet* w) {
    Blk* s1 = b->s1;
    Blk* s2 = b->s2;
    Blk* hd = nullptr;
    if (s1 && s1->id <= b->id)
        hd = s1;
    if (s2 && s2->id <= b->id)
        if (!hd || s2->id >= hd->id)
            hd = s2;

    if (hd) {
        // Back-edge handling.
        bszero(v);
        hd->gen->t[0] |= T.rglob;  // Global registers never spill.
        for (int k = 0; k < 2; ++k) {
            uint n = (k == 0) ? static_cast<uint>(T.ngpr) : static_cast<uint>(T.nfpr);
            bscopy(u, b->out);
            bsinter(u, &s.mask[k]);
            bscopy(w, u);
            bsinter(u, hd->gen);
            bsdiff(w, hd->gen);
            if (bscount(u) < n) {
                int j = static_cast<int>(bscount(w));  // Live-through.
                int l = hd->nlive[k];
                if (k == 1 && static_cast<int>(bscount(w)) > s.ctx->spill_fp_live_peak)
                    s.ctx->spill_fp_live_peak = static_cast<int>(bscount(w));
                limit_one(s, w, static_cast<int>(n) - (l - j), nullptr);
                bsunion(u, w);
            } else {
                if (k == 1 && static_cast<int>(bscount(u)) > s.ctx->spill_fp_live_peak)
                    s.ctx->spill_fp_live_peak = static_cast<int>(bscount(u));
                limit_one(s, u, static_cast<int>(n), nullptr);
            }
            bsunion(v, u);
        }
    } else if (s1) {
        // Forward edge handling.
        bszero(v);
        liveon(w, b, s1);
        merge_pred(s, v, b, w, s1);
        if (s2) {
            liveon(u, b, s2);
            merge_pred(s, v, b, u, s2);
            bsinter(w, u);
        }
        limit_two(s, v, 0, 0, w);
    } else {
        bscopy(v, b->out);
        if (rtype(b->jmp.arg) == RCall)
            v->t[0] |= T.retregs(b->jmp.arg, nullptr);
    }
}

// Single instruction processing in main loop.

// Process i->to.
void process_def(Ins* i, BSet* v, BSet* w) {
    if (req(i->to, Ref::none()))
        return;
    assert(rtype(i->to) == RTmp);
    uint t = i->to.val;
    if (bshas(v, t)) {
        bsclr(v, t);
    } else {
        // Dead def.
        assert(t >= Tmp0 && "dead reg");
        bsset(v, t);
        bsset(w, t);
    }
}

// Process args.
void process_args(Ins* i, BSet* v, BSet* w, int lvarg[2], Fn* fn) {
    int memquota = T.memargs(static_cast<int>(i->op));
    for (int n = 0; n < 2; ++n)
        if (rtype(i->arg[n]) == RMem)
            --memquota;

    for (int n = 0; n < 2; ++n) {
        switch (rtype(i->arg[n])) {
            case RMem: {
                uint t = i->arg[n].val;
                Mem* m = &fn->mem[t];
                if (rtype(m->base) == RTmp) {
                    bsset(v, m->base.val);
                    bsset(w, m->base.val);
                }
                if (rtype(m->index) == RTmp) {
                    bsset(v, m->index.val);
                    bsset(w, m->index.val);
                }
                break;
            }
            case RTmp: {
                uint t = i->arg[n].val;
                lvarg[n] = bshas(v, t);
                bsset(v, t);
                if (memquota-- <= 0)
                    bsset(w, t);
                break;
            }
            default:
                break;
        }
    }
}

// Rewrite evicted args after limit2.
void rewrite_evicted_args(SpillCtx& s, Ins* i, BSet* u, BSet* v, const int lvarg[2]) {
    for (int n = 0; n < 2; ++n) {
        if (rtype(i->arg[n]) != RTmp)
            continue;
        uint t = i->arg[n].val;
        if (!bshas(v, t)) {
            if (!lvarg[n])
                bsclr(u, t);
            i->arg[n] = alloc_spill_slot(s, static_cast<int>(t));
        }
    }
}

// Process jmp args.
void process_jmp_arg(SpillCtx& s, Blk* b, BSet* v) {
    bool has1 = rtype(b->jmp.arg) == RTmp;
    bool has2 = rtype(b->jmp.arg2) == RTmp;
    if (!has1 && !has2)
        return;
    int t1 = has1 ? static_cast<int>(b->jmp.arg.val) : -1;
    int t2 = has2 ? static_cast<int>(b->jmp.arg2.val) : -1;
    if (has1) {
        assert(KBASE(s.tmp[t1].cls) == 0);
        bsset(v, static_cast<uint>(t1));
    }
    if (has2) {
        assert(KBASE(s.tmp[t2].cls) == 0);
        bsset(v, static_cast<uint>(t2));
    }
    limit_two(s, v, 0, 0, nullptr);
    if (has1 && !bshas(v, static_cast<uint>(t1)))
        b->jmp.arg = alloc_spill_slot(s, t1);
    if (has2 && !bshas(v, static_cast<uint>(t2)))
        b->jmp.arg2 = alloc_spill_slot(s, t2);
}

// Slot unselected outs.
void slot_unselected_outs(SpillCtx& s, Blk* b, BSet* v) {
    int t = Tmp0;
    while (bsiter(b->out, &t)) {
        if (!bshas(v, static_cast<uint>(t)))
            alloc_spill_slot(s, t);
        ++t;
    }
}

// Process block phis.
void process_block_phis(Ctx* ctx, SpillCtx& s, Blk* b, BSet* v) {
    for (Phi* p = b->phi; p; p = p->link) {
        assert(rtype(p->to) == RTmp);
        uint t = p->to.val;
        if (bshas(v, t)) {
            bsclr(v, t);
            store_to_slot(ctx, s, p->to, s.tmp[t].slot);
        } else if (bshas(b->in, t)) {
            p->to = alloc_spill_slot(s, static_cast<int>(t));
        }
    }
}

void dump_spill_result(Ctx* ctx, Fn* fn) {
    FILE* f = ctx->dbg_buf;
    fprintf(f, "\n> Block information:\n");
    for (Blk* b = fn->start; b; b = b->link) {
        fprintf(f, "\t%-10s (% 5d) ", b->name, b->loop);
        dumpts(b->out, fn->tmp, f);
    }
    fprintf(f, "\n> After spilling:\n");
    printfn(fn, f);
}

}  // namespace

// spill main entry.

void spill(Ctx* ctx, Fn* fn) {
    SpillCtx s{};
    s.ctx = ctx;
    s.tmp = fn->tmp;
    s.ntmp = fn->ntmp;
    s.locs = fn->slot;
    s.slot4 = 0;
    s.slot8 = 0;

    uint n = static_cast<uint>(s.ntmp);
    bsinit(ctx, &s.mask[0], n);
    bsinit(ctx, &s.mask[1], n);
    bsinit(ctx, &s.b2_scratch, n);

    BSet u[1], v[1], w[1];
    bsinit(ctx, u, n);
    bsinit(ctx, v, n);
    bsinit(ctx, w, n);

    // Mask initialization.
    for (int t = 0; t < s.ntmp; ++t) {
        int k = 0;
        if (t >= T.fpr0 && t < T.fpr0 + T.nfpr)
            k = 1;
        if (t >= Tmp0)
            k = KBASE(s.tmp[t].cls);
        bsset(&s.mask[k], static_cast<uint>(t));
    }

    // RPO reverse order iteration.
    for (Blk** bp = &fn->rpo[fn->nblk]; bp != fn->rpo;) {
        Blk* b = *--bp;

        // Determine exit set v.
        ctx->curi = nullptr;  // emit/reloads disabled here.
        compute_exit_set(s, b, u, v, w);
        process_jmp_arg(s, b, v);
        slot_unselected_outs(s, b, v);
        bscopy(b->out, v);

        // Reverse process instructions, fill insb.
        ctx->curi = ctx->insb + NIns;
        for (Ins* i = &b->ins[b->nins]; i != b->ins;) {
            --i;
            if (is_regcopy(i)) {
                i = spill_par_moves(ctx, s, b, i, v);
                continue;
            }

            bszero(w);
            int lvarg[2] = {0, 0};
            process_def(i, v, w);
            process_args(i, v, w, lvarg, fn);

            bscopy(u, v);
            limit_two(s, v, 0, 0, w);
            rewrite_evicted_args(s, i, u, v, lvarg);
            reloads(ctx, s, u, v);

            if (!req(i->to, Ref::none())) {
                uint t = i->to.val;
                store_to_slot(ctx, s, i->to, s.tmp[t].slot);
                if (t >= Tmp0)
                    bsclr(v, t);
            }
            emiti(ctx, *i);

            bits r = v->t[0];  // Low 64 bits are physical registers.
            if (r)
                sethint(s, v, r);
        }

        // Entry handling.
        if (b == fn->start) {
            assert(v->t[0] == (T.rglob | fn->reg));
        } else {
            assert(v->t[0] == T.rglob);
        }

        // Block entry phis.
        process_block_phis(ctx, s, b, v);
        bscopy(b->in, v);

        idup(ctx, b, ctx->curi, static_cast<ulong>(ctx->insb + NIns - ctx->curi));
    }

    // 16-byte alignment.
    s.slot8 += s.slot8 & 3;
    fn->slot += s.slot8;

    if (debug['S'])
        dump_spill_result(ctx, fn);
}

}  // namespace rcc
