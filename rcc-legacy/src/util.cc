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

/// @file util.cc
/// @brief Infrastructure: Memory management, growable arrays.

#include "rcc/all.h"

#include <cstdarg>
#include <vector>

namespace rcc {

// Global Read-Only Resources

Typ* typ;
Target T = {};

char debug['Z' + 1] = {};
Op optab[NOp];

namespace {
struct OptabInit {
    OptabInit() {
        for (size_t i = 0; i < NOp; ++i) {
            const OpInfo& info = kops[i];
            Op& o = optab[i];
            o.name = const_cast<char*>(info.name);
            for (int a = 0; a < 2; ++a)
                for (int b = 0; b < 4; ++b)
                    o.argcls[a][b] = info.argcls[a][b];
            o.canfold = (info.flags & Fold) != 0;
            o.hasid = (info.flags & Id) != 0;
            o.idval = (info.flags & IdV) != 0;
            o.commutes = (info.flags & Comm) != 0;
            o.assoc = (info.flags & Assoc) != 0;
            o.idemp = (info.flags & Idemp) != 0;
            o.cmpeqwl = (info.flags & CmpEW) != 0;
            o.cmplgtewl = (info.flags & CmpLW) != 0;
            o.eqval = (info.flags & EqV) != 0;
            o.pinned = (info.flags & Pin) != 0;
        }
    }
} optab_init_;
}  // namespace

// SubPool: per-Fn Memory Pool

void* subpool_alloc(SubPool* sp, size_t n) {
    if (n == 0)
        return nullptr;
    void* p = emalloc(n);
    sp->chunks.push_back(p);
    return p;
}

void subpool_free(SubPool* sp) {
    for (void* p : sp->chunks)
        free(p);
    sp->chunks.clear();
}

// Ctx Lifecycle

void* emalloc(size_t n) {
    void* p = calloc(1, n);
    if (!p)
        die("emalloc, out of memory");
    return p;
}

void Ctx::init() {
    assert(!insb && "Ctx::init called twice");
    insb = static_cast<Ins*>(emalloc(NIns * sizeof(Ins)));
    curi = insb + NIns;
}

void Ctx::bind(Fn* f) {
    assert(f && f->_subpool && "bind requires fn with attached SubPool");
    subpool = f->_subpool;
    curi = insb + NIns;
    load_curf = nullptr;
    load_inum = 0;
    load_ilog = nullptr;
    load_nlog = 0;
    ssa_namel = nullptr;
    gvn_con01[0] = Ref::none();
    gvn_con01[1] = Ref::none();
    gvn_tbl = nullptr;
    gvn_tbln = 0;
    rega_regu = 0;
    rega_tmp = nullptr;
    rega_npm = 0;
    rega_loop = 0;
    rega_stmov = 0;
    rega_stblk = 0;
    emit_stash = nullptr;
    spill_fp_live_peak = 0;
    fn = f;
}

void Ctx::release() {
    if (subpool) {
        subpool_free(subpool);
        delete subpool;
        subpool = nullptr;
    }
    fn = nullptr;
}

Ctx::~Ctx() {
    if (subpool) {
        subpool_free(subpool);
        delete subpool;
    }
    if (insb)
        free(insb);
    if (out_buf)
        fclose(out_buf);
    if (dbg_buf)
        fclose(dbg_buf);
    // ssa.cc Name pool is allocated via emalloc and reused across functions, ~Ctx must drain it.
    ssa_drain_namel(this);
}

// alloc / freeall

void* alloc(Ctx* ctx, size_t n) {
    assert(ctx && ctx->subpool);
    return subpool_alloc(ctx->subpool, n);
}

void freeall(Ctx* ctx) {
    assert(ctx && ctx->subpool);
    subpool_free(ctx->subpool);
}

// Growable Arrays (vnew / vgrow / vfree)

struct ArrayHdr {
    ulong magic;
    ulong cap;
    size_t esz;
    Pool pool;
};

static constexpr ulong AHDR_MAGIC = 0xA77A7007u;
static constexpr ulong VMIN_CAP = 2;

/// Retreat from user pointer to header
static ArrayHdr* hdr_of(void* p) {
    auto* h = static_cast<ArrayHdr*>(p) - 1;
    assert(h->magic == AHDR_MAGIC);
    return h;
}

void* vnew(Ctx* ctx, ulong len, size_t esz, Pool pl) {
    ulong cap = VMIN_CAP;
    while (cap < len)
        cap *= 2;

    size_t total = sizeof(ArrayHdr) + cap * esz;
    void* raw = (pl == PHeap) ? emalloc(total) : alloc(ctx, total);

    auto* h = static_cast<ArrayHdr*>(raw);
    h->magic = AHDR_MAGIC;
    h->cap = cap;
    h->esz = esz;
    h->pool = pl;
    return h + 1;
}

void vfree(void* p) {
    ArrayHdr* h = hdr_of(p);
    if (h->pool == PHeap) {
        h->magic = 0;
        free(h);
    }
}

void vgrow(Ctx* ctx, void* vp, ulong len) {
    void** pp = static_cast<void**>(vp);
    ArrayHdr* h = hdr_of(*pp);

    if (h->cap >= len)
        return;

    void* nv = vnew(ctx, len, h->esz, h->pool);
    memcpy(nv, *pp, h->cap * h->esz);
    vfree(*pp);
    *pp = nv;
}

// String Pool (intern / str)

uint32_t hash(const char* s) {
    uint32_t h = 5381;
    for (; *s; s++)
        h = ((h << 5) + h) ^ static_cast<uchar>(*s);  // djb2 xor variant
    return h;
}

/// Hash table slot
struct StrSlot {
    uint32_t hash;
    uint32_t id;
};

static constexpr uint32_t SLOT_EMPTY = UINT32_MAX;
static constexpr size_t HTAB_INIT = 1024;

static std::vector<char> char_buf;       // Continuous character storage
static std::vector<uint32_t> id_to_off;  // ID -> char_buf offset
static std::vector<StrSlot> htab;        // Open addressing hash table
static uint32_t htab_used;               // Number of occupied slots

/// Initialize hash table (triggered automatically on first intern call)
static void htab_init() {
    htab.assign(HTAB_INIT, {SLOT_EMPTY, 0});
    htab_used = 0;
}

/// Grow and rehash when load factor exceeds 70%
static void htab_grow() {
    size_t new_sz = htab.size() * 2;
    std::vector<StrSlot> old;
    old.swap(htab);
    htab.assign(new_sz, {SLOT_EMPTY, 0});

    uint32_t mask = static_cast<uint32_t>(new_sz - 1);
    for (auto& s : old) {
        if (s.hash == SLOT_EMPTY)
            continue;
        uint32_t pos = s.hash & mask;
        while (htab[pos].hash != SLOT_EMPTY)
            pos = (pos + 1) & mask;
        htab[pos] = s;
    }
}

uint32_t intern(const char* s) {
    if (htab.empty())
        htab_init();

    uint32_t h = hash(s);
    uint32_t mask = static_cast<uint32_t>(htab.size() - 1);
    uint32_t pos = h & mask;

    // Linear probing search
    while (htab[pos].hash != SLOT_EMPTY) {
        if (htab[pos].hash == h) {
            // hash identical, compare actual string
            uint32_t off = id_to_off[htab[pos].id];
            if (strcmp(s, char_buf.data() + off) == 0)
                return htab[pos].id;
        }
        pos = (pos + 1) & mask;
    }

    // Not found, insert new string
    uint32_t new_id = static_cast<uint32_t>(id_to_off.size());
    uint32_t off = static_cast<uint32_t>(char_buf.size());
    size_t len = strlen(s);
    char_buf.insert(char_buf.end(), s, s + len + 1);  // Includes '\0'
    id_to_off.push_back(off);

    htab[pos] = {h, new_id};
    htab_used++;

    // Load factor check
    if (htab_used * 10 > htab.size() * 7)
        htab_grow();

    return new_id;
}

char* str(uint32_t id) {
    assert(id < id_to_off.size());
    return char_buf.data() + id_to_off[id];
}

// strf: Format string and allocate to specified memory pool

char* strf(Ctx* ctx, Pool pl, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);

