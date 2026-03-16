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

// File path: tb/unit/tb_ex_stage.cc
// Description: Execution Stage unit test

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vex_stage.h"

int pass_count = 0;

void eval(Vex_stage* dut) {
    dut->eval();
}

void check(bool cond, const char* name) {
    if (cond) {
        std::cout << "\033[32m[PASS] " << name << "\033[0m" << std::endl;
        pass_count++;
    } else {
        std::cout << "\033[31m[FAIL] " << name << "\033[0m" << std::endl;
        assert(false);
    }
}

// Emulate rv64_pkg enums
enum AluOp { ALU_ADD = 0, ALU_SUB = 8, ALU_SLL = 1, ALU_SLT = 2, ALU_SLTU = 3,
             ALU_XOR = 4, ALU_SRL = 5, ALU_SRA = 13, ALU_OR = 6, ALU_AND = 7 };
enum BranchOp { BRANCH_EQ = 0, BRANCH_NE = 1, BRANCH_LT = 4, BRANCH_GE = 5, BRANCH_LTU = 6, BRANCH_GEU = 7 };

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vex_stage* dut = new Vex_stage;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        Execute Stage Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // Default inputs
    dut->ex_pc = 0;
    dut->ex_rs1_rdata = 0;
    dut->ex_rs2_rdata = 0;
    dut->ex_imm = 0;
    dut->ex_alu_op = ALU_ADD;
    dut->ex_alu_src1_sel = 0;
    dut->ex_alu_src2_sel = 0;
    dut->ex_is_branch = 0;
    dut->ex_branch_op = 0;
    dut->ex_is_jump = 0;
    dut->ex_is_word_op = 0;

    // 1. Basic R-Type (ADD)
    // rs1 + rs2
    dut->ex_rs1_rdata = 50;
    dut->ex_rs2_rdata = 25;
    dut->ex_alu_src1_sel = 0; // rs1
    dut->ex_alu_src2_sel = 0; // rs2
    dut->ex_alu_op = ALU_ADD;
    
    eval(dut);
    check(dut->ex_alu_result == 75, "R_Type_ADD");
    check(dut->ex_branch_taken == 0, "R_Type_Not_Branch");

    // 2. Basic I-Type (ADDI)
    // rs1 + imm
    dut->ex_rs1_rdata = 100;
    dut->ex_imm = 42;
    dut->ex_alu_src1_sel = 0; // rs1
    dut->ex_alu_src2_sel = 1; // imm
    dut->ex_alu_op = ALU_ADD;
    
    eval(dut);
    check(dut->ex_alu_result == 142, "I_Type_ADDI");

    // 3. Branch Taken (BEQ: rs1 == rs2)
    // target = PC + imm_b
    dut->ex_is_branch = 1;
    dut->ex_branch_op = BRANCH_EQ;
    dut->ex_rs1_rdata = 88;
    dut->ex_rs2_rdata = 88; // Equal!
    
    dut->ex_pc = 0x80001000;
    dut->ex_imm = 0x10;
    dut->ex_alu_src1_sel = 1; // PC
    dut->ex_alu_src2_sel = 1; // imm
    dut->ex_alu_op = ALU_ADD;
    
    eval(dut);
    check(dut->ex_branch_taken == 1, "Branch_BEQ_Taken");
    check(dut->ex_branch_target == 0x80001010, "Branch_Target_Calculation");

    // 4. Branch Not Taken (BEQ: rs1 != rs2)
    dut->ex_rs1_rdata = 88;
    dut->ex_rs2_rdata = 99; // Not Equal
    eval(dut);
    check(dut->ex_branch_taken == 0, "Branch_BEQ_Not_Taken");

    // 5. Jump (JAL)
    // target = PC + imm_j (always taken)
    dut->ex_is_branch = 0;
    dut->ex_is_jump = 1;
    dut->ex_pc = 0x80002000;
    dut->ex_imm = 0x24; // Forward jump 36 bytes
    dut->ex_alu_src1_sel = 1; // PC
    dut->ex_alu_src2_sel = 1; // imm
    dut->ex_alu_op = ALU_ADD;
    
    eval(dut);
    check(dut->ex_branch_taken == 1, "Jump_JAL_Taken");
    check(dut->ex_branch_target == 0x80002024, "Jump_JAL_Target");

    // 6. Jump Register (JALR)
    // target = (rs1 + imm_i) & ~1
    dut->ex_is_jump = 1;
    dut->ex_rs1_rdata = 0x80003001; // Intentionally unaligned
    dut->ex_imm = 4;
    dut->ex_alu_src1_sel = 0; // rs1
    dut->ex_alu_src2_sel = 1; // imm
    dut->ex_alu_op = ALU_ADD;
    
    eval(dut);
    // 0x80003001 + 4 = 0x80003005. Target -> 0x80003004.
    check(dut->ex_branch_taken == 1, "Jump_JALR_Taken");
    check(dut->ex_branch_target == 0x80003004, "Jump_JALR_Clears_LSB");
    check(dut->ex_alu_result == 0x80003005, "JALR_Raw_ALU_Result");

    // 7. Word Operation (ADDW)
    // Force 32-bit addition with sign extension
    dut->ex_is_jump = 0;
    dut->ex_is_word_op = 1; 
    dut->ex_rs1_rdata = 0x000000007FFFFFFF;
    dut->ex_rs2_rdata = 0x0000000000000001;
    dut->ex_alu_src1_sel = 0; // rs1
    dut->ex_alu_src2_sel = 0; // rs2
    dut->ex_alu_op = ALU_ADD;
    
    // 0x7FFFFFFF + 1 = 0x80000000. Under 64-bit ADD this is positive 0x80000000.
    // However, ADDW requires sign-extending the lower 32 bits. Therefore, it becomes 0xFFFFFFFF80000000.
    eval(dut);
    check(dut->ex_alu_result == 0xFFFFFFFF80000000ULL, "Word_Op_ADDW_SignExt");

    // 8. Load/Store Address Calculation
    // target = rs1 + imm
    dut->ex_is_word_op = 0;
    dut->ex_rs1_rdata = 0x8000A000;
    dut->ex_imm = 0xFFFFFFFFFFFFFFF0ULL; // -16
    dut->ex_alu_src1_sel = 0; // rs1
    dut->ex_alu_src2_sel = 1; // imm
    dut->ex_alu_op = ALU_ADD;
    
    eval(dut);
    check(dut->ex_alu_result == 0x80009FF0, "LoadStore_Address_Mem");

    // 9. Shift Left Logical (SLL)
    dut->ex_is_word_op = 0;
    dut->ex_rs1_rdata = 0x000000000000FFFFULL;
    dut->ex_rs2_rdata = 16; // Shift by 16
    dut->ex_alu_src1_sel = 0;
    dut->ex_alu_src2_sel = 0;
    dut->ex_alu_op = ALU_SLL;
    eval(dut);
    check(dut->ex_alu_result == 0x00000000FFFF0000ULL, "ALU_SLL_64bit");

    // 10. Shift Right Arithmetic (SRA)
    dut->ex_rs1_rdata = 0xFFFFFFFF00000000ULL;
    dut->ex_rs2_rdata = 16;
    dut->ex_alu_op = ALU_SRA;
    eval(dut);
    check(dut->ex_alu_result == 0xFFFFFFFFFFFF0000ULL, "ALU_SRA_64bit");

    // 11. Word Shift Right Arithmetic (SRAW)
    dut->ex_is_word_op = 1;
    dut->ex_rs1_rdata = 0xFFFFFFFF80000000ULL; // -2147483648
    dut->ex_rs2_rdata = 4;
    dut->ex_alu_op = ALU_SRA;
    eval(dut);
    // 0x80000000 arithmetically shifted right by 4 is 0xF8000000. Sign extended to 64-bit.
    check(dut->ex_alu_result == 0xFFFFFFFFF8000000ULL, "ALU_SRAW_32bit");

    // 12. Word Shift Left Logical (SLLW) with large shamt
    dut->ex_rs1_rdata = 0x000000000000FFFFULL;
    dut->ex_rs2_rdata = 0x24; // 36 = 100100 in binary. Masked to 5 bits = 4.
    dut->ex_alu_op = ALU_SLL;
    eval(dut);
    check(dut->ex_alu_result == 0x00000000000FFFF0ULL, "ALU_SLLW_Shamt_Masking");

    // 13. Word Shift Right Logical (SRLW) with negative 32-bit
    // Zero extends the 32-bit source, avoiding sign extension during logical shift
    dut->ex_rs1_rdata = 0xFFFFFFFF80000000ULL;
    dut->ex_rs2_rdata = 4;
    dut->ex_alu_op = ALU_SRL;
    eval(dut);
    // 0x80000000 logically shifted right by 4 is 0x08000000. Sign extended to 64-bit -> positive.
    check(dut->ex_alu_result == 0x0000000008000000ULL, "ALU_SRLW_ZeroExtend");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
