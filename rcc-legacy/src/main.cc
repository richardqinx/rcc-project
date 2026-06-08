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

/// @file main.cc
/// @brief RCC compiler entry point.

#include "rcc/all.h"
#include "rcc/ast.h"
#include "rcc/irgen.h"
#include "rcc/rv64.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

// External Flex/Bison declarations
extern FILE* yyin;
extern int yyparse();
extern rcc::CompUnitNode* g_ast_root;

namespace {

struct Args {
    std::string input;
    std::string output;
    int opt_level = 0;
    bool dump_ast = false;
    bool dump_ir = false;
    bool emit_il = false;  // Output QBE-compatible SSA text to -o (or stdout)
    bool dbg = false;      // Set if any -d flag is enabled; triggers skipping emit
};

Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) {
            continue;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            args.output = argv[++i];
        } else if (strcmp(argv[i], "-O1") == 0) {
            args.opt_level = 1;
        } else if (strcmp(argv[i], "-O2") == 0) {
            args.opt_level = 2;
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            args.dump_ast = true;
        } else if (strcmp(argv[i], "--dump-ir") == 0) {
            args.dump_ir = true;
        } else if (strcmp(argv[i], "--emit-il") == 0) {
            args.emit_il = true;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            for (char* p = argv[++i]; *p; ++p) {
                unsigned char c = static_cast<unsigned char>(*p);
                if (std::isalpha(c)) {
                    rcc::debug[std::toupper(c)] = 1;
                    args.dbg = true;
                }
            }
        } else if (argv[i][0] != '-') {
            args.input = argv[i];
        }
    }

    if (args.input.empty()) {
        fprintf(stderr, "rcc: error: no input file\n");
        fprintf(stderr, "usage: compiler input.sy -S -o output.s [-O1] [--emit-il] [-d <flags>]\n");
        exit(1);
    }
    if (args.output.empty()) {
        args.output = args.emit_il ? "-" : "a.s";
    }
    return args;
}

// globals → Dat* → emitdat

constexpr size_t kElemBytes = 4;

void emit_one_global(const rcc::GlobalData& g, FILE* f) {
    rcc::Lnk lnk{};
    lnk.align = 4;
    // Do not export: SysY globals are not visible externally.

    std::string name = g.name;
    rcc::Dat d{};
    d.lnk = &lnk;
    d.name = name.data();

    d.type = rcc::Dat::DStart;
    rcc::emitdat(&d, f);

    if (g.is_zeroinit) {
        d.type = rcc::Dat::DZ;
        d.u.num = static_cast<int64_t>(g.total_elems * kElemBytes);
        rcc::emitdat(&d, f);
    } else {
        for (const auto& v : g.init_vals) {
            int32_t bits32 = 0;
            if (std::holds_alternative<int32_t>(v)) {
                int32_t iv = std::get<int32_t>(v);
                if (g.elem_cls == rcc::Ks) {
                    float fv = static_cast<float>(iv);
                    std::memcpy(&bits32, &fv, sizeof(bits32));
                } else {
                    bits32 = iv;
                }
            } else {
                float fv = std::get<float>(v);
                if (g.elem_cls == rcc::Kw) {
                    bits32 = static_cast<int32_t>(fv);
                } else {
                    std::memcpy(&bits32, &fv, sizeof(bits32));
                }
            }
            d.type = rcc::Dat::DW;
            d.isref = 0;
            d.isstr = 0;
            d.u.num = static_cast<int64_t>(static_cast<uint32_t>(bits32));
            rcc::emitdat(&d, f);
        }
    }

    d.type = rcc::Dat::DEnd;
    rcc::emitdat(&d, f);
}

void emit_globals_asm(const std::vector<rcc::GlobalData>& globals, FILE* f) {
    for (const auto& g : globals)
        emit_one_global(g, f);
}