    char* p = static_cast<char*>((pl == PFn) ? alloc(ctx, static_cast<size_t>(n + 1))
                                             : emalloc(static_cast<size_t>(n + 1)));
    vsnprintf(p, static_cast<size_t>(n + 1), fmt, ap2);
    va_end(ap2);
    return p;
}

// die

// die context handover (emit phase has no explicit Ctx*, bypassed via thread_local)
static thread_local Ctx* tl_die_ctx = nullptr;

void set_die_ctx(Ctx* c) {
    tl_die_ctx = c;
}

const char* die_ctx_str(Ctx* ctx, Ins* cur) {
    static thread_local char buf[2048];
    char* p = buf;
    char* end = buf + sizeof(buf);
    if (!ctx)
        ctx = tl_die_ctx;
    if (!ctx || !ctx->fn) {
        snprintf(buf, sizeof(buf), "no_fn");
        return buf;
    }
    Fn* fn = ctx->fn;

    int fp_par = 0, gp_par = 0;
    if (fn->start)
        for (uint j = 0; j < fn->start->nins; ++j) {
            Ins* ii = &fn->start->ins[j];
            if (ii->op != Ocopy || !isreg(ii->arg[0]))
                break;
            if (KBASE(ii->cls) == 1)
                ++fp_par;
            else
                ++gp_par;
        }
    int ncall = 0, max_loop = 0;
    for (Blk* b = fn->start; b; b = b->link) {
        if (b->loop > max_loop)
            max_loop = b->loop;
        for (uint j = 0; j < b->nins; ++j)
            if (b->ins[j].op == Ocall)
                ++ncall;
    }
    p += snprintf(p, static_cast<size_t>(end - p),
                  "nblk=%u ntmp=%d ncon=%d slot=%d fpar=%d gpar=%d ncall=%d mxloop=%d fplive=%d",
                  fn->nblk, fn->ntmp, fn->ncon, fn->slot, fp_par, gp_par, ncall, max_loop,
                  ctx->spill_fp_live_peak);

    if (!cur)
        return buf;
    Blk* host = nullptr;
    uint pos = 0;
    for (Blk* b = fn->start; b; b = b->link) {
        if (cur >= b->ins && cur < b->ins + b->nins) {
            host = b;
            pos = static_cast<uint>(cur - b->ins);
            break;
        }
    }
    if (!host) {
        snprintf(p, static_cast<size_t>(end - p), " host=none");
        return buf;
    }
    p += snprintf(p, static_cast<size_t>(end - p), " blk%u/L%d nins=%u idx=%u ", host->id,
                  host->loop, host->nins, pos);
    uint lo = pos > 10 ? pos - 10 : 0;
    uint hi = pos + 10 < host->nins ? pos + 10 : host->nins - 1;
    for (uint j = lo; j <= hi && p < end - 32; ++j) {
        Ins* ii = &host->ins[j];
        p += snprintf(p, static_cast<size_t>(end - p), "%s%d/%c,", j == pos ? "*" : "", ii->op,
                      "wlsd"[ii->cls]);
    }
    return buf;
}

