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

/// @file symtab.h
/// @brief Scope chain symbol table.

#include "rcc/sysy_type.h"

#include <cassert>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rcc {

/// Symbol information
struct Symbol {
    std::string name;
    BType type = BType::Int;
    bool is_const = false;
    bool is_array = false;
    bool is_param = false;
    bool is_global = false;
    std::vector<int> dims;  // Array dimensions

    // Compile-time constant value (for constant folding and const array initialization)
    std::variant<std::monostate, int32_t, float, std::vector<int32_t>, std::vector<float>>
        const_val;

    // IR phase: If variable needs an address (array, global), store its alloca/global address here
    int alloca_temp_id = -1;  // Stack-allocated temp ID, -1 means unallocated

    // Unique ID for Braun SSA: unique within a function, across scopes.
    // Allocated by SymbolTable::allocate_uid() during Symbol construction.
    int uid = -1;
};

/// Function signature
struct FuncSig {
    std::string name;
    BType ret_type = BType::Void;
    std::vector<std::pair<BType, bool>> params;  // (type, is_array)
};

/// Chained scope symbol table
class SymbolTable {
  public:
    void enter_scope() { scopes_.emplace_back(); }

    void leave_scope() {
        assert(!scopes_.empty());
        scopes_.pop_back();
    }

    /// Insert symbol in current scope. Returns false on duplicate definition.
    bool insert(const std::string& name, Symbol sym) {
        auto& cur = scopes_.back().symbols;
        if (cur.count(name)) {
            return false;
        }
        cur[name] = std::move(sym);
        return true;
    }

    /// Lookup variable symbol from inner to outer scopes.
    Symbol* lookup(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->symbols.find(name);
            if (found != it->symbols.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    /// Register function signature.
    void register_func(FuncSig sig) { func_table_[sig.name] = std::move(sig); }

    /// Lookup function signature.
    FuncSig* lookup_func(const std::string& name) {
        auto it = func_table_.find(name);
        return it != func_table_.end() ? &it->second : nullptr;
    }

    /// Register SysY runtime library functions.
    void register_builtins();

    /// Allocate a new Symbol uid.
    /// Counter increments monotonically across enter/leave_scope without resetting.
    int allocate_uid() { return next_uid_++; }

  private:
    struct Scope {
        std::unordered_map<std::string, Symbol> symbols;
    };
    std::vector<Scope> scopes_;
    std::unordered_map<std::string, FuncSig> func_table_;
    int next_uid_ = 0;
};

}  // namespace rcc
