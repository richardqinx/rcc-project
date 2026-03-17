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

// File path: tb/unit/tb_id_ex_reg.cc
// Description: ID/EX Pipeline Register unit test

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vid_ex_reg.h"

int pass_count = 0;

void tick(Vid_ex_reg* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
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

// Emulate rv64_pkg::alu_op_t
enum AluOp { ALU_ADD = 0, ALU_SUB = 8, ALU_SLL = 1, ALU_SLT = 2, ALU_SLTU = 3,
             ALU_XOR = 4, ALU_SRL = 5, ALU_SRA = 13, ALU_OR = 6, ALU_AND = 7 };

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vid_ex_reg* dut = new Vid_ex_reg;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        ID/EX Register Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // 1. Init / Reset
    dut->stall_en = 0;
    dut->flush_en = 0;
    
    // Set some random ID stage inputs with dangerous states
    dut->id_pc = 0x80001000;
    dut->id_rs1_rdata = 0x11111111;
    dut->id_rs2_rdata = 0x22222222;
    dut->id_rs1_addr = 31;
    dut->id_rs2_addr = 15;
    dut->id_imm = 0x1A2B3C;
    dut->id_alu_op = ALU_XOR;
    dut->id_reg_write_en = 1;
    dut->id_is_store = 1;
    dut->id_is_load = 1;
    dut->id_is_branch = 1;
    dut->id_is_jump = 1;
    dut->id_is_csr = 1;
    dut->id_illegal_instr = 1;

    dut->rst = 1;
    tick(dut);
    dut->rst = 0;

    // Verify reset clears out dangerous signals
    check(dut->ex_pc == 0, "Reset_Clears_PC");
    check(dut->ex_reg_write_en == 0, "Reset_Clears_RegWrite");
    check(dut->ex_is_store == 0, "Reset_Clears_IsStore");
    check(dut->ex_is_load == 0, "Reset_Clears_IsLoad");
    check(dut->ex_is_branch == 0, "Reset_Clears_IsBranch");
    check(dut->ex_is_jump == 0, "Reset_Clears_IsJump");
    check(dut->ex_is_csr == 0, "Reset_Clears_IsCSR");
    check(dut->ex_illegal_instr == 0, "Reset_Clears_IllegalInstr");

    // 2. Normal Propagation
    dut->id_pc = 0x80002000;
    dut->id_rs1_rdata = 0xAAAAAAAA;
    dut->id_rs2_rdata = 0xBBBBBBBB;
    dut->id_rs1_addr = 10;
    dut->id_rs2_addr = 11;
    dut->id_imm = 42;
    dut->id_alu_op = ALU_ADD;
    dut->id_reg_write_en = 1;
    dut->id_rd_addr = 5;
    dut->id_is_load = 1;
    dut->id_is_branch = 0;

    tick(dut);

    check(dut->ex_pc == 0x80002000, "Propagate_PC");
    check(dut->ex_rs1_rdata == 0xAAAAAAAA, "Propagate_RS1");
    check(dut->ex_rs2_rdata == 0xBBBBBBBB, "Propagate_RS2");
    check(dut->ex_rs1_addr == 10, "Propagate_RS1_Addr");
    check(dut->ex_rs2_addr == 11, "Propagate_RS2_Addr");
    check(dut->ex_imm == 42, "Propagate_IMM");
    check(dut->ex_alu_op == ALU_ADD, "Propagate_ALU_OP");
    check(dut->ex_reg_write_en == 1, "Propagate_RegWriteEn");
    check(dut->ex_rd_addr == 5, "Propagate_RdAddr");
    check(dut->ex_is_load == 1, "Propagate_IsLoad");

    // 3. Stall (Freeze)
    // Change input, but assert stall
    dut->stall_en = 1;
    dut->id_pc = 0x80003000;
    dut->id_rd_addr = 10;
    dut->id_reg_write_en = 0;
    dut->id_is_load = 0;
    
    tick(dut);

    check(dut->ex_pc == 0x80002000, "Stall_Maintains_PC");
    check(dut->ex_rd_addr == 5, "Stall_Maintains_RdAddr");
    check(dut->ex_reg_write_en == 1, "Stall_Maintains_RegWriteEn");
    check(dut->ex_is_load == 1, "Stall_Maintains_IsLoad");

    // Release Stall
    dut->stall_en = 0;
    tick(dut);
    check(dut->ex_pc == 0x80003000, "ReleaseStall_Captures_PC");
    check(dut->ex_rd_addr == 10, "ReleaseStall_Captures_RdAddr");
    check(dut->ex_reg_write_en == 0, "ReleaseStall_Captures_RegWriteEn");

    // 4. Flush (Insert Bubble/NOP)
    // Change input, but assert flush
    dut->flush_en = 1;
    dut->id_pc = 0x80004000;
    dut->id_reg_write_en = 1;
    dut->id_is_store = 1;
    dut->id_is_branch = 1;
    
    tick(dut);

    check(dut->ex_pc == 0x80003000, "Flush_Maintains_PC");
    check(dut->ex_reg_write_en == 0, "Flush_Clears_RegWrite"); 
    check(dut->ex_is_store == 0, "Flush_Clears_StoreEn");     
    check(dut->ex_is_branch == 0, "Flush_Clears_BranchEn");

    // 5. Flush Overrides Stall
    dut->flush_en = 0;
    dut->id_pc = 0x80005000;
    dut->id_reg_write_en = 1;
    tick(dut); 
    check(dut->ex_pc == 0x80005000, "Pre_FlushStall_Setup");

    dut->flush_en = 1;
    dut->stall_en = 1;
    dut->id_pc = 0x80006000;
    tick(dut);

    check(dut->ex_pc == 0x80005000, "Flush_Overrides_Stall_PC");
    check(dut->ex_reg_write_en == 0, "Flush_Overrides_Stall_RegWrite");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
