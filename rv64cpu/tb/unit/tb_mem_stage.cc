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

// File path: tb/unit/tb_mem_stage.cc
// Description: Memory Stage unit test

#include <iostream>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vmem_stage.h"

int pass_count = 0;

void tick(Vmem_stage* dut) {
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
    Vmem_stage* dut = new Vmem_stage;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        MEM Stage Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // 1. Initial / Default states
    dut->mem_alu_result = 0;
    dut->mem_rs2_rdata = 0;
    dut->mem_is_load = 0;
    dut->mem_is_store = 0;
    dut->mem_mem_size = 0;
    dut->dmem_req_ready = 1;
    dut->dmem_rsp_valid = 1;
    dut->dmem_rdata = 0;

    tick(dut);

    check(dut->dmem_read_req == 0, "No_Read_Req");
    check(dut->dmem_write_req == 0, "No_Write_Req");
    check(dut->mem_stall_req == 0, "No_Stall");

    // 2. Passthrough signals
    dut->mem_reg_write_en = 1;
    dut->mem_rd_addr = 15;
    dut->mem_wb_sel = 2; // PC+4
    dut->mem_pc = 0x80001000;
    dut->mem_is_csr = 1;
    dut->mem_csr_op = 2; // CSRRS
    dut->mem_rs1_rdata = 0x1234;
    dut->mem_rs1_addr = 5;
    dut->mem_imm = 0x777;

    tick(dut);

    check(dut->mem_wb_reg_write_en == 1, "Passthrough_RegWriteEn");
    check(dut->mem_wb_rd_addr == 15, "Passthrough_RdAddr");
    check(dut->mem_wb_wb_sel == 2, "Passthrough_WbSel");
    check(dut->mem_wb_pc == 0x80001000, "Passthrough_PC");
    check(dut->mem_wb_is_csr == 1, "Passthrough_IsCSR");
    check(dut->mem_wb_csr_op == 2, "Passthrough_CSROp");
    check(dut->mem_wb_rs1_rdata == 0x1234, "Passthrough_RS1Rdata");
    check(dut->mem_wb_rs1_addr == 5, "Passthrough_RS1Addr");
    check(dut->mem_wb_imm == 0x777, "Passthrough_Imm");

    // 3. Store Alignment (SB, SH, SW, SD)
    dut->mem_is_store = 1;
    dut->dmem_req_ready = 1;
    dut->mem_rs2_rdata = 0x1122334455667788ULL;

    // SB at offset 0
    dut->mem_mem_size = 0; // SB
    dut->mem_alu_result = 0x1000;
    tick(dut);
    check(dut->dmem_write_req == 1, "Store_Req");
    check(dut->dmem_addr == 0x1000, "Store_Addr");
    check(dut->dmem_wdata == 0x88, "SB_Data_0");
    check(dut->dmem_wstrb == 0x01, "SB_Strobe_0");

    // SB at offset 3
    dut->mem_alu_result = 0x1003;
    tick(dut);
    check(dut->dmem_wdata == 0x88000000ULL, "SB_Data_3");
    check(dut->dmem_wstrb == 0x08, "SB_Strobe_3");

    // SH at offset 2
    dut->mem_mem_size = 1; // SH
    dut->mem_alu_result = 0x1002;
    tick(dut);
    check(dut->dmem_wdata == 0x77880000ULL, "SH_Data_2");
    check(dut->dmem_wstrb == 0x0C, "SH_Strobe_2");

    // SW at offset 4
    dut->mem_mem_size = 2; // SW
    dut->mem_alu_result = 0x1004;
    tick(dut);
    check(dut->dmem_wdata == 0x5566778800000000ULL, "SW_Data_4");
    check(dut->dmem_wstrb == 0xF0, "SW_Strobe_4");

    // SD
    dut->mem_mem_size = 3; // SD
    dut->mem_alu_result = 0x1000;
    tick(dut);
    check(dut->dmem_wdata == 0x1122334455667788ULL, "SD_Data");
    check(dut->dmem_wstrb == 0xFF, "SD_Strobe");

    dut->mem_is_store = 0;

    // 4. Load Alignment and Extension (LB, LH, LW, LD, LBU, LHU, LWU)
    dut->mem_is_load = 1;
    dut->dmem_rdata = 0xAABBCCDDEEFF0011ULL;
    dut->dmem_rsp_valid = 1;

    // LB at offset 0 (0x11) -> sign extended
    dut->mem_mem_size = 0; // LB
    dut->mem_alu_result = 0x2000;
    tick(dut);
    check(dut->dmem_read_req == 1, "Load_Req");
    check(dut->mem_wb_mem_rdata == 0x11, "LB_Data_0_Pos");

    // LB at offset 1 (0x00)
    dut->mem_alu_result = 0x2001;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0x0, "LB_Data_1_Zero");

    // LB at offset 7 (0xAA) -> sign extended
    dut->mem_alu_result = 0x2007;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xFFFFFFFFFFFFFFAAULL, "LB_Data_7_Neg");

    // LBU at offset 7 (0xAA) -> zero extended
    dut->mem_mem_size = 4; // LBU
    dut->mem_alu_result = 0x2007;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xAA, "LBU_Data_7");

    // LH at offset 2 (0xEEFF) -> sign extended
    dut->mem_mem_size = 1; // LH
    dut->mem_alu_result = 0x2002;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xFFFFFFFFFFFFEEFFULL, "LH_Data_2_Neg");

    // LHU at offset 2 (0xEEFF) -> zero extended
    dut->mem_mem_size = 5; // LHU
    dut->mem_alu_result = 0x2002;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xEEFF, "LHU_Data_2");

    // LW at offset 4 (0xAABBCCDD) -> sign extended
    dut->mem_mem_size = 2; // LW
    dut->mem_alu_result = 0x2004;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xFFFFFFFFAABBCCDDULL, "LW_Data_4_Neg");

    // LWU at offset 4 (0xAABBCCDD) -> zero extended
    dut->mem_mem_size = 6; // LWU
    dut->mem_alu_result = 0x2004;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xAABBCCDD, "LWU_Data_4");

    // LD
    dut->mem_mem_size = 3; // LD
    dut->mem_alu_result = 0x2000;
    tick(dut);
    check(dut->mem_wb_mem_rdata == 0xAABBCCDDEEFF0011ULL, "LD_Data");

    // 5. Stall Logic
    dut->dmem_req_ready = 0; // Cache miss / AXI busy
    dut->dmem_rsp_valid = 0;
    tick(dut);
    check(dut->mem_stall_req == 1, "Stall_On_Req_NotReady");

    dut->dmem_req_ready = 1;
    dut->dmem_rsp_valid = 0; // Read request accepted, but data not valid yet
    tick(dut);
    check(dut->mem_stall_req == 1, "Stall_On_Rsp_NotValid");

    dut->dmem_rsp_valid = 1; // Read response arrived
    tick(dut);
    check(dut->mem_stall_req == 0, "No_Stall_On_RspValid");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