[[noreturn]] void die_(const char* file, const char* s, ...) {
    va_list ap;
    fprintf(stderr, "%s: dying: ", file);
    va_start(ap, s);
    vfprintf(stderr, s, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

[[noreturn]] void err(char* s, ...) {
    va_list ap;
    fprintf(stderr, "rcc: error: ");
    va_start(ap, s);
    vfprintf(stderr, s, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

// BSet Bitset Operations

void bsinit(Ctx* ctx, BSet* bs, uint n) {
    n = (n + NBit - 1) / NBit;
    bs->nt = n;
    bs->t = static_cast<bits*>(alloc(ctx, n * sizeof(bits)));
}

void bszero(BSet* bs) {
    memset(bs->t, 0, bs->nt * sizeof(bits));
}

uint bscount(BSet* bs) {
    uint c = 0;
    for (uint i = 0; i < bs->nt; i++) {
        bits b = bs->t[i];
        // Bitwise count: Hamming Weight (divide and conquer)
        b = b - ((b >> 1) & 0x5555555555555555ULL);
        b = (b & 0x3333333333333333ULL) + ((b >> 2) & 0x3333333333333333ULL);
        b = (b + (b >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
        c += static_cast<uint>((b * 0x0101010101010101ULL) >> 56);
    }
    return c;
}

void bsset(BSet* bs, uint elt) {
    assert(elt < bs->nt * NBit);
    bs->t[elt / NBit] |= BIT(elt % NBit);
}

void bsclr(BSet* bs, uint elt) {
    assert(elt < bs->nt * NBit);
    bs->t[elt / NBit] &= ~BIT(elt % NBit);
}

void bscopy(BSet* a, BSet* b) {
    assert(a->nt == b->nt);
    memcpy(a->t, b->t, a->nt * sizeof(bits));
}

void bsunion(BSet* a, BSet* b) {
    assert(a->nt == b->nt);
    for (uint i = 0; i < a->nt; i++)
        a->t[i] |= b->t[i];
}

void bsinter(BSet* a, BSet* b) {
    assert(a->nt == b->nt);
    for (uint i = 0; i < a->nt; i++)
        a->t[i] &= b->t[i];
}

void bsdiff(BSet* a, BSet* b) {
    assert(a->nt == b->nt);
    for (uint i = 0; i < a->nt; i++)
        a->t[i] &= ~b->t[i];
}

int bsequal(BSet* a, BSet* b) {
    assert(a->nt == b->nt);
    for (uint i = 0; i < a->nt; i++)
        if (a->t[i] != b->t[i])
            return 0;
    return 1;
}

/// Find index of lowest set bit in 64-bit word. For single bit (pow2) input, equivalent to log2.
int lowest_bit(bits b) {
    // De Bruijn sequence method
    static const int debruijn[64] = {
        0,  1,  2,  53, 3,  7,  54, 27, 4,  38, 41, 8,  34, 55, 48, 28, 62, 5,  39, 46, 44, 42,
        22, 9,  24, 35, 59, 56, 49, 18, 29, 11, 63, 52, 6,  26, 37, 40, 33, 47, 61, 45, 43, 21,
        23, 58, 17, 10, 51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12,
    };
    return debruijn[((b & (~b + 1)) * 0x022fdd63cc95386dULL) >> 58];
}

int bsiter(BSet* bs, int* elt) {
    uint i = static_cast<uint>(*elt);
    uint t = i / NBit;
    if (t >= bs->nt)
        return 0;

    bits b = bs->t[t] & ~(BIT(i % NBit) - 1);
    while (!b) {
        if (++t >= bs->nt)
            return 0;
        b = bs->t[t];
    }
    *elt = static_cast<int>(NBit * t + static_cast<uint>(lowest_bit(b)));
    return 1;
}

// Instruction Operation Helpers

void addins(Ctx* ctx, Ins** pi, uint* ni, Ins* ins) {
    if (ins->op == Onop)
        return;
    vgrow(ctx, pi, *ni + 1);
    (*pi)[*ni] = *ins;
    (*ni)++;
}

void addbins(Ctx* ctx, Ins** pi, uint* ni, Blk* b) {
    for (uint j = 0; j < b->nins; j++)
        addins(ctx, pi, ni, &b->ins[j]);
}

void emit(Ctx* ctx, int op, int k, Ref to, Ref a0, Ref a1) {
    if (ctx->curi == ctx->insb)
        die("emit, too many instructions (NIns=%d) op=%d cls=%d "
            "to=t%d/v%u a0=t%d/v%u a1=t%d/v%u || %s",
            NIns, op, k, rtype(to), to.val, rtype(a0), a0.val, rtype(a1), a1.val,
            die_ctx_str(ctx, nullptr));
    --ctx->curi;
    ctx->curi->op = static_cast<uint>(op);
    ctx->curi->cls = static_cast<uint>(k);
    ctx->curi->to = to;
    ctx->curi->arg[0] = a0;
    ctx->curi->arg[1] = a1;
}

void emiti(Ctx* ctx, Ins i) {
    emit(ctx, i.op, i.cls, i.to, i.arg[0], i.arg[1]);
}

Ins* icpy(Ins* dst, Ins* src, ulong n) {
    if (n)
        memmove(dst, src, n * sizeof(Ins));
    return dst + n;
}

void idup(Ctx* ctx, Blk* b, Ins* src, ulong n) {
    vgrow(ctx, &b->ins, n);
    icpy(b->ins, src, n);
    b->nins = static_cast<uint>(n);
}

void igroup(Blk* b, Ins* i, Ins** i0, Ins** i1) {
    Ins* ib = b->ins;
    Ins* ie = ib + b->nins;

    switch (i->op) {
        case Oblit0:
            *i0 = i;
            *i1 = i + 2;
            return;
        case Oblit1:
            *i0 = i - 1;
            *i1 = i + 1;
            return;
        case Osel0:
        case Osel1:
            // Backtrack to the leading sel0 from anywhere in the group, then consume consecutive
            // sel1s forward
            while (i->op == Osel1)
                --i;
            assert(i->op == Osel0);
            *i0 = i;
            for (++i; i < ie && i->op == Osel1; ++i)
                ;
            *i1 = i;
            return;
    }

    if (ispar(i->op)) {
        while (i > ib && ispar((i - 1)->op))
            --i;
        *i0 = i;
        while (i < ie && ispar(i->op))
            ++i;
        *i1 = i;
    } else if (i->op == Ocall || isarg(i->op)) {
        while (i > ib && isarg((i - 1)->op))
            --i;
        *i0 = i;
        while (i < ie && i->op != Ocall)
            ++i;
        assert(i < ie);
        *i1 = i + 1;
    } else {
        *i0 = i;
        *i1 = i + 1;
    }
}

int argcls(Ins* i, int n) {
    return optab[i->op].argcls[n][i->cls];
}

// Tmp / Con / Comparison Helpers

Ref newtmp(Ctx* ctx, const char* prfx, int k, Fn* fn) {
    static int cnt;
    int t = fn->ntmp++;
    vgrow(ctx, &fn->tmp, static_cast<ulong>(fn->ntmp));
    memset(&fn->tmp[t], 0, sizeof(Tmp));
    if (prfx)
        fn->tmp[t].name = strf(ctx, PFn, "%s.%d", prfx, ++cnt);
    fn->tmp[t].cls = static_cast<short>(k);
    fn->tmp[t].slot = -1;
    fn->tmp[t].nuse = 1;
    fn->tmp[t].ndef = 1;
    return Ref::tmp(static_cast<uint>(t));
}

void chuse(Ctx*, Ref r, int du, Fn* fn) {
    if (rtype(r) == RTmp)
        fn->tmp[r.val].nuse += static_cast<uint>(du);
}

int symeq(Sym a, Sym b) {
    return a.type == b.type && a.id == b.id;
}

Ref newcon(Ctx* ctx, Con* c0, Fn* fn) {
    for (int i = 1; i < fn->ncon; i++) {
        Con* c1 = &fn->con[i];
        if (c0->type == c1->type && symeq(c0->sym, c1->sym) && c0->bits.i == c1->bits.i)
            return Ref::con(static_cast<uint>(i));
    }
    int i = fn->ncon++;
    vgrow(ctx, &fn->con, static_cast<ulong>(fn->ncon));
    fn->con[i] = *c0;
    return Ref::con(static_cast<uint>(i));
}

Ref getcon(Ctx* ctx, int64_t val, Fn* fn) {
    for (int i = 1; i < fn->ncon; i++)
        if (fn->con[i].type == Con::CBits && fn->con[i].bits.i == val)
            return Ref::con(static_cast<uint>(i));
    int i = fn->ncon++;
    vgrow(ctx, &fn->con, static_cast<ulong>(fn->ncon));
    memset(&fn->con[i], 0, sizeof(Con));
    fn->con[i].type = Con::CBits;
    fn->con[i].bits.i = val;
    return Ref::con(static_cast<uint>(i));
}

int addcon(Con* c0, Con* c1, int m) {
    if (m != 1 && c1->type == Con::CAddr)
        return 0;
    if (c0->type == Con::CUndef) {
        *c0 = *c1;
        c0->bits.i *= m;
    } else {
        if (c1->type == Con::CAddr) {
            if (c0->type == Con::CAddr)
                return 0;
            c0->type = Con::CAddr;
            c0->sym = c1->sym;
        }
        c0->bits.i += c1->bits.i * m;
    }
    return 1;
}

int isconbits(Fn* fn, Ref r, int64_t* v) {
    if (rtype(r) == RCon) {
        Con* c = &fn->con[r.val];
        if (c->type == Con::CBits) {
            *v = c->bits.i;
            return 1;
        }
    }
    return 0;
}

int iscmp(int op, int* pk, int* pc) {
    if (Ocmpw <= op && op <= Ocmpw1) {
        *pc = op - Ocmpw;
        *pk = Kw;
    } else if (Ocmpl <= op && op <= Ocmpl1) {
        *pc = op - Ocmpl;
        *pk = Kl;
    } else if (Ocmps <= op && op <= Ocmps1) {
        *pc = NCmpI + op - Ocmps;
        *pk = Ks;
    } else if (Ocmpd <= op && op <= Ocmpd1) {
        *pc = NCmpI + op - Ocmpd;
        *pk = Kd;
    } else
        return 0;
    return 1;
}

/// Negation / Swap table for comparison operations
static const int cmptab[][2] = {
    /*           negate  swap  */
    /* Cieq  */ {Cine, Cieq},
    /* Cine  */ {Cieq, Cine},
    /* Cisge */ {Cislt, Cisle},
    /* Cisgt */ {Cisle, Cislt},
    /* Cisle */ {Cisgt, Cisge},
    /* Cislt */ {Cisge, Cisgt},
    /* Ciuge */ {Ciult, Ciule},
    /* Ciugt */ {Ciule, Ciult},
    /* Ciule */ {Ciugt, Ciuge},
    /* Ciult */ {Ciuge, Ciugt},
};

static const int fcmptab[][2] = {
    /*           negate  swap  */
    /* Cfeq  */ {-1, Cfeq},
    /* Cfge  */ {-1, Cfle},
    /* Cfgt  */ {-1, Cflt},
    /* Cfle  */ {-1, Cfge},
    /* Cflt  */ {-1, Cfgt},
    /* Cfne  */ {-1, Cfne},
    /* Cfo   */ {-1, Cfo},
    /* Cfuo  */ {-1, Cfuo},
};

int cmpop(int c) {
    assert(0 <= c && c < NCmp);
    if (c < NCmpI)
        return cmptab[c][1];
    return NCmpI + fcmptab[c - NCmpI][1];
}

int cmpwlneg(int op) {
    if (INRANGE(op, Ocmpw, Ocmpw1))
        return cmptab[op - Ocmpw][0] + Ocmpw;
    if (INRANGE(op, Ocmpl, Ocmpl1))
        return cmptab[op - Ocmpl][0] + Ocmpl;
    die("not a wl comparison");
}

int clsmerge(short* pk, short k) {
    short k1 = *pk;
    if (k1 == Kx) {
        *pk = k;
        return 0;
    }
    if ((k1 == Kw && k == Kl) || (k1 == Kl && k == Kw)) {
        *pk = Kw;
        return 0;
    }
    return k1 != k;
}

int phicls(int t, Tmp* tmp) {
    int t1 = tmp[t].phi;
    if (!t1)
        return t;
    t1 = phicls(t1, tmp);
    tmp[t].phi = t1;
    return t1;
}

uint phiargn(Phi* p, Blk* b) {
    if (p)
        for (uint n = 0; n < p->narg; n++)
            if (p->blk[n] == b)
                return n;
    return ~0u;
}

Ref phiarg(Phi* p, Blk* b) {
    uint n = phiargn(p, b);
    assert(n != ~0u && "block not found");
    return p->arg[n];
}

void dumpts(BSet* bs, Tmp* tmp, FILE* f) {
    int t;
    fprintf(f, "[");
    for (t = Tmp0; bsiter(bs, &t); t++)
        fprintf(f, " %s", tmp[t].name);
    fprintf(f, " ]\n");
}

}  // namespace rcc
