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

// File path: tb/unit/tb_id_stage.cc
// Description: ID Stage integration test
//   Verifies that id_stage correctly wires decoder + regfile:
//     1. Decoder control signal pass-through
//     2. RegFile write-back and read-out
//     3. PC pass-through
//     4. Immediate extraction

#include <iostream>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vid_stage.h"

static int pass_count = 0;

void tick(Vid_stage* dut) {
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

// ALU op encoding must match rv64_pkg
enum AluOp { ALU_ADD = 0, ALU_SUB = 8, ALU_SLL = 1, ALU_SLT = 2, ALU_SLTU = 3,
             ALU_XOR = 4, ALU_SRL = 5, ALU_SRA = 13, ALU_OR = 6, ALU_AND = 7 };

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vid_stage* dut = new Vid_stage;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        ID Stage Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // Initialize
    dut->clk = 0;
    dut->rst = 0;
    dut->wb_reg_write_en = 0;
    dut->wb_rd_addr = 0;
    dut->wb_rd_wdata = 0;
    dut->if_pc = 0;
    dut->if_instr = 0;

    // Reset the regfile
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;

    // -----------------
    // Test 1: PC Pass-through
    dut->if_pc = 0x80001000ULL;
    dut->if_instr = 0x00000013; // NOP (ADDI x0, x0, 0)
    dut->eval();
    check(dut->id_pc == 0x80001000ULL, "PC_Passthrough");

    // -----------------
    // Test 2: Decode ADDI x1, x0, 42 (0x02A00093)
    dut->if_instr = 0x02A00093;
    dut->eval();
    check(dut->id_rd_addr == 1,         "ADDI_rd_addr");
    check(dut->id_alu_op == ALU_ADD,    "ADDI_alu_op");
    check(dut->id_alu_src2_sel == 1,    "ADDI_alu_src2_sel");
    check(dut->id_imm == 42,            "ADDI_imm");
    check(dut->id_reg_write_en == 1,    "ADDI_reg_write_en");
    check(dut->id_is_branch == 0,       "ADDI_not_branch");
    check(dut->id_is_jump == 0,         "ADDI_not_jump");
    check(dut->id_illegal_instr == 0,   "ADDI_not_illegal");

    // -----------------
    // Test 3: RegFile write-back then read
    //   Write 0xDEADBEEF to x5 via WB port, then decode an instruction that reads x5
    dut->wb_reg_write_en = 1;
    dut->wb_rd_addr = 5;
    dut->wb_rd_wdata = 0xDEADBEEF;
    tick(dut);
    dut->wb_reg_write_en = 0;

    // ADD x1, x5, x0 → 0x000280B3
    // rs1=x5, rs2=x0, rd=x1
    dut->if_instr = 0x000280B3;
    dut->eval();
    check(dut->id_rs1_addr == 5,            "R_Type_rs1_addr");
    check(dut->id_rs2_addr == 0,            "R_Type_rs2_addr");
    check(dut->id_rs1_rdata == 0xDEADBEEF,  "RegFile_Read_x5");
    check(dut->id_rs2_rdata == 0,           "RegFile_Read_x0");
    check(dut->id_alu_src2_sel == 0,        "R_Type_alu_src2_sel");

    // -----------------
    // Test 4: Dual-port read via RegFile
    //   Write 0x11111111 to x10, then decode ADD x1, x5, x10
    dut->wb_reg_write_en = 1;
    dut->wb_rd_addr = 10;
    dut->wb_rd_wdata = 0x11111111;
    tick(dut);
    dut->wb_reg_write_en = 0;

    // ADD x1, x5, x10 → funct7=0000000, rs2=01010, rs1=00101, funct3=000, rd=00001, op=0110011
    // = 0x00A280B3
    dut->if_instr = 0x00A280B3;
    dut->eval();
    check(dut->id_rs1_rdata == 0xDEADBEEF, "DualRead_rs1_x5");
    check(dut->id_rs2_rdata == 0x11111111,  "DualRead_rs2_x10");

    // -----------------
    // Test 5: Branch BEQ x1, x2, -4
    //   Encoding: 0xFE208EE3
    dut->if_instr = 0xFE208EE3;
    dut->eval();
    check(dut->id_is_branch == 1,       "BEQ_is_branch");
    check(dut->id_reg_write_en == 0,    "BEQ_no_reg_write");
    check(dut->id_branch_op == 0,       "BEQ_branch_op");
    check(dut->id_imm == (uint64_t)(-4),"BEQ_imm_neg4");

    // -----------------
    // Test 6: JAL x1, +8 (0x008000EF)
    dut->if_instr = 0x008000EF;
    dut->eval();
    check(dut->id_is_jump == 1,         "JAL_is_jump");
    check(dut->id_reg_write_en == 1,    "JAL_reg_write_en");
    check(dut->id_wb_sel == 2,          "JAL_wb_sel_PC4");
    check(dut->id_imm == 8,             "JAL_imm_8");

    // -----------------
    // Test 7: LD x5, 8(x10) (0x00853283)
    dut->if_instr = 0x00853283;
    dut->eval();
    check(dut->id_is_load == 1,         "LD_is_load");
    check(dut->id_reg_write_en == 1,    "LD_reg_write_en");
    check(dut->id_wb_sel == 1,          "LD_wb_sel_MEM");
    check(dut->id_imm == 8,             "LD_imm_8");
    check(dut->id_mem_size == 3,        "LD_mem_size_dword");

    // -----------------
    // Test 8: SD x5, 16(x10) (0x00553823)
    dut->if_instr = 0x00553823;
    dut->eval();
    check(dut->id_is_store == 1,        "SD_is_store");
    check(dut->id_reg_write_en == 0,    "SD_no_reg_write");
    check(dut->id_imm == 16,            "SD_imm_16");

    // -----------------
    // Test 9: Illegal instruction (0x00000000)
    dut->if_instr = 0x00000000;
    dut->eval();
    check(dut->id_illegal_instr == 1,   "Illegal_Instr");

    // -----------------
    // Test 10: ECALL (0x00000073)
    dut->if_instr = 0x00000073;
    dut->eval();
    check(dut->id_is_ecall == 1,        "ECALL");

    // -----------------
    // Test 11: LUI x1, 0x12345 (0x123450B7)
    dut->if_instr = 0x123450B7;
    dut->eval();
    check(dut->id_reg_write_en == 1,    "LUI_reg_write_en");
    check(dut->id_imm == 0x12345000ULL, "LUI_imm");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