// Mid-end pass chain
// ssacheck after gcm; ifconvert only if '-d K' is set.
void run_midend(rcc::Ctx* ctx, rcc::Fn* fn) {
    ctx->fn = fn;
    ctx->spill_fp_live_peak = 0;
    rcc::T.abi0(fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::promote(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssa(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::filldom(fn);
    rcc::loopinterchange(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::ssacheck(fn);
    rcc::lsr(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::loop_idiom(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::ssacheck(fn);
    rcc::dce(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::reduction_unroll(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::ssacheck(fn);
    rcc::dce(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::dead_loop(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::unroll(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::unswitch(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::tre(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::fillalias(fn);
    rcc::loadopt(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::sccp(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::ssacheck(fn);
    rcc::fillalias(fn);
    rcc::licm(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::fillalias(fn);
    rcc::scalar_repl(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::coalesce(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::ssacheck(fn);
    rcc::sccp(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::ssacheck(fn);
    rcc::reassoc(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    rcc::gvn(ctx, fn);
    rcc::fillcfg(ctx, fn);
    rcc::simplcfg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::filldom(fn);
    rcc::gcm(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::divopt(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::mulopt(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::storemrg(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::dce(ctx, fn);
    rcc::filluse(ctx, fn);
    rcc::ssacheck(fn);
    if (rcc::T.cansel) {
        rcc::ifconvert(ctx, fn);
        rcc::fillcfg(ctx, fn);
        rcc::filluse(ctx, fn);
        rcc::filldom(fn);
        rcc::ssacheck(fn);
    }
}

// Backend pass chain
void run_backend(rcc::Ctx* ctx, rcc::Fn* fn) {
#define STAGE(call)                                                      \
    do {                                                                 \
        call;                                                            \
        if (rcc::debug['B']) {                                           \
            fprintf(stderr, "\n>>>> after %s (%s):\n", #call, fn->name); \
            rcc::printfn(fn, stderr);                                    \
        }                                                                \
    } while (0)
    STAGE(rcc::T.abi1(ctx, fn));
    STAGE(rcc::simpl(ctx, fn));
    STAGE(rcc::fillcfg(ctx, fn));
    STAGE(rcc::filluse(ctx, fn));
    STAGE(rcc::T.isel(ctx, fn));
    STAGE(rcc::fillcfg(ctx, fn));
    STAGE(rcc::filllive(ctx, fn));
    STAGE(rcc::fillloop(fn));
    STAGE(rcc::fillcost(ctx, fn));
    STAGE(rcc::spill(ctx, fn));
    STAGE(rcc::rega(ctx, fn));
    STAGE(rcc::peephole(ctx, fn));
    STAGE(rcc::fillcfg(ctx, fn));
    STAGE(rcc::simpljmp(ctx, fn));
    STAGE(rcc::fillcfg(ctx, fn));
#undef STAGE

    // Convert rpo[] to linked list for rv64_emitfn
    assert(fn->rpo[0] == fn->start);
    for (uint n = 0;; ++n) {
        if (n == fn->nblk - 1) {
            fn->rpo[n]->link = nullptr;
            break;
        }
        fn->rpo[n]->link = fn->rpo[n + 1];
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    auto args = parse_args(argc, argv);

    // Front-end: Lexical + Syntax Analysis → AST
    yyin = fopen(args.input.c_str(), "r");
    if (!yyin) {
        fprintf(stderr, "rcc: error: cannot open '%s'\n", args.input.c_str());
        return 1;
    }

    int parse_ret = yyparse();
    fclose(yyin);

    if (parse_ret != 0 || !g_ast_root) {
        fprintf(stderr, "rcc: error: parsing failed\n");
        return 1;
    }

    if (args.dump_ast) {
        rcc::ast_dump(g_ast_root);
    }

    // Target descriptor: rv64 lp64d. Assigned before worker creation, read-only afterwards.
    rcc::T = rcc::T_rv64;

    rcc::Ctx ctx_main;
    ctx_main.init();
    ctx_main.subpool = new rcc::SubPool();
    ctx_main.dbg_buf = stderr;  // Dump destination for if (debug['X'])

    // IR Generation: AST → SSA IR (Braun algorithm)
    rcc::IRGen irgen(&ctx_main);
    auto ir = irgen.generate(g_ast_root);

    if (args.dump_ir) {
        fprintf(stderr, "[%zu functions, %zu globals]\n", ir.functions.size(), ir.globals.size());
    }

    rcc::inline_module(&ctx_main, ir.functions);

    if (args.emit_il) {
        for (rcc::Fn* fn : ir.functions)
            run_midend(&ctx_main, fn);

        FILE* out = (args.output == "-") ? stdout : fopen(args.output.c_str(), "w");
        if (!out) {
            fprintf(stderr, "rcc: error: cannot open '%s' for write\n", args.output.c_str());
            return 1;
        }
        rcc::print_globals(ir.globals, out);
        for (rcc::Fn* fn : ir.functions)
            rcc::printfn(fn, out);
        if (out != stdout)
            fclose(out);
    } else {
        FILE* outf = nullptr;
        if (!args.dbg) {
            outf = (args.output == "-") ? stdout : fopen(args.output.c_str(), "w");
            if (!outf) {
                fprintf(stderr, "rcc: error: cannot open '%s' for write\n", args.output.c_str());
                return 1;
            }
            emit_globals_asm(ir.globals, outf);
        }

        for (rcc::Fn* fn : ir.functions) {
            if (args.dbg)
                fprintf(stderr, "**** Function %s ****", fn->name);
            run_midend(&ctx_main, fn);
            run_backend(&ctx_main, fn);
            if (!args.dbg) {
                rcc::set_die_ctx(&ctx_main);
                rcc::T.emitfn(fn, outf);
                fprintf(outf, "/* end function %s */\n\n", fn->name);
            } else {
                fprintf(stderr, "\n");
            }
        }

        if (!args.dbg) {
            rcc::T.emitfin(&ctx_main, outf);
            if (outf != stdout)
                fclose(outf);
        }
    }

    // Clean AST
    delete g_ast_root;
    g_ast_root = nullptr;

    // Release SubPool; ~Ctx automatically frees insb
    ctx_main.release();

    return 0;
}
