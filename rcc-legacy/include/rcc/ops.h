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

// See docs/ops.md for architectural notes.

enum O {
    Oxxx,

    Oadd,
    Osub,
    Oneg,
    Odiv,
    Orem,
    Oudiv,
    Ourem,
    Omul,
    Oand,
    Oor,
    Oxor,
    Osar,
    Oshr,
    Oshl,

    Oceqw,
    Ocnew,
    Ocsgew,
    Ocsgtw,
    Ocslew,
    Ocsltw,
    Ocugew,
    Ocugtw,
    Oculew,
    Ocultw,
    Oceql,
    Ocnel,
    Ocsgel,
    Ocsgtl,
    Ocslel,
    Ocsltl,
    Ocugel,
    Ocugtl,
    Oculel,
    Ocultl,
    Oceqs,
    Ocges,
    Ocgts,
    Ocles,
    Oclts,
    Ocnes,
    Ocos,
    Ocuos,
    Oceqd,
    Ocged,
    Ocgtd,
    Ocled,
    Ocltd,
    Ocned,
    Ocod,
    Ocuod,

    Ostoreb,
    Ostoreh,
    Ostorew,
    Ostorel,
    Ostores,
    Ostored,
    Oloadsb,
    Oloadub,
    Oloadsh,
    Oloaduh,
    Oloadsw,
    Oloaduw,
    Oload,

    Oextsb,
    Oextub,
    Oextsh,
    Oextuh,
    Oextsw,
    Oextuw,
    Oexts,
    Otruncd,
    Ostosi,
    Ostoui,
    Odtosi,
    Odtoui,
    Oswtof,
    Ouwtof,
    Osltof,
    Oultof,
    Ocast,

    Oalloc4,
    Oalloc8,
    Oalloc16,
    Ovaarg,
    Ovastart,
    Ocopy,
    Odbgloc,

    // The following are internal ops (Onop must be first)

    Onop,
    Oaddr,
    Oblit0,
    Oblit1,
    Osel0,
    Osel1,
    Oswap,
    Osign,
    Osalloc,
    Oxidiv,
    Oxdiv,
    Oxcmp,
    Oxtest,
    Oacmp,
    Oacmn,
    Oafcmp,
    Oreqz,
    Ornez,

    Opar,
    Oparsb,
    Oparub,
    Oparsh,
    Oparuh,
    Oparc,
    Opare,
    Oarg,
    Oargsb,
    Oargub,
    Oargsh,
    Oarguh,
    Oargc,
    Oarge,
    Oargv,
    Ocall,

    Oflagieq,
    Oflagine,
    Oflagisge,
    Oflagisgt,
    Oflagisle,
    Oflagislt,
    Oflagiuge,
    Oflagiugt,
    Oflagiule,
    Oflagiult,
    Oflagfeq,
    Oflagfge,
    Oflagfgt,
    Oflagfle,
    Oflagflt,
    Oflagfne,
    Oflagfo,
    Oflagfuo,

    Oxselieq,
    Oxseline,
    Oxselisge,
    Oxselisgt,
    Oxselisle,
    Oxselislt,
    Oxseliuge,
    Oxseliugt,
    Oxseliule,
    Oxseliult,
    Oxselfeq,
    Oxselfge,
    Oxselfgt,
    Oxselfle,
    Oxselflt,
    Oxselfne,
    Oxselfo,
    Oxselfuo,

    NOp,
};

// Placeholders in argcls: Ke means (op,cls) is illegal; Km means memory pointer (same as Kl
// structurally, used for semantic tagging).
enum { Ke = -2, Km = Kl };

enum OpFlags : unsigned {
    Fold = 1u << 0,
    Id = 1u << 1,
    IdV = 1u << 2,
    Comm = 1u << 3,
    Assoc = 1u << 4,
    Idemp = 1u << 5,
    CmpEW = 1u << 6,
    CmpLW = 1u << 7,
    EqV = 1u << 8,
    Pin = 1u << 9,
};

struct OpInfo {
    const char* name;
    short argcls[2][4];
    unsigned flags;
    unsigned char imm;  // rv64 I-form right operand can use 12-bit immediate
};

