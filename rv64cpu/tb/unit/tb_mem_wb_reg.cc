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

// File path: tb/unit/tb_mem_wb_reg.cc
// Description: MEM/WB Stage Register unit test

#include <iostream>
#include <cassert>
#include <verilated.h>
#include "Vmem_wb_reg.h"

int pass_count = 0;

void tick(Vmem_wb_reg* dut) {
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
    Vmem_wb_reg* dut = new Vmem_wb_reg;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        MEM/WB Register Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // 1. Init / Reset
    dut->stall_en = 0;
    dut->flush_en = 0;

    // Set some dangerous inputs
    dut->mem_pc = 0x80001000;
    dut->mem_alu_result = 0x11111111;
    dut->mem_mem_rdata = 0x22222222;
    dut->mem_reg_write_en = 1;
    dut->mem_rd_addr = 31;
    dut->mem_wb_sel = 1;
    dut->mem_is_csr = 1;
    dut->mem_csr_op = 2;
    dut->mem_rs1_rdata = 0x999;
    dut->mem_rs1_addr = 5;
    dut->mem_imm = 0x333;
    dut->mem_is_ecall = 1;
    dut->mem_illegal_instr = 1;
    dut->mem_is_sret = 1;

    dut->rst = 1;
    tick(dut);
    dut->rst = 0;

    check(dut->wb_pc == 0, "Reset_Clears_PC");
    check(dut->wb_alu_result == 0, "Reset_Clears_ALU");
    check(dut->wb_reg_write_en == 0, "Reset_Clears_RegWrite");
    check(dut->wb_is_csr == 0, "Reset_Clears_IsCSR");
    check(dut->wb_illegal_instr == 0, "Reset_Clears_IllegalInstr");

    // 2. Normal Propagation
    dut->mem_pc = 0x80002000;
    dut->mem_alu_result = 0xAAAAAAAA;
    dut->mem_mem_rdata = 0xBBBBBBBB;
    dut->mem_rd_addr = 15;
    dut->mem_reg_write_en = 1;
    dut->mem_wb_sel = 1;
    dut->mem_is_csr = 1;
    dut->mem_csr_op = 3;
    dut->mem_rs1_rdata = 0x777;
    dut->mem_rs1_addr = 6;
    dut->mem_imm = 0x888;
    dut->mem_is_ecall = 0;

    tick(dut);

    check(dut->wb_pc == 0x80002000, "Propagate_PC");
    check(dut->wb_alu_result == 0xAAAAAAAA, "Propagate_ALU_Result");
    check(dut->wb_mem_rdata == 0xBBBBBBBB, "Propagate_MemRData");
    check(dut->wb_rd_addr == 15, "Propagate_RdAddr");
    check(dut->wb_reg_write_en == 1, "Propagate_RegWriteEn");
    check(dut->wb_wb_sel == 1, "Propagate_WbSel");
    check(dut->wb_is_csr == 1, "Propagate_IsCSR");
    check(dut->wb_csr_op == 3, "Propagate_CSROp");
    check(dut->wb_rs1_rdata == 0x777, "Propagate_RS1_RData");
    check(dut->wb_rs1_addr == 6, "Propagate_RS1_Addr");
    check(dut->wb_imm == 0x888, "Propagate_Imm");

    // 3. Stall (Freeze)
    dut->stall_en = 1;
    dut->mem_pc = 0x80003000;
    dut->mem_rd_addr = 20;
    dut->mem_reg_write_en = 0;

    tick(dut);

    check(dut->wb_pc == 0x80002000, "Stall_Maintains_PC");
    check(dut->wb_rd_addr == 15, "Stall_Maintains_RdAddr");
    check(dut->wb_reg_write_en == 1, "Stall_Maintains_RegWriteEn");

    // Release Stall
    dut->stall_en = 0;
    tick(dut);
    check(dut->wb_pc == 0x80003000, "ReleaseStall_Captures_PC");
    check(dut->wb_rd_addr == 20, "ReleaseStall_Captures_RdAddr");
    check(dut->wb_reg_write_en == 0, "ReleaseStall_Captures_RegWriteEn");

    // 4. Flush (Insert Bubble/NOP)
    dut->flush_en = 1;
    dut->mem_pc = 0x80004000;
    dut->mem_alu_result = 0xCCCCCCCC;
    dut->mem_reg_write_en = 1;
    dut->mem_is_csr = 1;

    tick(dut);

    check(dut->wb_pc == 0x80003000, "Flush_Maintains_PC");
    check(dut->wb_alu_result == 0xAAAAAAAA, "Flush_Maintains_ALU");
    check(dut->wb_reg_write_en == 0, "Flush_Clears_RegWrite");
    check(dut->wb_is_csr == 0, "Flush_Clears_IsCSR");

    // 5. Flush Overrides Stall
    dut->flush_en = 0;
    dut->mem_pc = 0x80005000;
    dut->mem_reg_write_en = 1;
    tick(dut);
    check(dut->wb_pc == 0x80005000, "Pre_FlushStall_Setup");

    dut->flush_en = 1;
    dut->stall_en = 1;
    dut->mem_pc = 0x80006000;
    tick(dut);

    check(dut->wb_pc == 0x80005000, "Flush_Overrides_Stall_PC");
    check(dut->wb_reg_write_en == 0, "Flush_Overrides_Stall_RegWrite");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
