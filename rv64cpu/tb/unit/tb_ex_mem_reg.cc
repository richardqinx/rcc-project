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

// File path: tb/unit/tb_ex_mem_reg.cc
// Description: Execution/Memory Stage Pipeline Register unit test

#include <iostream>
#include <cassert>
#include <verilated.h>
#include "Vex_mem_reg.h"

int pass_count = 0;

void tick(Vex_mem_reg* dut) {
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

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vex_mem_reg* dut = new Vex_mem_reg;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        EX/MEM Register Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // 1. Init / Reset
    dut->stall_en = 0;
    dut->flush_en = 0;
    
    // Set some random dangerous EX inputs
    dut->ex_pc = 0x80001000;
    dut->ex_rs1_rdata = 0x11111111;
    dut->ex_rs2_rdata = 0x22222222;
    dut->ex_rs1_addr = 31;
    dut->ex_alu_result = 0x88888888;
    dut->ex_branch_taken = 1;
    dut->ex_branch_target = 0x80001020;
    dut->ex_is_store = 1;
    dut->ex_is_load = 1;
    dut->ex_reg_write_en = 1;
    dut->ex_is_csr = 1;
    dut->ex_illegal_instr = 1;

    dut->rst = 1;
    tick(dut);
    dut->rst = 0;

    // Verify reset clears out dangerous signals
    check(dut->mem_pc == 0, "Reset_Clears_PC");
    check(dut->mem_branch_taken == 0, "Reset_Clears_BranchTaken");
    check(dut->mem_reg_write_en == 0, "Reset_Clears_RegWrite");
    check(dut->mem_is_store == 0, "Reset_Clears_IsStore");
    check(dut->mem_is_load == 0, "Reset_Clears_IsLoad");
    check(dut->mem_is_csr == 0, "Reset_Clears_IsCSR");
    check(dut->mem_illegal_instr == 0, "Reset_Clears_IllegalInstr");

    // 2. Normal Propagation
    dut->ex_pc = 0x80002000;
    dut->ex_rs1_rdata = 0xAAAAAAAA;
    dut->ex_rs2_rdata = 0xBBBBBBBB;
    dut->ex_rs1_addr = 15;
    dut->ex_alu_result = 0xCAFEBABE;
    dut->ex_branch_taken = 1;
    dut->ex_branch_target = 0x80002008;
    dut->ex_is_store = 0;
    dut->ex_is_load = 1;
    dut->ex_reg_write_en = 1;
    dut->ex_rd_addr = 15;
    dut->ex_wb_sel = 1;

    tick(dut);

    check(dut->mem_pc == 0x80002000, "Propagate_PC");
    check(dut->mem_rs1_rdata == 0xAAAAAAAA, "Propagate_RS1");
    check(dut->mem_rs2_rdata == 0xBBBBBBBB, "Propagate_RS2");
    check(dut->mem_rs1_addr == 15, "Propagate_RS1_Addr");
    check(dut->mem_alu_result == 0xCAFEBABE, "Propagate_ALU_Result");
    check(dut->mem_branch_taken == 1, "Propagate_BranchTaken");
    check(dut->mem_branch_target == 0x80002008, "Propagate_BranchTarget");
    check(dut->mem_is_load == 1, "Propagate_IsLoad");
    check(dut->mem_reg_write_en == 1, "Propagate_RegWriteEn");
    check(dut->mem_rd_addr == 15, "Propagate_RdAddr");
    check(dut->mem_wb_sel == 1, "Propagate_WbSel");

    // 3. Stall (Freeze)
    // Change input, but assert stall
    dut->stall_en = 1;
    dut->ex_pc = 0x80003000;
    dut->ex_rs1_rdata = 0xCCCCCCCC;
    dut->ex_rs1_addr = 20;
    dut->ex_branch_taken = 0;
    dut->ex_rd_addr = 20;
    dut->ex_reg_write_en = 0;
    dut->ex_is_load = 0;
    
    tick(dut);

    check(dut->mem_pc == 0x80002000, "Stall_Maintains_PC");
    check(dut->mem_rs1_rdata == 0xAAAAAAAA, "Stall_Maintains_RS1");
    check(dut->mem_rs1_addr == 15, "Stall_Maintains_RS1_Addr");
    check(dut->mem_branch_taken == 1, "Stall_Maintains_BranchTaken");
    check(dut->mem_rd_addr == 15, "Stall_Maintains_RdAddr");
    check(dut->mem_reg_write_en == 1, "Stall_Maintains_RegWriteEn");
    check(dut->mem_is_load == 1, "Stall_Maintains_IsLoad");

    // Release Stall
    dut->stall_en = 0;
    tick(dut);
    check(dut->mem_pc == 0x80003000, "ReleaseStall_Captures_PC");
    check(dut->mem_rs1_rdata == 0xCCCCCCCC, "ReleaseStall_Captures_RS1");
    check(dut->mem_rs1_addr == 20, "ReleaseStall_Captures_RS1_Addr");
    check(dut->mem_branch_taken == 0, "ReleaseStall_Captures_BranchTaken");
    check(dut->mem_rd_addr == 20, "ReleaseStall_Captures_RdAddr");
    check(dut->mem_reg_write_en == 0, "ReleaseStall_Captures_RegWriteEn");

    // 4. Flush (Insert Bubble/NOP)
    // Change input, but assert flush
    dut->flush_en = 1;
    dut->ex_pc = 0x80004000;
    dut->ex_branch_taken = 1;
    dut->ex_reg_write_en = 1;
    dut->ex_is_store = 1;
    
    tick(dut);

    check(dut->mem_pc == 0x0, "Flush_Clears_PC");
    check(dut->mem_branch_taken == 0, "Flush_Clears_BranchTaken");
    check(dut->mem_reg_write_en == 0, "Flush_Clears_RegWrite"); 
    check(dut->mem_is_store == 0, "Flush_Clears_StoreEn");     

    // 5. Flush Overrides Stall
    dut->flush_en = 0;
    dut->ex_pc = 0x80005000;
    dut->ex_reg_write_en = 1;
    tick(dut); 
    check(dut->mem_pc == 0x80005000, "Pre_FlushStall_Setup");

    dut->flush_en = 1;
    dut->stall_en = 1;
    dut->ex_pc = 0x80006000;
    dut->ex_branch_taken = 1;
    tick(dut);

    check(dut->mem_pc == 0x0, "Flush_Overrides_Stall_PC");
    check(dut->mem_branch_taken == 0, "Flush_Overrides_Stall_BranchTaken");
    check(dut->mem_reg_write_en == 0, "Flush_Overrides_Stall_RegWrite");
    check(dut->mem_is_store == 0, "Flush_Overrides_Stall_Store");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
