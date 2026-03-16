//     Richard CPU
//     Copyright (C) 2026  Richard Qin
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.

//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.

//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <https://www.gnu.org/licenses/>.

// File path: tb/unit/tb_branch.cc
// Description: Branch Unit test

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vbranch_unit.h"

// Enums must match rv64_pkg::branch_op_t
enum BranchOp {
    BRANCH_EQ  = 0b000,
    BRANCH_NE  = 0b001,
    BRANCH_LT  = 0b100,
    BRANCH_GE  = 0b101,
    BRANCH_LTU = 0b110,
    BRANCH_GEU = 0b111
};

void test_branch(Vbranch_unit* dut, uint8_t is_branch, uint8_t op, uint64_t a, uint64_t b, uint8_t expected, const char* name) {
    dut->is_branch = is_branch;
    dut->branch_op = op;
    dut->operand_a = a;
    dut->operand_b = b;
    dut->eval();

    if (dut->branch_taken == expected) {
        std::cout << "\033[32m[PASS] " << name << " | Expected: " << (int)expected << " Got: " << (int)dut->branch_taken << "\033[0m" << std::dec << std::endl;
    } else {
        std::cout << "\033[31m[FAIL] " << name << " | Expected: " << (int)expected << " Got: " << (int)dut->branch_taken << "\033[0m" << std::endl;
        assert(false);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vbranch_unit* dut = new Vbranch_unit;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        Branch Unit Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // --- Not a branch ---
    test_branch(dut, 0, BRANCH_EQ, 10, 10, 0, "Not_A_Branch");

    // --- BEQ / BNE ---
    test_branch(dut, 1, BRANCH_EQ, 10, 10, 1, "BEQ_Equal");
    test_branch(dut, 1, BRANCH_EQ, 10, 20, 0, "BEQ_Not_Equal");
    test_branch(dut, 1, BRANCH_NE, 10, 20, 1, "BNE_Not_Equal");
    test_branch(dut, 1, BRANCH_NE, 10, 10, 0, "BNE_Equal");

    uint64_t minus_one = (uint64_t)-1; // 0xFFFFFFFFFFFFFFFF
    uint64_t one = 1;

    // --- BLT / BGE (Signed) ---
    // -1 is less than 1 (Signed)
    test_branch(dut, 1, BRANCH_LT, minus_one, one, 1, "BLT_Signed_Negative_vs_Positive");
    test_branch(dut, 1, BRANCH_GE, minus_one, one, 0, "BGE_Signed_Negative_vs_Positive");
    
    // 1 is greater than -1 (Signed)
    test_branch(dut, 1, BRANCH_LT, one, minus_one, 0, "BLT_Signed_Positive_vs_Negative");
    test_branch(dut, 1, BRANCH_GE, one, minus_one, 1, "BGE_Signed_Positive_vs_Negative");

    // Equal signed
    test_branch(dut, 1, BRANCH_LT, minus_one, minus_one, 0, "BLT_Signed_Equal");
    test_branch(dut, 1, BRANCH_GE, minus_one, minus_one, 1, "BGE_Signed_Equal");

    // --- BLTU / BGEU (Unsigned) ---
    // 0xFFFFFFFFFFFFFFFF is NOT less than 1 (Unsigned)
    test_branch(dut, 1, BRANCH_LTU, minus_one, one, 0, "BLTU_Unsigned_Max_vs_1");
    // 0xFFFFFFFFFFFFFFFF is greater than or equal to 1 (Unsigned)
    test_branch(dut, 1, BRANCH_GEU, minus_one, one, 1, "BGEU_Unsigned_Max_vs_1");

    // Equal unsigned
    test_branch(dut, 1, BRANCH_LTU, minus_one, minus_one, 0, "BLTU_Unsigned_Equal");
    test_branch(dut, 1, BRANCH_GEU, minus_one, minus_one, 1, "BGEU_Unsigned_Equal");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
