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

/// @file backend/rega.cc
/// @brief Register allocation pass.

#include "rcc/all.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace rcc {

namespace {

#ifdef TEST_PMOV
#undef assert
#define assert(x) assert_test(#x, x)
#endif

struct RMap {
    int t[Tmp0];
    int r[Tmp0];
    int w[Tmp0]; /* wait list, for unmatched hints */
    BSet b[1];
    int n;
};

int* hint(Ctx* ctx, int t) {
    return &ctx->rega_tmp[phicls(t, ctx->rega_tmp)].hint.r;
}

void sethint(Ctx* ctx, int t, int r) {
    Tmp* p;

    p = &ctx->rega_tmp[phicls(t, ctx->rega_tmp)];
    if (p->hint.r == -1 || p->hint.w > ctx->rega_loop) {
        p->hint.r = r;
        p->hint.w = ctx->rega_loop;
        ctx->rega_tmp[t].visit = -1;
    }
}

void rcopy(RMap* ma, RMap* mb) {
    memcpy(ma->t, mb->t, sizeof ma->t);
    memcpy(ma->r, mb->r, sizeof ma->r);
    memcpy(ma->w, mb->w, sizeof ma->w);
    bscopy(ma->b, mb->b);
    ma->n = mb->n;
}

int rfind(RMap* m, int t) {
    int i;

    for (i = 0; i < m->n; i++)
        if (m->t[i] == t)
            return m->r[i];
    return -1;
}

Ref rref(Ctx* ctx, RMap* m, int t) {
    int r, s;

    r = rfind(m, t);
    if (r == -1) {
        s = ctx->rega_tmp[t].slot;
        assert(s != -1 && "should have spilled");
        return Ref::slot(s);
    } else
        return Ref::tmp(static_cast<uint>(r));
}

void radd(Ctx* ctx, RMap* m, int t, int r) {
    assert((t >= Tmp0 || t == r) && "invalid temporary");
    assert(((T.gpr0 <= r && r < T.gpr0 + T.ngpr) || (T.fpr0 <= r && r < T.fpr0 + T.nfpr)) &&
           "invalid register");
    assert(!bshas(m->b, static_cast<uint>(t)) && "temporary has mapping");
    assert(!bshas(m->b, static_cast<uint>(r)) && "register already allocated");
    assert(m->n <= T.ngpr + T.nfpr && "too many mappings");
    bsset(m->b, static_cast<uint>(t));
    bsset(m->b, static_cast<uint>(r));
    m->t[m->n] = t;
    m->r[m->n] = r;
    m->n++;
    ctx->rega_regu |= BIT(r);
}

Ref ralloctry(Ctx* ctx, RMap* m, int t, int try_) {
    bits regs;
    int h, r, r0, r1;

    if (t < Tmp0) {
        assert(bshas(m->b, static_cast<uint>(t)));
        return Ref::tmp(static_cast<uint>(t));
    }
    if (bshas(m->b, static_cast<uint>(t))) {
        r = rfind(m, t);
        assert(r != -1);
        return Ref::tmp(static_cast<uint>(r));
    }
    r = ctx->rega_tmp[t].visit;
    if (r == -1 || bshas(m->b, static_cast<uint>(r)))
        r = *hint(ctx, t);
    if (r == -1 || bshas(m->b, static_cast<uint>(r))) {
        if (try_)
            return Ref::none();
        regs = ctx->rega_tmp[phicls(t, ctx->rega_tmp)].hint.m;
        regs |= m->b->t[0];
        if (KBASE(ctx->rega_tmp[t].cls) == 0) {
            r0 = T.gpr0;
            r1 = r0 + T.ngpr;
        } else {
            r0 = T.fpr0;
            r1 = r0 + T.nfpr;
        }
        for (r = r0; r < r1; r++)
            if (!(regs & BIT(r)))
                goto Found;
        for (r = r0; r < r1; r++)
            if (!bshas(m->b, static_cast<uint>(r)))
                goto Found;
        fprintf(stderr, "DBG no-more-regs: t=%d cls=%d range=[R%d,R%d) m->n=%d\n", t,
                ctx->rega_tmp[t].cls, r0, r1, m->n);
        fprintf(stderr, "  m->t/r entries:\n");
        for (int idx = 0; idx < m->n; ++idx)
            fprintf(stderr, "    [%d] t=%d -> r=R%d\n", idx, m->t[idx], m->r[idx]);
        fprintf(stderr, "  m->b set bits:");
        for (int rr = 0; rr < 64; ++rr)
            if (bshas(m->b, static_cast<uint>(rr)))
                fprintf(stderr, " %d", rr);
        fprintf(stderr, "\n");
        err(const_cast<char*>("no more regs"));
    }
Found:
    radd(ctx, m, t, r);
    sethint(ctx, t, r);
    ctx->rega_tmp[t].visit = r;
    h = *hint(ctx, t);
    if (h != -1 && h != r)
        m->w[h] = t;
    return Ref::tmp(static_cast<uint>(r));
}

inline Ref ralloc(Ctx* ctx, RMap* m, int t) {
    return ralloctry(ctx, m, t, 0);
}

int rfree(RMap* m, int t) {
    int i, r;

    assert(t >= Tmp0 || !(BIT(t) & T.rglob));
    if (!bshas(m->b, static_cast<uint>(t)))
        return -1;
    for (i = 0; m->t[i] != t; i++)
        assert(i + 1 < m->n);
    r = m->r[i];
    bsclr(m->b, static_cast<uint>(t));
    bsclr(m->b, static_cast<uint>(r));
    m->n--;
    memmove(&m->t[i], &m->t[i + 1], static_cast<size_t>(m->n - i) * sizeof m->t[0]);
    memmove(&m->r[i], &m->r[i + 1], static_cast<size_t>(m->n - i) * sizeof m->r[0]);
    assert(t >= Tmp0 || t == r);
    return r;
}

void mdump(Ctx* ctx, RMap* m) {
    int i;

    for (i = 0; i < m->n; i++)
        if (m->t[i] >= Tmp0)
            fprintf(ctx->dbg_buf, " (%s, R%d)", ctx->rega_tmp[m->t[i]].name, m->r[i]);
    fprintf(ctx->dbg_buf, "\n");
}

void pmadd(Ctx* ctx, Ref src, Ref dst, int k) {
    if (ctx->rega_npm == NPm)
        err(const_cast<char*>("no more pm slots"));
    ctx->rega_pm[ctx->rega_npm].src = src;
    ctx->rega_pm[ctx->rega_npm].dst = dst;
    ctx->rega_pm[ctx->rega_npm].cls = k;
    ctx->rega_npm++;
}

enum PMStat { ToMove, Moving, Moved };

int pmrec(Ctx* ctx, enum PMStat* status, int i, int* k) {
    int j, c;

    /* note, this routine might emit
     * too many large instructions
     */
    if (req(ctx->rega_pm[i].src, ctx->rega_pm[i].dst)) {
        status[i] = Moved;
        return -1;
    }
    assert(KBASE(ctx->rega_pm[i].cls) == KBASE(*k));
    assert((Kw | Kl) == Kl && (Ks | Kd) == Kd);
    *k |= ctx->rega_pm[i].cls;
    for (j = 0; j < ctx->rega_npm; j++)
        if (req(ctx->rega_pm[j].dst, ctx->rega_pm[i].src))
            break;
    switch (j == ctx->rega_npm ? Moved : status[j]) {
        case Moving:
            c = j; /* start of cycle */
            emit(ctx, Oswap, *k, Ref::none(), ctx->rega_pm[i].src, ctx->rega_pm[i].dst);
            break;
        case ToMove:
            status[i] = Moving;
            c = pmrec(ctx, status, j, k);
            if (c == i) {
                c = -1; /* end of cycle */
                break;
            }
            if (c != -1) {
                emit(ctx, Oswap, *k, Ref::none(), ctx->rega_pm[i].src, ctx->rega_pm[i].dst);
                break;
            }
            [[fallthrough]];
        case Moved:
            c = -1;
            emit(ctx, Ocopy, ctx->rega_pm[i].cls, ctx->rega_pm[i].dst, ctx->rega_pm[i].src,
                 Ref::none());
            break;
        default:
            die("unreachable");
    }
    status[i] = Moved;
    return c;
}

void pmgen(Ctx* ctx) {
    int i;
    enum PMStat* status;

    status = static_cast<enum PMStat*>(
        alloc(ctx, static_cast<size_t>(ctx->rega_npm) * sizeof status[0]));
    assert(!ctx->rega_npm || status[ctx->rega_npm - 1] == ToMove);
    for (i = 0; i < ctx->rega_npm; i++)
        if (status[i] == ToMove) {
            int k = ctx->rega_pm[i].cls;
            pmrec(ctx, status, i, &k);
        }
}

void move(Ctx* ctx, int r, Ref to, RMap* m) {
    int n, t, r1;

    r1 = req(to, Ref::none()) ? -1 : rfree(m, static_cast<int>(to.val));
    if (bshas(m->b, static_cast<uint>(r))) {
        /* r is used and not by to */
        assert(r1 != r);
        for (n = 0; m->r[n] != r; n++)
            assert(n + 1 < m->n);
        t = m->t[n];
        rfree(m, t);
        bsset(m->b, static_cast<uint>(r));
        ralloc(ctx, m, t);
        bsclr(m->b, static_cast<uint>(r));
    }
    t = req(to, Ref::none()) ? r : static_cast<int>(to.val);
    radd(ctx, m, t, r);
}

int regcpy(Ins* i) {
    return i->op == Ocopy && isreg(i->arg[0]);
}

Ins* dopm(Ctx* ctx, Blk* b, Ins* i, RMap* m) {
    RMap m0;
    int n, r, r1, t, s;
    Ins *i1, *ip;
    bits def;

    m0 = *m; /* okay since we don't use m0.b */
    m0.b->t = 0;
    i1 = ++i;
    do {
        i--;
        move(ctx, static_cast<int>(i->arg[0].val), i->to, m);
    } while (i != b->ins && regcpy(i - 1));
    assert(m0.n <= m->n);
    if (i != b->ins && (i - 1)->op == Ocall) {
        def = T.retregs((i - 1)->arg[1], 0) | T.rglob;
        for (r = 0; T.rsave[r] >= 0; r++)
            if (!(BIT(T.rsave[r]) & def))
                move(ctx, T.rsave[r], Ref::none(), m);
    }
    for (ctx->rega_npm = 0, n = 0; n < m->n; n++) {
        t = m->t[n];
        s = ctx->rega_tmp[t].slot;
        r1 = m->r[n];
        r = rfind(&m0, t);
        if (r != -1)
            pmadd(ctx, Ref::tmp(static_cast<uint>(r1)), Ref::tmp(static_cast<uint>(r)),
                  ctx->rega_tmp[t].cls);
        else if (s != -1)
            pmadd(ctx, Ref::tmp(static_cast<uint>(r1)), Ref::slot(s), ctx->rega_tmp[t].cls);
    }
    for (ip = i; ip < i1; ip++) {
        if (!req(ip->to, Ref::none()))
            rfree(m, static_cast<int>(ip->to.val));
        r = static_cast<int>(ip->arg[0].val);
        if (rfind(m, r) == -1)
            radd(ctx, m, r, r);
    }
    pmgen(ctx);
    return i;
}

int prio1(Ctx* ctx, Ref r1, Ref r2) {
    /* trivial heuristic to begin with,
     * later we can use the distance to
     * the definition instruction
     */
    (void)r2;
    return *hint(ctx, static_cast<int>(r1.val)) != -1;
}

void insert(Ctx* ctx, Ref* r, Ref** rs, int p) {
    int i;

    rs[i = p] = r;
    while (i-- > 0 && prio1(ctx, *r, *rs[i])) {
        rs[i + 1] = rs[i];
        rs[i] = r;
    }
}

void doblk(Ctx* ctx, Blk* b, RMap* cur) {
    int t, x, r, rf, rt, nr;
    bits rs;
    Ins *i, *i1;
    Mem* m;
    Ref* ra[4];

    if (rtype(b->jmp.arg) == RTmp)
        b->jmp.arg = ralloc(ctx, cur, static_cast<int>(b->jmp.arg.val));
    if (rtype(b->jmp.arg2) == RTmp)
        b->jmp.arg2 = ralloc(ctx, cur, static_cast<int>(b->jmp.arg2.val));
    ctx->curi = &ctx->insb[NIns];
    for (i1 = &b->ins[b->nins]; i1 != b->ins;) {
        emiti(ctx, *--i1);
        i = ctx->curi;
        rf = -1;
        switch (i->op) {
            case Ocall:
                rs = T.argregs(i->arg[1], 0) | T.rglob;
                for (r = 0; T.rsave[r] >= 0; r++)
                    if (!(BIT(T.rsave[r]) & rs))
                        rfree(cur, T.rsave[r]);
                break;
            case Ocopy:
                if (regcpy(i)) {
                    ctx->curi++;
                    i1 = dopm(ctx, b, i1, cur);
                    ctx->rega_stmov += static_cast<uint>(i + 1 - ctx->curi);
                    continue;
                }
                if (isreg(i->to))
                    if (rtype(i->arg[0]) == RTmp)
                        sethint(ctx, static_cast<int>(i->arg[0].val), static_cast<int>(i->to.val));
                [[fallthrough]];
            default:
                if (!req(i->to, Ref::none())) {
                    assert(rtype(i->to) == RTmp);
                    r = static_cast<int>(i->to.val);
                    if (r < Tmp0 && (BIT(r) & T.rglob))
                        break;
                    rf = rfree(cur, r);
                    if (rf == -1) {
                        assert(!isreg(i->to));
                        ctx->curi++;
                        continue;
                    }
                    i->to = Ref::tmp(static_cast<uint>(rf));
                }
                break;
        }
        for (x = 0, nr = 0; x < 2; x++)
            switch (rtype(i->arg[x])) {
                case RMem:
                    m = &ctx->rega_mem[i->arg[x].val];
                    if (rtype(m->base) == RTmp)
                        insert(ctx, &m->base, ra, nr++);
                    if (rtype(m->index) == RTmp)
                        insert(ctx, &m->index, ra, nr++);
                    break;
                case RTmp:
                    insert(ctx, &i->arg[x], ra, nr++);
                    break;
            }
        for (r = 0; r < nr; r++)
            *ra[r] = ralloc(ctx, cur, static_cast<int>(ra[r]->val));
        if (i->op == Ocopy && req(i->to, i->arg[0]))
            ctx->curi++;

        /* try to change the register of a hinted
         * temporary if rf is available */
        if (rf != -1 && (t = cur->w[rf]) != 0)
            if (!bshas(cur->b, static_cast<uint>(rf)) && *hint(ctx, t) == rf &&
                (rt = rfree(cur, t)) != -1) {
                ctx->rega_tmp[t].visit = -1;
                ralloc(ctx, cur, t);
                assert(bshas(cur->b, static_cast<uint>(rf)));
                emit(ctx, Ocopy, ctx->rega_tmp[t].cls, Ref::tmp(static_cast<uint>(rt)),
                     Ref::tmp(static_cast<uint>(rf)), Ref::none());
                ctx->rega_stmov += 1;
                cur->w[rf] = 0;
                for (r = 0; r < nr; r++)
                    if (req(*ra[r], Ref::tmp(static_cast<uint>(rt))))
                        *ra[r] = Ref::tmp(static_cast<uint>(rf));
                /* one could iterate this logic with
                 * the newly freed rt, but in this case
                 * the above loop must be changed */
            }
    }
    idup(ctx, b, ctx->curi, static_cast<ulong>(&ctx->insb[NIns] - ctx->curi));
}

/* comparison function to order temporaries
 * for allocation at the end of blocks */
int prio2(Ctx* ctx, int t1, int t2) {
    if ((ctx->rega_tmp[t1].visit ^ ctx->rega_tmp[t2].visit) < 0) /* != signs */
        return ctx->rega_tmp[t1].visit != -1 ? +1 : -1;
    if ((*hint(ctx, t1) ^ *hint(ctx, t2)) < 0)
        return *hint(ctx, t1) != -1 ? +1 : -1;
    return static_cast<int>(ctx->rega_tmp[t1].cost) - static_cast<int>(ctx->rega_tmp[t2].cost);
}

}  // namespace

