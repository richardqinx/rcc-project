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

#pragma once

#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rcc {

// Basic type aliases

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned long long bits;

// Forward declaration
struct BSet;
struct Ref;
struct Op;
struct Ins;
struct Phi;
struct Blk;
struct Use;
struct Sym;
struct Num;
struct Alias;
struct Tmp;
struct Con;
struct Fn;
struct Typ;
struct Field;
struct Dat;
struct Lnk;
struct Target;
struct SubPool;
struct Ctx;
struct Insert;   // load.cc
struct Name;     // ssa.cc
struct Asmbits;  // emit.cc
typedef struct Addr Mem;

enum {
    NIns = 1 << 20,
    NAlign = 3,
    NField = 32,
    NBit = CHAR_BIT * sizeof(bits),
    NPm = 64,  // Max copies for parallel move (rega)
};

struct Target {
    char name[16];
    char apple;
    char windows;
    int gpr0;
    int ngpr;
    int fpr0;
    int nfpr;
    bits rglob;
    int nrglob;
    int* rsave;
    int nrsave[2];
    bits (*retregs)(Ref, int[2]);
    bits (*argregs)(Ref, int[2]);
    int (*memargs)(int);
    void (*abi0)(Fn*);
    void (*abi1)(Ctx*, Fn*);
    void (*isel)(Ctx*, Fn*);
    void (*emitfn)(Fn*, FILE*);
    void (*emitfin)(Ctx*, FILE*);
    char asloc[4];
    char assym[4];
    uint cansel : 1;
};

#define BIT(n) (static_cast<bits>(1) << (n))

enum {
    RXX = 0,
    Tmp0 = NBit,
};

struct BSet {
    uint nt;
    bits* t;
};

enum {
    RTmp,
    RCon,
    RInt,
    RType, /* last kind to come out of the parser */
    RSlot,
    RCall,
    RMem,
};

struct Ref {
    uint type : 3;
    uint val : 29;

    // Factory methods
    static Ref none() { return {RTmp, 0}; }
    static Ref undef() { return {RCon, 0}; }
    static Ref con_z() { return {RCon, 1}; }
    static Ref tmp(uint v) { return {RTmp, v}; }
    static Ref con(uint v) { return {RCon, v}; }
    static Ref slot(int v) { return {RSlot, static_cast<uint>(v) & 0x1fffffffu}; }
    static Ref typ(uint v) { return {RType, v}; }
    static Ref call(uint v) { return {RCall, v}; }
    static Ref mem(uint v) { return {RMem, v}; }
    static Ref integer(int v) { return {RInt, static_cast<uint>(v) & 0x1fffffffu}; }

    bool operator==(Ref o) const { return type == o.type && val == o.val; }
    bool operator!=(Ref o) const { return !(*this == o); }
};

static inline int req(Ref a, Ref b) {
    return a.type == b.type && a.val == b.val;
}

static inline int rtype(Ref r) {
    if (req(r, Ref::none()))
        return -1;
    return r.type;
}

static inline int rsval(Ref r) {
    return (static_cast<int>(r.val) ^ 0x10000000) - 0x10000000;
}

static inline int isreg(Ref r) {
    return rtype(r) == RTmp && r.val < Tmp0;
}

enum CmpI {
    Cieq,
    Cine,
    Cisge,
    Cisgt,
    Cisle,
    Cislt,
    Ciuge,
    Ciugt,
    Ciule,
    Ciult,
    NCmpI,
};

enum CmpF {
    Cfeq,
    Cfge,
    Cfgt,
    Cfle,
    Cflt,
    Cfne,
    Cfo,
    Cfuo,
    NCmpF,
    NCmp = NCmpI + NCmpF,
};

enum { Kx = -1, Kw, Kl, Ks, Kd };

#define KWIDE(k) ((k) & 1)
#define KBASE(k) ((k) >> 1)

#include "ops.h"  // enum O { Oxxx, ..., NOp } + OpInfo / kops[] single source attribute table

enum J {
    Jxxx,
#define JMPS(X)                                                                             \
    X(retw)                                                                                 \
    X(retl)                                                                                 \
    X(rets)                                                                                 \
    X(retd)                                                                                 \
    X(retsb)                                                                                \
    X(retub)                                                                                \
    X(retsh)                                                                                \
    X(retuh)                                                                                \
    X(retc)                                                                                 \
    X(ret0)                                                                                 \
    X(jmp)                                                                                  \
    X(jnz)                                                                                  \
    X(jfieq)                                                                                \
    X(jfine)                                                                                \
    X(jfisge)                                                                               \
    X(jfisgt) X(jfisle) X(jfislt) X(jfiuge) X(jfiugt) X(jfiule) X(jfiult) X(jffeq) X(jffge) \
        X(jffgt) X(jffle) X(jfflt) X(jffne) X(jffo) X(jffuo) X(hlt)
#define X(j) J##j,
    JMPS(X)
#undef X
        NJmp
};