inline constexpr auto kops = [] {
    std::array<OpInfo, NOp> a{};
    a[Oxxx] = {"", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, 0, 0};

    a[Oadd] = {"add", {{Kw, Kl, Ks, Kd}, {Kw, Kl, Ks, Kd}}, Fold | Id | Comm | Assoc, 1};
    a[Osub] = {"sub", {{Kw, Kl, Ks, Kd}, {Kw, Kl, Ks, Kd}}, Fold | Id, 0};
    a[Oneg] = {"neg", {{Kw, Kl, Ks, Kd}, {Kx, Kx, Kx, Kx}}, Fold, 0};
    a[Odiv] = {"div", {{Kw, Kl, Ks, Kd}, {Kw, Kl, Ks, Kd}}, Fold | Id | IdV, 0};
    a[Orem] = {"rem", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Fold, 0};
    a[Oudiv] = {"udiv", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Fold | Id | IdV, 0};
    a[Ourem] = {"urem", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Fold, 0};
    a[Omul] = {"mul", {{Kw, Kl, Ks, Kd}, {Kw, Kl, Ks, Kd}}, Fold | Id | IdV | Comm, 0};
    a[Oand] = {"and", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Fold | Comm | Assoc | Idemp, 1};
    a[Oor] = {"or", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Fold | Id | Comm | Assoc | Idemp, 1};
    a[Oxor] = {"xor", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Fold | Id | Comm | Assoc, 1};
    a[Osar] = {"sar", {{Kw, Kl, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | Id, 1};
    a[Oshr] = {"shr", {{Kw, Kl, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | Id, 1};
    a[Oshl] = {"shl", {{Kw, Kl, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | Id, 1};

    a[Oceqw] = {
        "ceqw", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | Id | IdV | Comm | CmpEW | EqV, 0};
    a[Ocnew] = {"cnew", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | Id | Comm | CmpEW, 0};
    a[Ocsgew] = {"csgew", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocsgtw] = {"csgtw", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW, 0};
    a[Ocslew] = {"cslew", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocsltw] = {"csltw", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW, 1};
    a[Ocugew] = {"cugew", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocugtw] = {"cugtw", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW, 0};
    a[Oculew] = {"culew", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocultw] = {"cultw", {{Kw, Kw, Ke, Ke}, {Kw, Kw, Ke, Ke}}, Fold | CmpLW, 1};
    a[Oceql] = {"ceql", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | Comm | CmpEW | EqV, 0};
    a[Ocnel] = {"cnel", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | Comm | CmpEW, 0};
    a[Ocsgel] = {"csgel", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocsgtl] = {"csgtl", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW, 0};
    a[Ocslel] = {"cslel", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocsltl] = {"csltl", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW, 1};
    a[Ocugel] = {"cugel", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocugtl] = {"cugtl", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW, 0};
    a[Oculel] = {"culel", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW | EqV, 0};
    a[Ocultl] = {"cultl", {{Kl, Kl, Ke, Ke}, {Kl, Kl, Ke, Ke}}, Fold | CmpLW, 1};
    a[Oceqs] = {"ceqs", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold | Comm, 0};
    a[Ocges] = {"cges", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold, 0};
    a[Ocgts] = {"cgts", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold, 0};
    a[Ocles] = {"cles", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold, 0};
    a[Oclts] = {"clts", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold, 0};
    a[Ocnes] = {"cnes", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold | Comm, 0};
    a[Ocos] = {"cos", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold | Comm, 0};
    a[Ocuos] = {"cuos", {{Ks, Ks, Ke, Ke}, {Ks, Ks, Ke, Ke}}, Fold | Comm, 0};
    a[Oceqd] = {"ceqd", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold | Comm, 0};
    a[Ocged] = {"cged", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold, 0};
    a[Ocgtd] = {"cgtd", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold, 0};
    a[Ocled] = {"cled", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold, 0};
    a[Ocltd] = {"cltd", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold, 0};
    a[Ocned] = {"cned", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold | Comm, 0};
    a[Ocod] = {"cod", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold | Comm, 0};
    a[Ocuod] = {"cuod", {{Kd, Kd, Ke, Ke}, {Kd, Kd, Ke, Ke}}, Fold | Comm, 0};

    a[Ostoreb] = {"storeb", {{Kw, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Ostoreh] = {"storeh", {{Kw, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Ostorew] = {"storew", {{Kw, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Ostorel] = {"storel", {{Kl, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Ostores] = {"stores", {{Ks, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Ostored] = {"stored", {{Kd, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Oloadsb] = {"loadsb", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Pin, 0};
    a[Oloadub] = {"loadub", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Pin, 0};
    a[Oloadsh] = {"loadsh", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Pin, 0};
    a[Oloaduh] = {"loaduh", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Pin, 0};
    a[Oloadsw] = {"loadsw", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Pin, 0};
    a[Oloaduw] = {"loaduw", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Pin, 0};
    a[Oload] = {"load", {{Km, Km, Km, Km}, {Kx, Kx, Kx, Kx}}, Pin, 0};

    a[Oextsb] = {"extsb", {{Kw, Kw, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Oextub] = {"extub", {{Kw, Kw, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Oextsh] = {"extsh", {{Kw, Kw, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Oextuh] = {"extuh", {{Kw, Kw, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Oextsw] = {"extsw", {{Ke, Kw, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Fold, 0};
    a[Oextuw] = {"extuw", {{Ke, Kw, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Fold, 0};
    a[Oexts] = {"exts", {{Ke, Ke, Ke, Ks}, {Ke, Ke, Ke, Kx}}, Fold, 0};
    a[Otruncd] = {"truncd", {{Ke, Ke, Kd, Ke}, {Ke, Ke, Kx, Ke}}, Fold, 0};
    a[Ostosi] = {"stosi", {{Ks, Ks, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Ostoui] = {"stoui", {{Ks, Ks, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Odtosi] = {"dtosi", {{Kd, Kd, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Odtoui] = {"dtoui", {{Kd, Kd, Ke, Ke}, {Kx, Kx, Ke, Ke}}, Fold, 0};
    a[Oswtof] = {"swtof", {{Ke, Ke, Kw, Kw}, {Ke, Ke, Kx, Kx}}, Fold, 0};
    a[Ouwtof] = {"uwtof", {{Ke, Ke, Kw, Kw}, {Ke, Ke, Kx, Kx}}, Fold, 0};
    a[Osltof] = {"sltof", {{Ke, Ke, Kl, Kl}, {Ke, Ke, Kx, Kx}}, Fold, 0};
    a[Oultof] = {"ultof", {{Ke, Ke, Kl, Kl}, {Ke, Ke, Kx, Kx}}, Fold, 0};
    a[Ocast] = {"cast", {{Ks, Kd, Kw, Kl}, {Kx, Kx, Kx, Kx}}, Fold, 0};

    a[Oalloc4] = {"alloc4", {{Ke, Kl, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Pin, 0};
    a[Oalloc8] = {"alloc8", {{Ke, Kl, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Pin, 0};
    a[Oalloc16] = {"alloc16", {{Ke, Kl, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Pin, 0};

    a[Ovaarg] = {"vaarg", {{Km, Km, Km, Km}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Ovastart] = {"vastart", {{Km, Ke, Ke, Ke}, {Kx, Ke, Ke, Ke}}, Pin, 0};

    a[Ocopy] = {"copy", {{Kw, Kl, Ks, Kd}, {Kx, Kx, Kx, Kx}}, 0, 0};
    a[Odbgloc] = {"dbgloc", {{Kw, Ke, Ke, Ke}, {Kw, Ke, Ke, Ke}}, Pin, 0};

    a[Onop] = {"nop", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, 0, 0};
    a[Oaddr] = {"addr", {{Km, Km, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oblit0] = {"blit0", {{Km, Ke, Ke, Ke}, {Km, Ke, Ke, Ke}}, Pin, 0};
    a[Oblit1] = {"blit1", {{Kw, Ke, Ke, Ke}, {Kx, Ke, Ke, Ke}}, Pin, 0};
    a[Osel0] = {"sel0", {{Kw, Ke, Ke, Ke}, {Kx, Ke, Ke, Ke}}, Pin, 0};
    a[Osel1] = {"sel1", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, Pin, 0};
    a[Oswap] = {"swap", {{Kw, Kl, Ks, Kd}, {Kw, Kl, Ks, Kd}}, 0, 0};
    a[Osign] = {"sign", {{Kw, Kl, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Osalloc] = {"salloc", {{Ke, Kl, Ke, Ke}, {Ke, Kx, Ke, Ke}}, 0, 0};
    a[Oxidiv] = {"xidiv", {{Kw, Kl, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oxdiv] = {"xdiv", {{Kw, Kl, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oxcmp] = {"xcmp", {{Kw, Kl, Ks, Kd}, {Kw, Kl, Ks, Kd}}, 0, 0};
    a[Oxtest] = {"xtest", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oacmp] = {"acmp", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oacmn] = {"acmn", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oafcmp] = {"afcmp", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oreqz] = {"reqz", {{Kw, Kl, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Ornez] = {"rnez", {{Kw, Kl, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};

    a[Opar] = {"par", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oparsb] = {"parsb", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oparub] = {"parub", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oparsh] = {"parsh", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oparuh] = {"paruh", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oparc] = {"parc", {{Ke, Kx, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Pin, 0};
    a[Opare] = {"pare", {{Ke, Kx, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Pin, 0};
    a[Oarg] = {"arg", {{Kw, Kl, Ks, Kd}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oargsb] = {"argsb", {{Kw, Ke, Ke, Ke}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oargub] = {"argub", {{Kw, Ke, Ke, Ke}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oargsh] = {"argsh", {{Kw, Ke, Ke, Ke}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oarguh] = {"arguh", {{Kw, Ke, Ke, Ke}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Oargc] = {"argc", {{Ke, Kx, Ke, Ke}, {Ke, Kl, Ke, Ke}}, Pin, 0};
    a[Oarge] = {"arge", {{Ke, Kl, Ke, Ke}, {Ke, Kx, Ke, Ke}}, Pin, 0};
    a[Oargv] = {"argv", {{Kx, Kx, Kx, Kx}, {Kx, Kx, Kx, Kx}}, Pin, 0};
    a[Ocall] = {"call", {{Km, Km, Km, Km}, {Kx, Kx, Kx, Kx}}, Pin, 0};

    a[Oflagieq] = {"flagieq", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagine] = {"flagine", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagisge] = {"flagisge", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagisgt] = {"flagisgt", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagisle] = {"flagisle", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagislt] = {"flagislt", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagiuge] = {"flagiuge", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagiugt] = {"flagiugt", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagiule] = {"flagiule", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagiult] = {"flagiult", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfeq] = {"flagfeq", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfge] = {"flagfge", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfgt] = {"flagfgt", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfle] = {"flagfle", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagflt] = {"flagflt", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfne] = {"flagfne", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfo] = {"flagfo", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};
    a[Oflagfuo] = {"flagfuo", {{Kx, Kx, Ke, Ke}, {Kx, Kx, Ke, Ke}}, 0, 0};

    a[Oxselieq] = {"xselieq", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxseline] = {"xseline", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxselisge] = {"xselisge", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxselisgt] = {"xselisgt", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxselisle] = {"xselisle", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxselislt] = {"xselislt", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxseliuge] = {"xseliuge", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxseliugt] = {"xseliugt", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxseliule] = {"xseliule", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxseliult] = {"xseliult", {{Kw, Kl, Ke, Ke}, {Kw, Kl, Ke, Ke}}, 0, 0};
    a[Oxselfeq] = {"xselfeq", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselfge] = {"xselfge", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselfgt] = {"xselfgt", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselfle] = {"xselfle", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselflt] = {"xselflt", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselfne] = {"xselfne", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselfo] = {"xselfo", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    a[Oxselfuo] = {"xselfuo", {{Ke, Ke, Ks, Kd}, {Ke, Ke, Ks, Kd}}, 0, 0};
    return a;
}();
