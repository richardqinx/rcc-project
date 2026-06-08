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

/// @file irgen.h
/// @brief AST -> SSA IR (Braun Algorithm)
///
/// Produces Fn/Blk/Ins/Phi structures defined in all.h.
/// Memory is managed via the injected Ctx (SubPool-backed).

#include "rcc/all.h"
#include "rcc/ast.h"
#include "rcc/symtab.h"

#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rcc {

/// Global variable data (for data segment emission)
struct GlobalData {
    std::string name;
    int elem_cls = Kw;  // Kw / Ks
    size_t total_elems = 0;
    bool is_const = false;
    bool is_zeroinit = false;
    std::vector<std::variant<int32_t, float>> init_vals;
};

/// IRGen output
struct IROutput {
    std::vector<Fn*> functions;
    std::vector<GlobalData> globals;
};

/// Export global variables list as data segment syntax.
void print_globals(const std::vector<GlobalData>& globals, FILE* f);

class IRGen {
  public:
    explicit IRGen(Ctx* ctx) : ctx_(ctx) {}

    /// Translate AST to IR
    IROutput generate(CompUnitNode* root);

  private:
    Ctx* ctx_;
    IROutput output_;
    Fn* cur_fn_ = nullptr;
    Blk* cur_blk_ = nullptr;
    SymbolTable sym_;

    // Current function's return K class: Kw / Ks (Kx=-1 for void).
    // Do not reuse Fn::retty.
    int cur_fn_ret_cls_ = Kx;

    // Fn/Blk constructors

    /// Create new function
    Fn* new_fn(const char* name);

    /// Create new basic block
    Blk* new_blk(const char* name);

    /// Get basic block (by id)
    Blk* get_blk(uint id);

    /// All blocks in current function (indexed by id)
    std::vector<Blk*> blk_vec_;

    // constant/Ref constructors

    /// Create int constant Ref
    Ref make_int_ref(int32_t val);
    /// Create float constant Ref
    Ref make_float_ref(float val);
    /// Create global symbol RCon (CAddr).
    /// For call target use only; data access should uniformly go through make_global_addr_ref.
    Ref make_global_ref(const char* name);
    /// Materialize global symbol into a Tmp (Oaddr Kl).
    Ref make_global_addr_ref(const char* name);
    /// Create new temporary
    Ref new_tmp_ref(int cls);

    // Braun SSA algorithm
    //
    // Index using Symbol::uid rather than variable name to prevent scope shadowing bugs.

    std::unordered_map<int, std::unordered_map<uint, Ref>> current_def_;

    struct IncompletePhi {
        int var_uid;
        Phi* phi;
        uint block_id;
        int cls;
    };
    std::vector<IncompletePhi> incomplete_phis_;

    void write_variable(int uid, Blk* blk, Ref val);
    Ref read_variable(int uid, Blk* blk, int cls);
    Ref read_variable_recursive(int uid, Blk* blk, int cls);
    void seal_block(Blk* blk);
    void add_phi_operands(int uid, Blk* blk, Phi* phi, int cls);
    Ref try_remove_trivial_phi(Phi* phi);

    // Block sealing

    std::unordered_map<uint, bool> sealed_;
    bool is_sealed(Blk* b) const;

    // Loop context

    struct LoopCtx {
        Blk* cond_block;
        Blk* exit_block;
    };
    std::vector<LoopCtx> loop_stack_;

    // AST traversal

    void visit_comp_unit(CompUnitNode* node);
    void visit_func_def(FuncDefNode* node);
    void visit_global_decl(VarDeclNode* node);
    void visit_local_decl(VarDeclNode* node);
    void visit_stmt(ASTNode* node);
    void visit_block(BlockNode* node);
    void visit_if(IfStmtNode* node);
    void visit_while(WhileStmtNode* node);
    void visit_assign(AssignStmtNode* node);
    void visit_return(ReturnStmtNode* node);

    Ref visit_expr(ASTNode* node);

    void visit_cond(ASTNode* cond, Blk* true_bb, Blk* false_bb);
    void visit_cond_and(BinaryExprNode* node, Blk* true_bb, Blk* false_bb);
    void visit_cond_or(BinaryExprNode* node, Blk* true_bb, Blk* false_bb);

    // Instruction emission

    Ref emit_ins(int op, int cls, Ref a0, Ref a1);
    void emit_store(Ref val, Ref addr, int cls);
    Ref emit_load(Ref addr, int cls);
    /// IR-level zero-fill loop. Clears [addr, addr+size_bytes) entirely.
    void emit_bulk_zero(Ref addr, int size_bytes);
    void emit_jmp(Blk* target);
    void emit_jnz(Ref cond, Blk* true_bb, Blk* false_bb);
    void emit_ret(Ref val, int cls);
    void emit_ret_void();
    void push_ins(Ins i);
    bool cur_blk_terminated() const;
    void add_edge(Blk* from, Blk* to);

    // Helpers

    Ref compute_array_addr(LValNode* node);
    Ref get_var_addr_ref(Symbol* sym);

    int32_t eval_const_int(ASTNode* node);
    float eval_const_float(ASTNode* node);

    void flatten_init(const std::vector<int>& dims, ASTNode* init, std::vector<ASTNode*>& out);

    /// BType -> K class (Kw / Ks)
    static int btype_to_cls(BType t);

    /// Infer actual K class of a Ref.
    static int ref_cls_of(Ref r, Fn* fn);

    /// Type coercion: Emit Oswtof/Ostosi when Ref's current cls differs from target.
    Ref coerce(Ref v, int target_cls);
};

}  // namespace rcc