enum {
    Ocmpw = Oceqw,
    Ocmpw1 = Ocultw,
    Ocmpl = Oceql,
    Ocmpl1 = Ocultl,
    Ocmps = Oceqs,
    Ocmps1 = Ocuos,
    Ocmpd = Oceqd,
    Ocmpd1 = Ocuod,
    Oalloc = Oalloc4,
    Oalloc1 = Oalloc16,
    Oflag = Oflagieq,
    Oflag1 = Oflagfuo,
    Oxsel = Oxselieq,
    Oxsel1 = Oxselfuo,
    NPubOp = Onop,
    Jjf = Jjfieq,
    Jjf1 = Jjffuo,
};

#define INRANGE(x, l, u)                                    \
    (static_cast<unsigned>(x) - static_cast<unsigned>(l) <= \
     static_cast<unsigned>(u) - static_cast<unsigned>(l))
#define isstore(o) INRANGE(o, Ostoreb, Ostored)
#define isload(o) INRANGE(o, Oloadsb, Oload)
#define isalloc(o) INRANGE(o, Oalloc4, Oalloc16)
#define isext(o) INRANGE(o, Oextsb, Oextuw)
#define ispar(o) INRANGE(o, Opar, Opare)
#define isarg(o) INRANGE(o, Oarg, Oargv)
#define isret(j) INRANGE(j, Jretw, Jret0)
#define isparbh(o) INRANGE(o, Oparsb, Oparuh)
#define isargbh(o) INRANGE(o, Oargsb, Oarguh)
#define isretbh(j) INRANGE(j, Jretsb, Jretuh)
#define isxsel(o) INRANGE(o, Oxsel, Oxsel1)

struct Op {
    char* name;
    short argcls[2][4];
    uint canfold : 1;
    uint hasid : 1;
    uint idval : 1;
    uint commutes : 1;
    uint assoc : 1;
    uint idemp : 1;
    uint cmpeqwl : 1;
    uint cmplgtewl : 1;
    uint eqval : 1;
    uint pinned : 1;
};

struct Ins {
    uint op : 30;
    uint cls : 2;
    Ref to;
    Ref arg[2];
};

struct Phi {
    Ref to;
    short cls;
    int visit;
    uint narg;
    Ref* arg;
    Blk** blk;
    Phi* link;
};

struct Blk {
    Phi* phi;
    Ins* ins;
    uint nins;
    struct {
        short type;
        short cls;  // Jjfi* / Jjff* carried cmp operand cls; other jmp types are 0
        Ref arg;
        Ref arg2;  // Used only by Jjfi* / Jjff* (carries 2nd cmp operand when fusing cmp+jnz)
    } jmp;
    Blk* s1;
    Blk* s2;
    Blk* link;

    uint id;
    uint visit;

    Blk* idom;
    Blk *dom, *dlink;
    Blk** fron;
    uint nfron;
    int depth;

    Blk** pred;
    uint npred;
    BSet in[1], out[1], gen[1];
    int nlive[2];
    int loop;
    char* name;
};

struct Use {
    enum {
        UXXX,
        UPhi,
        UIns,
        UJmp,
    } type;
    uint bid;
    union {
        Ins* ins;
        Phi* phi;
    } u;
};

struct Sym {
    enum {
        SGlo = 0,
        SThr = 1,
        SExt = 2,
        SExtThr = SExt | SThr,
    } type;
    uint32_t id;
};

struct Num {
    uchar n;
    uchar nl, nr;
    Ref l, r;
};

enum { NoAlias, MayAlias, MustAlias };

struct Alias {
    enum {
        ABot = 0,
        ALoc = 1,
        ACon = 2,
        AEsc = 3,
        ASym = 4,
        AUnk = 6,
#define astack(t) ((t) & 1)
    } type;
    int base;
    int64_t offset;
    // True only for ASym path: Oadd(ASym, AUnk) preserves symbol identity, but offset is
    // unreliable. Makes alias() degrade to MayAlias on "same symbol / same base" branch, while
    // "different ASym" can still NoAlias. Not propagated to ALoc/AEsc: stack slot offset is
    // controlled by IRGen and always known.
    bool offset_unknown;
    union {
        Sym sym;
        struct {
            int sz;
            bits m;
        } loc;
    } u;
    Alias* slot;
};

