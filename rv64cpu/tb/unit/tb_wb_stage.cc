//     Richard CPU
//     Copyright (C) 2026  Richard Qin

// File path: tb/unit/tb_wb_stage.cc
// Description: WB Stage unit test

#include <iostream>
#include <cassert>
#include <verilated.h>
#include "Vwb_stage.h"

int pass_count = 0;

void tick(Vwb_stage* dut) {
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
    Vwb_stage* dut = new Vwb_stage;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        WB Stage Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // 1. Basic ALU writeback
    dut->mem_wb_alu_result = 0xAA55AA55;
    dut->mem_wb_mem_rdata = 0x11223344;
    dut->mem_wb_pc = 0x80000000;
    dut->mem_wb_pc_plus_4 = 0x80000004;
    dut->mem_wb_reg_write_en = 1;
    dut->mem_wb_rd_addr = 5;
    dut->mem_wb_wb_sel = 0; // ALU
    dut->mem_wb_is_csr = 0;
    dut->mem_wb_csr_op = 0;
    dut->mem_wb_rs1_rdata = 0x99;
    dut->mem_wb_rs1_addr = 1;
    dut->mem_wb_imm = 0x123;
    dut->mem_wb_illegal_instr = 0;
    dut->mem_wb_is_ecall = 0;
    dut->mem_wb_is_ebreak = 0;
    dut->mem_wb_is_mret = 0;
    dut->mem_wb_is_sret = 0;
    dut->csr_rdata = 0x0;

    tick(dut);

    check(dut->wb_rd_wdata == 0xAA55AA55, "WB_Sel_ALU");
    check(dut->wb_rd_addr == 5, "WB_RdAddr");
    check(dut->wb_reg_write_en == 1, "WB_RegWriteEn");
    check(dut->csr_req == 0, "CSR_Req_0");

    // 2. Memory read writeback
    dut->mem_wb_wb_sel = 1; // MEM
    tick(dut);
    check(dut->wb_rd_wdata == 0x11223344, "WB_Sel_MEM");

    // 3. PC+4 writeback (JAL/JALR)
    dut->mem_wb_wb_sel = 2; // PC+4
    tick(dut);
    check(dut->wb_rd_wdata == 0x80000004, "WB_Sel_PC_Plus_4");

    // 4. CSR Read/Write (e.g. CSRRS)
    dut->mem_wb_is_csr = 1;
    dut->mem_wb_wb_sel = 0; // Doesn't matter because is_csr forces CSR path
    dut->mem_wb_csr_op = 2; // CSRRS
    dut->mem_wb_imm = 0x300; // mstatus
    dut->mem_wb_rs1_rdata = 0x00000008; // Set MIE
    dut->mem_wb_rs1_addr = 10;
    dut->csr_rdata = 0x00001800; // Old mstatus

    tick(dut);

    check(dut->wb_rd_wdata == 0x00001800, "CSR_Read_To_Reg");
    check(dut->csr_req == 1, "CSR_Req_1");
    check(dut->csr_op == 2, "CSR_Op_Propagated");
    check(dut->csr_addr == 0x300, "CSR_Addr_Extracted");
    check(dut->csr_rs1_data == 0x00000008, "CSR_RS1_Data_Rdata");

    // 5. CSR Immediate (e.g. CSRRSI)
    dut->mem_wb_csr_op = 6; // CSRRSI (funct3 = 3'b110)
    tick(dut);
    check(dut->csr_rs1_data == 10, "CSR_RS1_Data_Zimm");

    // 6. Exception Signals Propagation
    dut->mem_wb_illegal_instr = 1;
    dut->mem_wb_is_ecall = 1;
    dut->mem_wb_is_ebreak = 1;
    dut->mem_wb_is_mret = 1;
    dut->mem_wb_is_sret = 1;
    tick(dut);

    check(dut->trap_illegal_instr == 1, "Trap_Illegal_Instr");
    check(dut->trap_is_ecall == 1, "Trap_ECALL");
    check(dut->trap_is_ebreak == 1, "Trap_EBREAK");
    check(dut->trap_is_mret == 1, "Trap_MRET");
    check(dut->trap_is_sret == 1, "Trap_SRET");
    check(dut->trap_epc == 0x80000000, "Trap_EPC");
    check(dut->wb_reg_write_en == 0, "Trap_Gates_RegWriteEn");
    check(dut->csr_req == 0, "Trap_Gates_CSR_Req");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