/* register allocation
 * depends on rpo, phi, cost, (and obviously spill)
 */
void rega(Ctx* ctx, Fn* fn) {
    int j, t, r, x, rl[Tmp0];
    Blk *b, *b1, *s, ***ps, *blist, **blk, **bp;
    RMap *end, *beg, cur, old, *m;
    Ins* i;
    Phi* p;
    uint u, n;
    Ref src, dst;

    /* setup */
    ctx->rega_stmov = 0;
    ctx->rega_stblk = 0;
    ctx->rega_regu = 0;
    ctx->rega_tmp = fn->tmp;
    ctx->rega_mem = fn->mem;
    blk = static_cast<Blk**>(alloc(ctx, fn->nblk * sizeof blk[0]));
    end = static_cast<RMap*>(alloc(ctx, fn->nblk * sizeof end[0]));
    beg = static_cast<RMap*>(alloc(ctx, fn->nblk * sizeof beg[0]));
    for (n = 0; n < fn->nblk; n++) {
        bsinit(ctx, end[n].b, static_cast<uint>(fn->ntmp));
        bsinit(ctx, beg[n].b, static_cast<uint>(fn->ntmp));
    }
    bsinit(ctx, cur.b, static_cast<uint>(fn->ntmp));
    bsinit(ctx, old.b, static_cast<uint>(fn->ntmp));
    (void)old; /* declares but doesn't use this, suppress -Wunused here */

    ctx->rega_loop = INT_MAX;
    for (t = 0; t < fn->ntmp; t++) {
        ctx->rega_tmp[t].hint.r = t < Tmp0 ? t : -1;
        ctx->rega_tmp[t].hint.w = ctx->rega_loop;
        ctx->rega_tmp[t].visit = -1;
    }
    for (bp = blk, b = fn->start; b; b = b->link)
        *bp++ = b;
    /* qsort() comparison function to peel
     * loop nests from inside out */
    std::sort(blk, blk + fn->nblk, [](Blk* ba, Blk* bb) {
        /* todo, evaluate if this order is really
         * better than the simple postorder */
        if (ba->loop == bb->loop)
            return ba->id > bb->id;
        return ba->loop > bb->loop;
    });
    for (b = fn->start, i = b->ins; i < &b->ins[b->nins]; i++)
        if (i->op != Ocopy || !isreg(i->arg[0]))
            break;
        else {
            assert(rtype(i->to) == RTmp);
            sethint(ctx, static_cast<int>(i->to.val), static_cast<int>(i->arg[0].val));
        }

    /* assign registers */
    for (bp = blk; bp < &blk[fn->nblk]; bp++) {
        b = *bp;
        n = b->id;
        ctx->rega_loop = b->loop;
        cur.n = 0;
        bszero(cur.b);
        memset(cur.w, 0, sizeof cur.w);
        for (x = 0, t = Tmp0; bsiter(b->out, &t); t++) {
            j = x++;
            rl[j] = t;
            while (j-- > 0 && prio2(ctx, t, rl[j]) > 0) {
                rl[j + 1] = rl[j];
                rl[j] = t;
            }
        }
        for (r = 0; bsiter(b->out, &r) && r < Tmp0; r++)
            radd(ctx, &cur, r, r);
        for (j = 0; j < x; j++)
            ralloctry(ctx, &cur, rl[j], 1);
        for (j = 0; j < x; j++)
            ralloc(ctx, &cur, rl[j]);
        rcopy(&end[n], &cur);
        doblk(ctx, b, &cur);
        bscopy(b->in, cur.b);
        for (p = b->phi; p; p = p->link)
            if (rtype(p->to) == RTmp)
                bsclr(b->in, p->to.val);
        rcopy(&beg[n], &cur);
    }

    /* emit copies shared by multiple edges
     * to the same block */
    for (s = fn->start; s; s = s->link) {
        if (s->npred <= 1)
            continue;
        m = &beg[s->id];

        /* rl maps a register that is live at the
         * beginning of s to the one used in all
         * predecessors (if any, -1 otherwise) */
        memset(rl, 0, sizeof rl);

        /* to find the register of a phi in a
         * predecessor, we have to find the
         * corresponding argument */
        for (p = s->phi; p; p = p->link) {
            if (rtype(p->to) != RTmp || (r = rfind(m, static_cast<int>(p->to.val))) == -1)
                continue;
            for (u = 0; u < p->narg; u++) {
                b = p->blk[u];
                src = p->arg[u];
                if (rtype(src) != RTmp)
                    continue;
                x = rfind(&end[b->id], static_cast<int>(src.val));
                if (x == -1) /* spilled */
                    continue;
                rl[r] = (!rl[r] || rl[r] == x) ? x : -1;
            }
            if (rl[r] == 0)
                rl[r] = -1;
        }

        /* process non-phis temporaries */
        for (j = 0; j < m->n; j++) {
            t = m->t[j];
            r = m->r[j];
            if (rl[r] || t < Tmp0 /* todo, remove this */)
                continue;
            for (bp = s->pred; bp < &s->pred[s->npred]; bp++) {
                x = rfind(&end[(*bp)->id], t);
                if (x == -1) /* spilled */
                    continue;
                rl[r] = (!rl[r] || rl[r] == x) ? x : -1;
            }
            if (rl[r] == 0)
                rl[r] = -1;
        }

        ctx->rega_npm = 0;
        for (j = 0; j < m->n; j++) {
            t = m->t[j];
            r = m->r[j];
            x = rl[r];
            assert(x != 0 || t < Tmp0 /* todo, ditto */);
            if (x > 0 && !bshas(m->b, static_cast<uint>(x))) {
                pmadd(ctx, Ref::tmp(static_cast<uint>(x)), Ref::tmp(static_cast<uint>(r)),
                      ctx->rega_tmp[t].cls);
                m->r[j] = x;
                bsset(m->b, static_cast<uint>(x));
            }
        }
        ctx->curi = &ctx->insb[NIns];
        pmgen(ctx);
        j = static_cast<int>(&ctx->insb[NIns] - ctx->curi);
        if (j == 0)
            continue;
        ctx->rega_stmov += static_cast<uint>(j);
        s->nins += static_cast<uint>(j);
        i = static_cast<Ins*>(alloc(ctx, s->nins * sizeof(Ins)));
        icpy(icpy(i, ctx->curi, static_cast<ulong>(j)), s->ins, s->nins - static_cast<uint>(j));
        s->ins = i;
    }

    if (debug['R']) {
        fprintf(ctx->dbg_buf, "\n> Register mappings:\n");
        for (n = 0; n < fn->nblk; n++) {
            b = fn->rpo[n];
            fprintf(ctx->dbg_buf, "\t%-10s beg", b->name);
            mdump(ctx, &beg[n]);
            fprintf(ctx->dbg_buf, "\t           end");
            mdump(ctx, &end[n]);
        }
        fprintf(ctx->dbg_buf, "\n");
    }

    /* emit remaining copies in new blocks */
    blist = 0;
    for (b = fn->start;; b = b->link) {
        Blk* null_blk = nullptr;
        Blk** ps_arr[3] = {&b->s1, &b->s2, &null_blk};
        ps = ps_arr;
        for (; (s = **ps); ps++) {
            ctx->rega_npm = 0;
            for (p = s->phi; p; p = p->link) {
                dst = p->to;
                assert(rtype(dst) == RSlot || rtype(dst) == RTmp);
                if (rtype(dst) == RTmp) {
                    r = rfind(&beg[s->id], static_cast<int>(dst.val));
                    if (r == -1)
                        continue;
                    dst = Ref::tmp(static_cast<uint>(r));
                }
                for (u = 0; p->blk[u] != b; u++)
                    assert(u + 1 < p->narg);
                src = p->arg[u];
                if (rtype(src) == RTmp)
                    src = rref(ctx, &end[b->id], static_cast<int>(src.val));
                pmadd(ctx, src, dst, p->cls);
            }
            for (t = Tmp0; bsiter(s->in, &t); t++) {
                src = rref(ctx, &end[b->id], t);
                dst = rref(ctx, &beg[s->id], t);
                pmadd(ctx, src, dst, ctx->rega_tmp[t].cls);
            }
            ctx->curi = &ctx->insb[NIns];
            pmgen(ctx);
            if (ctx->curi == &ctx->insb[NIns])
                continue;
            b1 = newblk(ctx);
            b1->loop = (b->loop + s->loop) / 2;
            b1->link = blist;
            blist = b1;
            fn->nblk++;
            b1->name = strf(ctx, PFn, "%s_%s", b->name, s->name);
            ctx->rega_stmov += static_cast<uint>(&ctx->insb[NIns] - ctx->curi);
            ctx->rega_stblk += 1;
            idup(ctx, b1, ctx->curi, static_cast<ulong>(&ctx->insb[NIns] - ctx->curi));
            b1->jmp.type = Jjmp;
            b1->s1 = s;
            **ps = b1;
        }
        if (!b->link) {
            b->link = blist;
            break;
        }
    }
    for (b = fn->start; b; b = b->link)
        b->phi = 0;
    fn->reg = ctx->rega_regu;

    if (debug['R']) {
        fprintf(ctx->dbg_buf, "\n> Register allocation statistics:\n");
        fprintf(ctx->dbg_buf, "\tnew moves:  %d\n", ctx->rega_stmov);
        fprintf(ctx->dbg_buf, "\tnew blocks: %d\n", ctx->rega_stblk);
        fprintf(ctx->dbg_buf, "\n> After register allocation:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
