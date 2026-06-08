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

/// @file mem.cc
/// @brief Memory optimizations: `promote` and `coalesce`.
///
/// See docs/mem.md for architectural notes.

#include "rcc/all.h"

#include <climits>
#include <cstdlib>
#include <cstring>

namespace rcc {

// promote slots.

namespace {

/// Describes the result of a promote compatibility scan.
struct UniformFit {
    int sz;  // Shared access width; -1 means "undetermined" (only in all-load state)
    int k;   // Shared store cls; -1 means "no store" (pure read slot, dies later)
};

/// Scan all uses of alloc tmp to check if it can be promoted. Returns true and fills *fit on
/// success.
bool check_uniform(Tmp* t, Ref alloc_ref, UniformFit* fit) {
    int s = -1, k = -1;
    for (Use* u = t->use; u < &t->use[t->nuse]; u++) {
        if (u->type != Use::UIns)
            return false;
        Ins* l = u->u.ins;

        if (isload(l->op)) {
            int ls = loadsz(l);
            if (s != -1 && s != ls)
                return false;
            s = ls;
            continue;
        }

        if (isstore(l->op)) {
            // Must be store(value, addr=alloc): addr in arg[1], value in arg[0]
            if (!req(alloc_ref, l->arg[1]) || req(alloc_ref, l->arg[0]))
                return false;
            int ss = storesz(l);
            if (s != -1 && s != ss)
                return false;
            int sk = optab[l->op].argcls[0][0];
            if (k != -1 && k != sk)
                return false;
            s = ss;
            k = sk;
            continue;
        }

        return false;  // Non load/store use (addr passed out, compared, etc.)
    }
    fit->sz = s;
    fit->k = k;
    return true;
}

inline Ins make_nop() {
    Ins n{};
    n.op = Onop;
    return n;
}

/// Rewrite uses to copy / cast / ext (see docs/mem.md).
void rewrite_uses(Tmp* t, int k, Fn* fn) {
    Use* ue = &t->use[t->nuse];
    for (Use* u = t->use; u != ue; u++) {
        Ins* l = u->u.ins;

        if (isstore(l->op)) {
            // store value, addr  →  copy value to addr-as-def
            l->cls = static_cast<uint>(k);
            l->op = Ocopy;
            l->to = l->arg[1];
            l->arg[1] = Ref::none();
            t->nuse--;
            t->ndef++;
            continue;
        }

        // Check lock.
        if (k == -1)
            die("promote: slot %%%s is read but never stored to", fn->tmp[l->arg[0].val].name);

        bool extend = false;
        switch (l->op) {
            case Oloadsw:
            case Oloaduw:
                if (k == Kl) {
                    extend = true;
                    break;
                }
                [[fallthrough]];
            case Oload:
                if (KBASE(k) != KBASE(static_cast<int>(l->cls)))
                    l->op = Ocast;
                else
                    l->op = Ocopy;
                break;
            default:
                extend = true;
                break;
        }
        if (extend)
            l->op = static_cast<uint>(Oextsb + (static_cast<int>(l->op) - Oloadsb));
    }
}

}  // namespace

void promote(Ctx* ctx, Fn* fn) {
    Blk* b = fn->start;
    for (Ins* i = b->ins; i < &b->ins[b->nins]; i++) {
        if (i->op < Oalloc || i->op > Oalloc1)
            continue;
        // Assumption NAlign == 3.
        assert(rtype(i->to) == RTmp);
        Tmp* t = &fn->tmp[i->to.val];
        if (t->ndef != 1)
            continue;

        UniformFit fit;
        if (!check_uniform(t, i->to, &fit))
            continue;

        // Kill alloc, rewrite all uses.
        *i = make_nop();
        t->ndef--;
        rewrite_uses(t, fit.k, fn);
    }

    if (debug['M']) {
        fprintf(ctx->dbg_buf, "\n> After slot promotion:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

// coalesce slots.

namespace {

/// Half-open interval [a, b). b == 0 means "empty interval" (slot not yet radd'ed).
struct Range {
    int a, b;
};

/// store-record: a store instruction recorded on a slot + its ip.
struct Store {
    int ip;
    Ins* i;
};

struct Slot {
    int t;      // corresponding tmp number
    int sz;     // alloc size (bytes)
    bits m;     // accumulated write mask for this slot (from fillalias)
    bits l;     // read mask accumulated during reverse block traversal
    Range r;    // live range (covers all active ips)
    Slot* s;    // fused representative (s == this means self is rep); nullptr if not fused
    Store* st;  // array of store records on this slot (PHeap-vnew)
    int nst;
};

inline bool rin(Range r, int n) {
    return r.a <= n && n < r.b;
}

inline bool rovlap(Range r0, Range r1) {
    // Overlap check.
    return r0.b && r1.b && r0.a < r1.b && r1.a < r0.b;
}

void radd(Range* r, int n) {
    if (!r->b) {
        r->a = n;
        r->b = n + 1;
    } else if (n < r->a) {
        r->a = n;
    } else if (n >= r->b) {
        r->b = n + 1;
    }
}

/// Translate ref r to its slot (if any) and offset. Returns false if r does not point to a coalesce
/// candidate.
bool slot_of(Slot** ps, int64_t* off, Ref r, Fn* fn, Slot* sl) {
    Alias a;
    getalias(&a, r, fn);
    if (a.type != Alias::ALoc)
        return false;
    Tmp* t = &fn->tmp[a.base];
    if (t->visit < 0)
        return false;  // tmp not in sl[]
    *off = a.offset;
    *ps = &sl[t->visit];
    return true;
}

/// Encountered a read of x high-order bytes from r during reverse RPO.
void load(Ref r, bits x, int ip, Fn* fn, Slot* sl) {
    int64_t off;
    Slot* s;
    if (!slot_of(&s, &off, r, fn, sl))
        return;
    s->l |= x << off;
    s->l &= s->m;
    if (s->l)
        radd(&s->r, ip);
}

/// Encountered a write of x bytes to r during reverse RPO.
/// Encountered a write.
void store(Ref r, bits x, int ip, Ins* i, Ctx* ctx, Fn* fn, Slot* sl) {
    int64_t off;
    Slot* s;
    if (!slot_of(&s, &off, r, fn, sl))
        return;
    if (s->l) {
        // Currently has live bytes: store is valid, advance live range.
        radd(&s->r, ip);
        s->l &= ~(x << off);
    } else {
        // No live bytes: store is a potential dead store, record in slot.st[].
        vgrow(ctx, &s->st, static_cast<ulong>(++s->nst));
        s->st[s->nst - 1].ip = ip;
        s->st[s->nst - 1].i = i;
    }
}

/// qsort comparator: descending by sz; ascending by r.a if sz equal (earlier first).
int scmp(const void* pa, const void* pb) {
    const Slot* a = static_cast<const Slot*>(pa);
    const Slot* b = static_cast<const Slot*>(pb);
    if (a->sz != b->sz)
        return b->sz - a->sz;
    return a->r.a - b->r.a;
}

/// loopiter callback (see docs/mem.md).
void maxrpo(Blk* hd, Blk* b) {
    if (hd->loop < static_cast<int>(b->id))
        hd->loop = static_cast<int>(b->id);
}

struct SlotList {
    Slot* sl;
    int nsl;
};

SlotList collect_slots(Ctx* ctx, Fn* fn) {
    SlotList sls;
    sls.nsl = 0;
    sls.sl = static_cast<Slot*>(vnew(ctx, 0, sizeof(Slot), PHeap));
    for (int n = Tmp0; n < fn->ntmp; n++) {
        Tmp* t = &fn->tmp[n];
        t->visit = -1;
        // Qualification check.
        if (t->alias.type != Alias::ALoc)
            continue;
        if (t->alias.slot != &t->alias)
            continue;
        if (t->bid != fn->start->id)
            continue;
        if (t->alias.u.loc.sz == -1)
            continue;

        t->visit = sls.nsl;
        vgrow(ctx, &sls.sl, static_cast<ulong>(++sls.nsl));
        Slot* s = &sls.sl[sls.nsl - 1];
        s->t = n;
        s->sz = t->alias.u.loc.sz;
        s->m = t->alias.u.loc.m;
        s->l = 0;
        s->r = Range{0, 0};
        s->s = nullptr;
        s->st = static_cast<Store*>(vnew(ctx, 0, sizeof(Store), PHeap));
        s->nst = 0;
    }
    return sls;
}

struct LiveOut {
    Range* br;  // br[n] is the entry/exit ip range of the n-th RPO block; PHeap, freed at coalesce
                // end.
    Ins** bl;   // blit list (PHeap vnew)
    int nbl;
    int ip;  // final ip (minimum); used for debugging output restoration.
};

LiveOut compute_liveness(Ctx* ctx, Fn* fn, Slot* sl, int nsl) {
    LiveOut lo;
    lo.nbl = 0;
    lo.bl = static_cast<Ins**>(vnew(ctx, 0, sizeof(Ins*), PHeap));
    lo.br = static_cast<Range*>(emalloc(static_cast<size_t>(fn->nblk) * sizeof(Range)));

    // Initialize b->loop for maxrpo
    for (Blk* b = fn->start; b; b = b->link)
        b->loop = -1;
    loopiter(fn, maxrpo);

    int ip = INT_MAX - 1;
    for (int n = static_cast<int>(fn->nblk) - 1; n >= 0; n--) {
        Blk* b = fn->rpo[n];

        // succ array (s1, s2 deduplicated, terminated by nullptr)
        Blk* succ[3];
        succ[0] = b->s1;
        succ[1] = b->s2;
        succ[2] = nullptr;

        lo.br[n].b = ip--;

        // For each slot: inherit live bytes from successor entries.
        for (Slot* s = sl; s < &sl[nsl]; s++) {
            s->l = 0;
            for (Blk** ps = succ; *ps; ps++) {
                int m = static_cast<int>((*ps)->id);
                if (m > n && rin(s->r, lo.br[m].a)) {
                    s->l = s->m;
                    radd(&s->r, ip);
                }
            }
        }

        // Process Jretc jmp arg: treat as full slot read.
        if (b->jmp.type == Jretc)
            load(b->jmp.arg, static_cast<bits>(-1), --ip, fn, sl);

        // Reverse traversal of instructions
        for (Ins* i = &b->ins[b->nins]; i != b->ins;) {
            --i;
            Ref* arg = i->arg;

            if (i->op == Oargc) {
                // by-value struct arg: mark entire slot as read
                load(arg[1], static_cast<bits>(-1), --ip, fn, sl);
            }
            if (isload(i->op)) {
                bits x = BIT(loadsz(i)) - 1;
                load(arg[0], x, --ip, fn, sl);
            }
            if (isstore(i->op)) {
                bits x = BIT(storesz(i)) - 1;
                store(arg[1], x, ip--, i, ctx, fn, sl);
            }
            if (i->op == Oblit0) {
                assert((i + 1)->op == Oblit1);
                assert(rtype((i + 1)->arg[0]) == RInt);
                int sz = std::abs(rsval((i + 1)->arg[0]));
                bits x = sz >= static_cast<int>(NBit) ? static_cast<bits>(-1) : BIT(sz) - 1;
                store(arg[1], x, ip--, i, ctx, fn, sl);
                load(arg[0], x, ip, fn, sl);
                vgrow(ctx, &lo.bl, static_cast<ulong>(++lo.nbl));
                lo.bl[lo.nbl - 1] = i;
            }
        }

        // Block entry update.
        for (Slot* s = sl; s < &sl[nsl]; s++)
            if (s->l) {
                radd(&s->r, ip);
                if (b->loop != -1) {
                    assert(b->loop >= n);
                    radd(&s->r, lo.br[b->loop].b - 1);
                }
            }
        lo.br[n].a = ip;
    }

    lo.ip = ip;
    return lo;
}

void kill_dead_stores(Slot* sl, int nsl) {
    for (Slot* s = sl; s < &sl[nsl]; s++) {
        for (int n = 0; n < s->nst; n++) {
            if (rin(s->r, s->st[n].ip))
                continue;
            // ip not in r -> dead store
            Ins* i = s->st[n].i;
            if (i->op == Oblit0)
                *(i + 1) = make_nop();
            *i = make_nop();
        }
    }
}

/// Kills empty interval slots recursively (see docs/mem.md).
struct KillStats {
    int new_nsl;
    uint total;
    uint freed;
};

KillStats kill_empty_slots(Ctx* ctx, Slot* sl, int nsl, Fn* fn) {
    int* stk = static_cast<int*>(vnew(ctx, 0, sizeof(int), PHeap));
    int n = 0;
    uint total = 0, freed = 0;

    // Compact sl[].
    Slot* s0 = sl;
    for (Slot* s = sl; s < &sl[nsl]; s++) {
        total += static_cast<uint>(s->sz);
        if (!s->r.b) {
            vfree(s->st);
            vgrow(ctx, &stk, static_cast<ulong>(++n));
            stk[n - 1] = s->t;
            freed += static_cast<uint>(s->sz);
        } else {
            *s0++ = *s;
        }
    }
    int new_nsl = static_cast<int>(s0 - sl);

    if (debug['M']) {
        fputs("\n> Slot coalescing:\n", ctx->dbg_buf);
        if (n) {
            fputs("\tkill [", ctx->dbg_buf);
            for (int m = 0; m < n; m++)
                fprintf(ctx->dbg_buf, " %%%s", fn->tmp[stk[m]].name);
            fputs(" ]\n", ctx->dbg_buf);
        }
    }

    // Propagate kill.
    while (n--) {
        Tmp* t = &fn->tmp[stk[n]];
        assert(t->ndef == 1 && t->def);
        Ins* i = t->def;
        if (isload(i->op)) {
            // This tmp is a pointer loaded; change to copy UNDEF so subsequent fold cleans it
            // naturally.
            i->op = Ocopy;
            i->arg[0] = Ref::undef();
            continue;
        }
        *i = make_nop();
        for (Use* u = t->use; u < &t->use[t->nuse]; u++) {
            if (u->type == Use::UJmp) {
                Blk* b = fn->rpo[u->bid];
                assert(isret(b->jmp.type));
                b->jmp.type = Jret0;
                b->jmp.arg = Ref::none();
                continue;
            }
            assert(u->type == Use::UIns);
            i = u->u.ins;
            if (!req(i->to, Ref::none())) {
                assert(rtype(i->to) == RTmp);
                vgrow(ctx, &stk, static_cast<ulong>(++n));
                stk[n - 1] = static_cast<int>(i->to.val);
            } else if (isarg(i->op)) {
                // Only Oargc reaches here: change its arg[1] to CON_Z to crash the callee
                // intentionally.
                assert(i->op == Oargc);
                i->arg[1] = Ref::con_z();
            } else {
                if (i->op == Oblit0)
                    *(i + 1) = make_nop();
                *i = make_nop();
            }
        }
    }
    vfree(stk);

    return KillStats{new_nsl, total, freed};
}

uint fuse_slots(Slot* sl, int nsl) {
    qsort(sl, static_cast<size_t>(nsl), sizeof(Slot), scmp);
    uint fused = 0;

    for (int n = 0; n < nsl; n++) {
        Slot* s0 = &sl[n];
        if (s0->s)
            continue;
        s0->s = s0;
        Range r = s0->r;

        for (Slot* s = s0 + 1; s < &sl[nsl]; s++) {
            if (s->s || !s->r.b)
                continue;
            if (rovlap(r, s->r)) {
                // Scan for collision.
                bool conflict = false;
                for (int m = n; &sl[m] < s; m++) {
                    if (sl[m].s == s0 && rovlap(sl[m].r, s->r)) {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                    continue;
            }
            radd(&r, s->r.a);
            radd(&r, s->r.b - 1);
            s->s = s0;
            fused += static_cast<uint>(s->sz);
        }
    }
    return fused;
}

void substitute_fused_slots(Slot* sl, int nsl, Fn* fn) {
    for (Slot* s = sl; s < &sl[nsl]; s++) {
        Tmp* t = &fn->tmp[s->t];
        // Reset visit.
        t->visit = static_cast<int>(s - sl);
        assert(t->ndef == 1 && t->def);
        if (s->s == s)
            continue;

        // Erase alloc instruction of this slot
        *t->def = make_nop();

        Tmp* ts = &fn->tmp[s->s->t];
        assert(t->bid == ts->bid);

        // Guarantee dominance.
        if (t->def < ts->def) {
            *t->def = *ts->def;
            *ts->def = make_nop();
            ts->def = t->def;
        }

        // Rewrite all uses to point to the representative tmp
        for (Use* u = t->use; u < &t->use[t->nuse]; u++) {
            if (u->type == Use::UJmp) {
                Blk* b = fn->rpo[u->bid];
                b->jmp.arg = Ref::tmp(static_cast<uint>(s->s->t));
                continue;
            }
            assert(u->type == Use::UIns);
            Ref* arg = u->u.ins->arg;
            for (int n = 0; n < 2; n++)
                if (req(arg[n], Ref::tmp(static_cast<uint>(s->t))))
                    arg[n] = Ref::tmp(static_cast<uint>(s->s->t));
        }
    }
}

void fix_blits(Ins** bl, int nbl, Slot* sl, Fn* fn) {
    for (int n = 0; n < nbl; n++) {
        Ins* i = bl[n];
        if (i->op != Oblit0)
            continue;  // Already cleaned up in some round
        Slot *sa, *sb;
        int64_t off0, off1;
        if (!slot_of(&sa, &off0, i->arg[0], fn, sl))
            continue;
        if (!slot_of(&sb, &off1, i->arg[1], fn, sl))
            continue;
        if (sa->s != sb->s)
            continue;

        // Both ends fused into same slot
        if (off0 < off1) {
            // Reverse blit: negative size to tell backend to reverse memmove.
            int sz = rsval((i + 1)->arg[0]);
            assert(sz >= 0);
            (i + 1)->arg[0] = Ref::integer(-sz);
        } else if (off0 == off1) {
            // Self-blit: delete entirely.
            *i = make_nop();
            *(i + 1) = make_nop();
        }
    }
}

void print_fuse_groups(Ctx* ctx, Slot* sl, int nsl, int ip, Fn* fn, uint freed, uint fused,
                       uint total) {
    FILE* f = ctx->dbg_buf;
    for (Slot* s0 = sl; s0 < &sl[nsl]; s0++) {
        if (s0->s != s0)
            continue;
        fprintf(f, "\tfuse (% 3db) [", s0->sz);
        for (Slot* s = s0; s < &sl[nsl]; s++) {
            if (s->s != s0)
                continue;
            fprintf(f, " %%%s", fn->tmp[s->t].name);
            if (s->r.b)
                fprintf(f, "[%d,%d)", s->r.a - ip, s->r.b - ip);
            else
                fputs("{}", f);
        }
        fputs(" ]\n", f);
    }
    fprintf(f, "\tsums %u/%u/%u (killed/fused/total)\n\n", freed, fused, total);
    printfn(fn, f);
}

}  // namespace

void coalesce(Ctx* ctx, Fn* fn) {
    // Collect candidate slots
    SlotList sls = collect_slots(ctx, fn);
    Slot* sl = sls.sl;
    int nsl = sls.nsl;

    // Single reverse RPO to compute live ranges
    LiveOut lo = compute_liveness(ctx, fn, sl, nsl);
    free(lo.br);

    // Kill out-of-range stores
    kill_dead_stores(sl, nsl);

    // Kill empty interval slots and propagate use-chain cleanup
    KillStats ks = kill_empty_slots(ctx, sl, nsl, fn);
    nsl = ks.new_nsl;

    // Fuse slots
    uint fused = fuse_slots(sl, nsl);

    // Redirect def/use of fused slots to representatives
    substitute_fused_slots(sl, nsl, fn);

    // Fix instructions that became same-slot self-blits
    fix_blits(lo.bl, lo.nbl, sl, fn);
    vfree(lo.bl);

    if (debug['M'])
        print_fuse_groups(ctx, sl, nsl, lo.ip, fn, ks.freed, fused, ks.total);

    for (Slot* s = sl; s < &sl[nsl]; s++)
        vfree(s->st);
    vfree(sl);
}

}  // namespace rcc