struct Tmp {
    char* name;
    Ins* def;
    Use* use;
    uint ndef, nuse;
    uint bid;
    uint cost;
    int slot;
    short cls;
    struct {
        int r;
        int w;
        bits m;
    } hint;
    int phi;
    Alias alias;
    enum { WFull, Wsb, Wub, Wsh, Wuh, Wsw, Wuw } width;
    int visit;
    uint gcmbid;
};

struct Con {
    enum {
        CUndef,
        CBits,
        CAddr,
    } type;
    Sym sym;
    union {
        int64_t i;
        double d;
        float s;
    } bits;
    char flt;
};

struct Addr {
    Con offset;
    Ref base;
    Ref index;
    int scale;
};

struct Lnk {
    char export_;  // 'export' is C++ reserved word (C++20 module)
    char thread;
    char common;
    char align;
    char* sec;
    char* secf;
};

struct Fn {
    Blk* start;
    Tmp* tmp;
    Con* con;
    Mem* mem;
    int ntmp;
    int ncon;
    int nmem;
    uint nblk;
    int retty;
    Ref retr;
    Blk** rpo;
    bits reg;
    int slot;
    int salign;
    char vararg;
    char dynalloc;
    char leaf;
    char* name;
    Lnk lnk;
    SubPool* _subpool;  // Memory pool ownership for this fn; parse attaches it when generating fn,
                        // worker calls subpool_free after processing
};

struct Field {
    enum {
        FEnd,
        Fb,
        Fh,
        Fw,
        Fl,
        Fs,
        Fd,
        FPad,
        FTyp,
    } type;
    uint len;
};

struct Typ {
    char* name;
    char isdark;
    char isunion;
    int align;
    uint64_t size;
    uint nunion;
    Field (*fields)[NField + 1];
};

struct Dat {
    enum { DStart, DEnd, DB, DH, DW, DL, DZ } type;
    char* name;
    Lnk* lnk;
    union {
        int64_t num;
        double fltd;
        float flts;
        char* str;
        struct {
            char* name;
            int64_t off;
        } ref;
    } u;
    char isref;
    char isstr;
};

extern Target T;
extern char debug['Z' + 1];

typedef enum {
    PHeap,
    PFn,
} Pool;

// SubPool / Ctx

struct SubPool {
    std::vector<void*> chunks;  // Each chunk is a pointer from emalloc
};

void* subpool_alloc(SubPool*, size_t);
void subpool_free(SubPool*);

struct Ctx {
    SubPool* subpool = nullptr;

    Ins* insb = nullptr;  // 8MB, emalloc during init(); freed during ~Ctx
    Ins* curi = nullptr;  // Reverse fill pointer, reset to insb + NIns during bind()

    Fn* load_curf = nullptr;
    uint load_inum = 0;
    Insert* load_ilog = nullptr;
    uint load_nlog = 0;

    Name* ssa_namel = nullptr;

    Ref gvn_con01[2]{};
    Ins** gvn_tbl = nullptr;
    uint gvn_tbln = 0;

    bits rega_regu = 0;
    Tmp* rega_tmp = nullptr;
    Mem* rega_mem = nullptr;
    struct {
        Ref src, dst;
        int cls;
    } rega_pm[NPm]{};
    int rega_npm = 0;
    int rega_loop = 0;
    uint rega_stmov = 0, rega_stblk = 0;

    Asmbits* emit_stash = nullptr;

    // die context: during spill, record "peak FPR live set when limit_one"; dumped during emit die
    int spill_fp_live_peak = 0;

    FILE* out_buf = nullptr;
    FILE* dbg_buf = nullptr;

    Fn* fn = nullptr;

    void init();     // Allocate insb (8MB) and initial SubPool; other fields zero-initialized
    void bind(Fn*);  // Take over fn's SubPool, reset curi and per-Fn pass states
    void release();  // Release current SubPool, clear states; do not release insb
    ~Ctx();          // Release insb and remaining SubPool
};

extern Typ* typ;
extern Op optab[NOp];

// util.cc

uint32_t hash(const char*);
[[noreturn]] void die_(const char*, const char*, ...) __attribute__((format(printf, 2, 3)));

