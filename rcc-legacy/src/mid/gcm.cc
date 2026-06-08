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

/// @file gcm.cc
/// @brief Global Code Motion (GCM, Click's algorithm) -- rearranges instructions into the
/// "shallowest loop" legal block on the dominator tree.
///
/// See docs/gcm.md for architectural notes.

#include "rcc/all.h"

namespace rcc {

namespace {

constexpr uint NOBID = ~0u;

bool isdivwl(Ins* i) {
    return INRANGE(i->op, Odiv, Ourem) && KBASE(i->cls) == 0;
}

bool canelim(Ins* i) {
    return isload(i->op) || isalloc(i->op) || isdivwl(i);
}

Ins nopins() {
    Ins i = {};
    i.op = Onop;
    return i;
}

bool cheap(Ins* i) {
    if (KBASE(i->cls) != 0)
        return false;
    switch (i->op) {
        case Oneg:
        case Oadd:
        case Osub:
        case Omul:
        case Oand:
        case Oor:
        case Oxor:
        case Osar:
        case Oshr:
        case Oshl:
            return true;
        default: {
            int x;
            return iscmp(i->op, &x, &x) != 0;
        }
    }
}

// schedearly

uint earlyins(Fn*, Blk*, Ins*);

uint schedearly(Fn* fn, Ref r) {
    if (rtype(r) != RTmp)
        return 0;

    Tmp* t = &fn->tmp[r.val];
    if (t->gcmbid != NOBID)
        return t->gcmbid;

    if (!t->def)
        return t->gcmbid = t->bid;  // phi doesn't move, pinned to def block

    Blk* b = fn->rpo[t->bid];
    assert(b->ins <= t->def && t->def < &b->ins[b->nins]);
    t->gcmbid = 0;  // Visiting sentinel (recursive backedge gets 0 = start)
    return t->gcmbid = earlyins(fn, b, t->def);
}

uint earlyins(Fn* fn, Blk* b, Ins* i) {
    uint ba = schedearly(fn, i->arg[0]);
    uint bb = schedearly(fn, i->arg[1]);
    assert(ba != NOBID && bb != NOBID);

    // Take the deeper operand block in dominator tree (shallower dominates deeper)
    if (fn->rpo[ba]->depth < fn->rpo[bb]->depth) {
        assert(dom(fn->rpo[ba], fn->rpo[bb]));
        ba = bb;
    }
    return pinned(i) ? b->id : ba;
}

void earlyblk(Fn* fn, uint bid) {
    Blk* b = fn->rpo[bid];

    for (Phi* p = b->phi; p; p = p->link)
        for (uint n = 0; n < p->narg; n++)
            schedearly(fn, p->arg[n]);
    for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
        if (pinned(i)) {
            schedearly(fn, i->arg[0]);
            schedearly(fn, i->arg[1]);
        }
    schedearly(fn, b->jmp.arg);
}

// schedlate + bestbid

uint lcabid(Fn* fn, uint b1, uint b2) {
    if (b1 == NOBID)
        return b2;
    if (b2 == NOBID)
        return b1;
    Blk* b = lca(fn->rpo[b1], fn->rpo[b2]);
    assert(b);
    return b->id;
}

uint bestbid(Fn* fn, uint earlybid, uint latebid) {
    if (latebid == NOBID)
        return NOBID;  // Unused
    assert(earlybid != NOBID);

    Blk* earlyb = fn->rpo[earlybid];
    Blk* bestb = fn->rpo[latebid];
    assert(dom(earlyb, bestb));

    for (Blk* curb = bestb; curb != earlyb;) {
        curb = curb->idom;
        if (curb->loop < bestb->loop)
            bestb = curb;
    }
    return bestb->id;
}

uint lateins(Fn*, Blk*, Ins*, Ref);
uint latephi(Fn*, Phi*, Ref);
uint latejmp(Blk*, Ref);

uint schedlate(Fn* fn, Ref r) {
    if (rtype(r) != RTmp)
        return NOBID;

    Tmp* t = &fn->tmp[r.val];
    if (t->visit)
        return t->gcmbid;
    t->visit = 1;

    uint earlybid = t->gcmbid;
    if (earlybid == NOBID)
        return NOBID;  // Unused

    t->gcmbid = t->bid;  // Reuse gcmbid field to temporarily store late
    uint latebid = NOBID;
    for (Use* u = t->use; u < &t->use[t->nuse]; u++) {
        assert(u->bid < fn->nblk);
        Blk* b = fn->rpo[u->bid];
        uint usebid;
        switch (u->type) {
            case Use::UPhi:
                usebid = latephi(fn, u->u.phi, r);
                break;
            case Use::UIns:
                usebid = lateins(fn, b, u->u.ins, r);
                break;
            case Use::UJmp:
                usebid = latejmp(b, r);
                break;
            default:
                die("unreachable");
        }
        latebid = lcabid(fn, latebid, usebid);
    }
    // latebid might still be NOBID: temp is only indirectly used by "deletable pinned instructions"

    if (t->def && !pinned(t->def))
        t->gcmbid = bestbid(fn, earlybid, latebid);
    // Otherwise keep early

    return t->gcmbid;
}

uint lateins(Fn* fn, Blk* b, Ins* i, Ref r) {
    assert(b->ins <= i && i < &b->ins[b->nins]);
    assert(req(i->arg[0], r) || req(i->arg[1], r));

    uint latebid = schedlate(fn, i->to);
    if (pinned(i)) {
        if (latebid == NOBID && canelim(i))
            return NOBID;
        return b->id;
    }
    return latebid;
}

uint latephi(Fn* fn, Phi* p, Ref r) {
    if (!p->narg)
        return NOBID;  // Marked unused

    uint latebid = NOBID;
    for (uint n = 0; n < p->narg; n++)
        if (req(p->arg[n], r))
            latebid = lcabid(fn, latebid, p->blk[n]->id);

    assert(latebid != NOBID);
    return latebid;
}

uint latejmp(Blk* b, Ref r) {
    if (req(b->jmp.arg, Ref::none()))
        return NOBID;
    assert(req(b->jmp.arg, r));
    return b->id;
}

void lateblk(Fn* fn, uint bid) {
    Blk* b = fn->rpo[bid];

    for (Phi** pp = &b->phi; *pp;) {
        if (schedlate(fn, (*pp)->to) == NOBID) {
            (*pp)->narg = 0;    // Mark unused
            *pp = (*pp)->link;  // Remove this phi
        } else {
            pp = &(*pp)->link;
        }
    }
    for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
        if (pinned(i))
            schedlate(fn, i->to);
}

// gcmmove

void addgcmins(Ctx* ctx, Fn* fn, Ins* vins, uint nins) {
    for (Ins* i = vins; i < &vins[nins]; i++) {
        assert(rtype(i->to) == RTmp);
        Tmp* t = &fn->tmp[i->to.val];
        Blk* b = fn->rpo[t->gcmbid];
        addins(ctx, &b->ins, &b->nins, i);
    }
}

void gcmmove(Ctx* ctx, Fn* fn) {
    uint nins = 0;
    Ins* vins = static_cast<Ins*>(vnew(ctx, nins, sizeof vins[0], PFn));

    for (Tmp* t = fn->tmp; t < &fn->tmp[fn->ntmp]; t++) {
        if (!t->def || t->bid == t->gcmbid)
            continue;
        Ins* i = t->def;
        if (pinned(i) && !canelim(i))
            continue;
        assert(rtype(i->to) == RTmp && t == &fn->tmp[i->to.val]);
        if (t->gcmbid != NOBID)
            addins(ctx, &vins, &nins, i);
        *i = nopins();
    }
    addgcmins(ctx, fn, vins, nins);
}

// schedblk (intra-block DFS ordering)

Ins* schedins(Ctx* ctx, Fn* fn, Blk* b, Ins* i, Ins** pvins, uint* pnins) {
    Ins *i0, *i1;
    igroup(b, i, &i0, &i1);

    // First recursively order the defs in this block that are depended on by operands in this group
    for (i = i0; i < i1; i++)
        for (uint n = 0; n < 2; n++) {
            if (rtype(i->arg[n]) != RTmp)
                continue;
            Tmp* t = &fn->tmp[i->arg[n].val];
            if (t->bid != b->id || !t->def)
                continue;
            schedins(ctx, fn, b, t->def, pvins, pnins);
        }
    // Then emit this group, wipe original to nop (prevent double emission)
    for (i = i0; i < i1; i++) {
        addins(ctx, pvins, pnins, i);
        *i = nopins();
    }
    return i1;
}

void schedblk(Ctx* ctx, Fn* fn) {
    Ins* vins = static_cast<Ins*>(vnew(ctx, 0, sizeof vins[0], PHeap));

    for (Blk* b = fn->start; b; b = b->link) {
        uint nins = 0;
        for (Ins* i = b->ins; i < &b->ins[b->nins];)
            i = schedins(ctx, fn, b, i, &vins, &nins);
        idup(ctx, b, vins, nins);
    }
    vfree(vins);
}

// sink (trivial op sinking)

void sinkref(Ctx* ctx, Fn* fn, Blk* b, Ref* pr) {
    if (rtype(*pr) != RTmp)
        return;
    Tmp* t = &fn->tmp[pr->val];
    if (!t->def || t->bid == b->id || pinned(t->def) || !cheap(t->def))
        return;

    // Copy t->def and sink to b
    Ins i = *t->def;
    Ref r = newtmp(ctx, "snk", t->cls, fn);
    t = nullptr;  // newtmp might realloc fn->tmp, old pointer invalid
    *pr = r;
    i.to = r;
    fn->tmp[r.val].gcmbid = b->id;
    emiti(ctx, i);
    sinkref(ctx, fn, b, &i.arg[0]);
    sinkref(ctx, fn, b, &i.arg[1]);
}

void sink(Ctx* ctx, Fn* fn) {
    for (Blk* b = fn->start; b; b = b->link) {
        for (Ins* i = b->ins; i < &b->ins[b->nins]; i++)
            if (isload(i->op))
                sinkref(ctx, fn, b, &i->arg[0]);
            else if (isstore(i->op))
                sinkref(ctx, fn, b, &i->arg[1]);
        sinkref(ctx, fn, b, &b->jmp.arg);
    }
    addgcmins(ctx, fn, ctx->curi, static_cast<uint>(&ctx->insb[NIns] - ctx->curi));
}

}  // namespace

int pinned(Ins* i) {
    return optab[i->op].pinned || isdivwl(i);
}

void gcm(Ctx* ctx, Fn* fn) {
    filldepth(fn);
    fillloop(fn);

    for (Tmp* t = fn->tmp; t < &fn->tmp[fn->ntmp]; t++) {
        t->visit = 0;
        t->gcmbid = NOBID;
    }
    for (uint bid = 0; bid < fn->nblk; bid++)
        earlyblk(fn, bid);
    for (uint bid = 0; bid < fn->nblk; bid++)
        lateblk(fn, bid);

    gcmmove(ctx, fn);
    filluse(ctx, fn);
    ctx->curi = &ctx->insb[NIns];
    sink(ctx, fn);
    filluse(ctx, fn);
    schedblk(ctx, fn);

    if (debug['G']) {
        fprintf(ctx->dbg_buf, "\n> After GCM:\n");
        printfn(fn, ctx->dbg_buf);
    }
}

}  // namespace rcc