// die-context utilities
struct Ins;
void set_die_ctx(Ctx*);
const char* die_ctx_str(Ctx*, Ins* cur);  // cur can be nullptr
void* emalloc(size_t);
void* alloc(Ctx*, size_t);
void freeall(Ctx*);
void* vnew(Ctx*, ulong, size_t, Pool);
void vfree(void*);
void vgrow(Ctx*, void*, ulong);

/// Lazy-allocating vgrow
inline void vensure(Ctx* ctx, void* vp, ulong len, size_t esz, Pool pl) {
    void** pp = static_cast<void**>(vp);
    if (*pp == nullptr)
        *pp = vnew(ctx, len, esz, pl);
    else
        vgrow(ctx, vp, len);
}
void addins(Ctx*, Ins**, uint*, Ins*);
void addbins(Ctx*, Ins**, uint*, Blk*);
char* strf(Ctx*, Pool, const char*, ...) __attribute__((format(printf, 3, 4)));
uint32_t intern(const char*);
char* str(uint32_t);
int argcls(Ins*, int);
int iscmp(int, int*, int*);
void igroup(Blk*, Ins*, Ins**, Ins**);
void emit(Ctx*, int, int, Ref, Ref, Ref);
void emiti(Ctx*, Ins);
void idup(Ctx*, Blk*, Ins*, ulong);
Ins* icpy(Ins*, Ins*, ulong);
int cmpop(int);
int cmpwlneg(int);
int clsmerge(short*, short);
int phicls(int, Tmp*);
uint phiargn(Phi*, Blk*);
Ref phiarg(Phi*, Blk*);
Ref newtmp(Ctx*, const char*, int, Fn*);
void chuse(Ctx*, Ref, int, Fn*);
int symeq(Sym, Sym);
Ref newcon(Ctx*, Con*, Fn*);
Ref getcon(Ctx*, int64_t, Fn*);
int addcon(Con*, Con*, int);
int isconbits(Fn* fn, Ref r, int64_t* v);
void salloc(Ctx*, Ref, Ref, Fn*);
int lowest_bit(bits);
void dumpts(BSet*, Tmp*, FILE*);
void runmatch(uchar*, Num*, Ref, Ref*);

void bsinit(Ctx*, BSet*, uint);
void bszero(BSet*);
uint bscount(BSet*);
void bsset(BSet*, uint);
void bsclr(BSet*, uint);
void bscopy(BSet*, BSet*);
void bsunion(BSet*, BSet*);
void bsinter(BSet*, BSet*);
void bsdiff(BSet*, BSet*);
int bsequal(BSet*, BSet*);
int bsiter(BSet*, int*);

static inline int bshas(BSet* bs, uint elt) {
    assert(elt < bs->nt * NBit);
    return (bs->t[elt / NBit] & BIT(elt % NBit)) != 0;
}

// parse.cc

void parse(FILE*, char*, void (*)(char*), void (*)(Dat*), void (*)(Fn*));
void printfn(Fn*, FILE*);
void printref(Ref, Fn*, FILE*);
[[noreturn]] void err(char*, ...) __attribute__((format(printf, 1, 2)));

// abi.cc

void elimsb(Fn*);

// cfg.cc - CFG analysis and simplification (Ctx*, Fn*) conventions

Blk* newblk(Ctx*);
void fillpreds(Ctx*, Fn*);
void fillcfg(Ctx*, Fn*);
void filldom(Fn*);
int sdom(Blk*, Blk*);
int dom(Blk*, Blk*);
void fillfron(Ctx*, Fn*);
void loopiter(Fn*, void (*)(Blk*, Blk*));
void filldepth(Fn*);
Blk* lca(Blk*, Blk*);
void fillloop(Fn*);
void simpljmp(Ctx*, Fn*);
int reaches(Fn*, Blk*, Blk*);
int reachesnotvia(Fn*, Blk*, Blk*, Blk*);
int ifgraph(Blk*, Blk**, Blk**, Blk**);
void simplcfg(Ctx*, Fn*);

// mem.cc

void promote(Ctx*, Fn*);
void coalesce(Ctx*, Fn*);

// alias.cc

void fillalias(Fn*);
void getalias(Alias*, Ref, Fn*);
int alias(Ref, int, int, Ref, int, int*, Fn*);
int escapes(Ref, Fn*);

// load.cc

int loadsz(Ins*);
int storesz(Ins*);
void loadopt(Ctx*, Fn*);

// ssa.cc - Use chain population / SSA construction / SSA invariant verification

void adduse_ins(Ctx*, Tmp*, Blk*, Ins*);
void adduse_phi(Ctx*, Tmp*, Blk*, Phi*);
void adduse_jmp(Ctx*, Tmp*, Blk*);
void filluse(Ctx*, Fn*);
void ssa(Ctx*, Fn*);
void ssacheck(Fn*);
void ssa_drain_namel(Ctx*);  // Called by ~Ctx to drain emalloc nodes from ssa_namel free list

// copy.cc

void narrowpars(Ctx*, Fn*);
Ref copyref(Ctx*, Fn*, Blk*, Ins*);
Ref phicopyref(Ctx*, Fn*, Blk*, Phi*);

// fold.cc

int foldint(Con*, int, int, Con*, Con*);
Ref foldref(Ctx*, Fn*, Ins*);

// gvn.cc
//

int zeroval(Fn*, Blk*, Ref, int, int*);
void gvn(Ctx*, Fn*);

// gcm.cc

int pinned(Ins*);
void gcm(Ctx*, Fn*);

// ifopt.cc

void ifconvert(Ctx*, Fn*);

// divopt.cc

void divopt(Ctx*, Fn*);

// mulopt.cc - Constant multiplication strength reduction (x * c -> <=3 shift/add/sub/neg).

void mulopt(Ctx*, Fn*);

// storemrg.cc - Merge adjacent same-base 4B-zero stores into 8B storel.

void storemrg(Ctx*, Fn*);

// licm.cc

void licm(Ctx*, Fn*);

// scalar_repl.cc - Hoist intra-loop same-address load+store accumulators to reg + phi.

void scalar_repl(Ctx*, Fn*);

// unroll.cc - Loop unrolling (Phase 1: Full unroll for constant trips).

void unroll(Ctx*, Fn*);

// tre.cc - Tail recursion elimination (Phase 1: Self-recursion + normalized tail call form).

void tre(Ctx*, Fn*);

// lsr.cc - Loop strength reduction (Phase 1: Constant basic induction variable * constant).

void lsr(Ctx*, Fn*);

// loop_idiom.cc - memset / memcpy / memmove loop idiom recognition -> Rewrite as Ocall to builtin
// runtime.

void loop_idiom(Ctx*, Fn*);

// reduction_unroll.cc - Integer reduction loop K-way parallel accumulator unrolling, breaking
// serial dependency chains.

void reduction_unroll(Ctx*, Fn*);

// loopinterchange.cc - Integer perfect 2-level nested loop interchange (row-major column access ->
// row access).

void loopinterchange(Ctx*, Fn*);

// unswitch.cc - Loop unswitching (hoisting if (loop_invariant_cond) out of loop).

void unswitch(Ctx*, Fn*);

// sccp.cc — Sparse Conditional Constant Propagation(Wegman-Zadeck)。

void sccp(Ctx*, Fn*);

// reassoc.cc - Expression reassociation (rank-based, constant gathering + normalization + common
// term elimination).

void reassoc(Ctx*, Fn*);

// dce.cc - Global dead code elimination (mark-and-sweep, roots = side-effect Ins + jmp.arg).

void dce(Ctx*, Fn*);

// dead_loop.cc - Dead loop elimination (no side-effects + provably terminating + no live-out loop).

void dead_loop(Ctx*, Fn*);

// inline.cc - Function inlining (leaf-only + fixpoint), whole-module perspective, called once after
// IRGen and before run_midend.

void inline_module(Ctx*, std::vector<Fn*>&);

// simpl.cc

void simpl(Ctx*, Fn*);

// live.cc - Backward dataflow liveness analysis

void liveon(BSet*, Blk*, Blk*);
void filllive(Ctx*, Fn*);

// spill.cc

void fillcost(Ctx*, Fn*);
void spill(Ctx*, Fn*);

// rega.cc

void rega(Ctx*, Fn*);

// peephole.cc

void peephole(Ctx*, Fn*);

// emit.cc

void emitfnlnk(char*, Lnk*, FILE*);
void emitdat(Dat*, FILE*);
void emitdbgfile(char*, FILE*);
void emitdbgloc(uint, uint, FILE*);
int stashbits(Ctx*, bits, int);
void elf_emitfnfin(char*, FILE*);
void elf_emitfin(Ctx*, FILE*);
void macho_emitfin(Ctx*, FILE*);
void pe_emitfin(Ctx*, FILE*);
void rv64_emitfin(Ctx*, FILE*);

// macro
#define die(...) die_(__FILE__, __VA_ARGS__)
#define MAKESURE(what, x) static_assert((x), #what)

}  // namespace rcc
