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

// File path: tb/unit/tb_decoder.cc
// Description: Decoder unit test

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cstdlib>
#include <verilated.h>
#include "Vdecoder.h"

static int test_count = 0;
static int fail_count = 0;

#define CHECK(dut, signal, expected, test_name) \
    do { \
        uint64_t _got = (uint64_t)(dut->signal); \
        uint64_t _exp = (uint64_t)(expected); \
        if (_got != _exp) { \
            std::cerr << "\033[31m[FAIL] " << test_name \
                      << " | " << #signal << " expected: 0x" \
                      << std::hex << _exp << " got: 0x" << _got \
                      << std::dec << "\033[0m" << std::endl; \
            fail_count++; \
        } \
    } while(0)

void decode(Vdecoder* dut, uint32_t instr, const char* name) {
    dut->instr = instr;
    dut->eval();
    test_count++;
    std::cout << "\033[32m[PASS] [TEST " << std::setw(2) << test_count << "] " << name
              << "  (0x" << std::hex << std::setw(8) << std::setfill('0')
              << instr << std::dec << std::setfill(' ') << ")\033[0m" << std::endl;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vdecoder* dut = new Vdecoder;

    std::cout << "═══════════════════════════════════════════════" << std::endl;
    std::cout << "        Decoder Unit Test Suite" << std::endl;
    std::cout << "═══════════════════════════════════════════════" << std::endl;

    // 1. R-type: ADD x1, x2, x3
    //    funct7=0000000 rs2=00011 rs1=00010 funct3=000 rd=00001 op=0110011
    decode(dut, 0x003100B3, "ADD  x1, x2, x3");
    CHECK(dut, rs1_addr,     2,  "ADD");
    CHECK(dut, rs2_addr,     3,  "ADD");
    CHECK(dut, rd_addr,      1,  "ADD");
    CHECK(dut, reg_write_en, 1,  "ADD");       // R-type writes rd
    CHECK(dut, alu_op,       0,  "ADD");       // ALU_ADD = 0b0000
    CHECK(dut, alu_src1_sel, 0,  "ADD");       // operand1 = rs1
    CHECK(dut, alu_src2_sel, 0,  "ADD");       // operand2 = rs2
    CHECK(dut, wb_sel,       0,  "ADD");       // writeback from ALU
    CHECK(dut, illegal_instr,0,  "ADD");

    // 2. R-type: SUB x3, x1, x2
    //    funct7=0100000 rs2=00010 rs1=00001 funct3=000 rd=00011 op=0110011
    //    Tests: funct7[5] distinguishes ADD vs SUB
    decode(dut, 0x402081B3, "SUB  x3, x1, x2");
    CHECK(dut, alu_op,       8,  "SUB");       // ALU_SUB = 0b1000
    CHECK(dut, reg_write_en, 1,  "SUB");
    CHECK(dut, alu_src2_sel, 0,  "SUB");       // operand2 = rs2

    // 3. R-type: SRA x1, x2, x3  (vs SRL)
    //    funct7=0100000 rs2=00011 rs1=00010 funct3=101 rd=00001 op=0110011
    //    Tests: funct7[5] distinguishes SRL vs SRA for funct3=101
    decode(dut, 0x403150B3, "SRA  x1, x2, x3");
    CHECK(dut, alu_op,       13, "SRA");       // ALU_SRA = 0b1101

    // 4. R-type: SRL x1, x2, x3
    //    funct7=0000000 rs2=00011 rs1=00010 funct3=101 rd=00001 op=0110011
    decode(dut, 0x003150B3, "SRL  x1, x2, x3");
    CHECK(dut, alu_op,       5,  "SRL");       // ALU_SRL = 0b0101

    // 5. I-type ALU: ADDI x1, x0, 42
    //    imm=000000101010 rs1=00000 funct3=000 rd=00001 op=0010011
    //    Tests: I-type sets alu_src2_sel=1 (immediate), reg_write=1
    decode(dut, 0x02A00093, "ADDI x1, x0, 42");
    CHECK(dut, reg_write_en, 1,  "ADDI");
    CHECK(dut, alu_op,       0,  "ADDI");      // ALU_ADD
    CHECK(dut, alu_src1_sel, 0,  "ADDI");      // rs1
    CHECK(dut, alu_src2_sel, 1,  "ADDI");      // immediate
    CHECK(dut, imm,          42, "ADDI");
    CHECK(dut, rd_addr,      1,  "ADDI");
    CHECK(dut, rs1_addr,     0,  "ADDI");

    // 6. I-type ALU: ADDI x1, x0, -1  (negative immediate sign extension)
    //    imm=111111111111 rs1=00000 funct3=000 rd=00001 op=0010011
    //    Tests: 12-bit immediate sign-extended to 64-bit
    decode(dut, 0xFFF00093, "ADDI x1, x0, -1");
    CHECK(dut, imm, (uint64_t)-1, "ADDI -1");  // 0xFFFFFFFFFFFFFFFF

    // 7. I-type ALU: SRAI x1, x2, 3
    //    imm=010000_000011 rs1=00010 funct3=101 rd=00001 op=0010011
    //    Tests: funct7[5]=1 selects SRA for funct3=101 in OP_IMM
    decode(dut, 0x40315093, "SRAI x1, x2, 3");
    CHECK(dut, alu_op,       13, "SRAI");      // ALU_SRA = 0b1101
    CHECK(dut, alu_src2_sel, 1,  "SRAI");      // immediate (shamt)

    // 8. Load: LD x5, 8(x10)
    //    imm=000000001000 rs1=01010 funct3=011 rd=00101 op=0000011
    //    Tests: is_load, mem_size, wb_sel=MEM, alu computes address
    decode(dut, 0x00853283, "LD   x5, 8(x10)");
    CHECK(dut, is_load,      1,  "LD");
    CHECK(dut, reg_write_en, 1,  "LD");
    CHECK(dut, alu_src2_sel, 1,  "LD");        // imm for address calc
    CHECK(dut, imm,          8,  "LD");
    CHECK(dut, mem_size,     3,  "LD");        // funct3=011 → doubleword
    CHECK(dut, wb_sel,       1,  "LD");        // writeback from MEM
    CHECK(dut, alu_op,       0,  "LD");        // ALU_ADD for addr

    // 9. Store: SD x5, 16(x10)
    //    imm[11:5]=0000000 rs2=00101 rs1=01010 funct3=011 imm[4:0]=10000 op=0100011
    //    Tests: is_store, reg_write=0, S-type immediate extraction
    decode(dut, 0x00553823, "SD   x5, 16(x10)");
    CHECK(dut, is_store,     1,  "SD");
    CHECK(dut, reg_write_en, 0,  "SD");        // stores don't write rd
    CHECK(dut, alu_src2_sel, 1,  "SD");        // imm for address calc (NOT rs2)
    CHECK(dut, imm,          16, "SD");        // S-type immediate = 16
    CHECK(dut, mem_size,     3,  "SD");        // funct3=011 → doubleword
    CHECK(dut, rs1_addr,     10, "SD");        // base register
    CHECK(dut, rs2_addr,     5,  "SD");        // data to store

    // 10. Branch: BEQ x1, x2, -4
    //     Tests: is_branch, branch_op=funct3, negative B-type immediate
    decode(dut, 0xFE208EE3, "BEQ  x1, x2, -4");
    CHECK(dut, is_branch,    1,  "BEQ");
    CHECK(dut, reg_write_en, 0,  "BEQ");
    CHECK(dut, alu_src1_sel, 1,  "BEQ");       // PC for target calc
    CHECK(dut, branch_op,    0,  "BEQ");       // funct3=000 → BEQ
    CHECK(dut, imm, (uint64_t)(-4), "BEQ");    // sign-extended -4

    // 11. Branch: BNE x1, x2, +8
    //     imm[12]=0 imm[10:5]=000000 rs2=00010 rs1=00001 funct3=001
    //     imm[4:1]=0100 imm[11]=0 op=1100011
    //     Tests: branch_op correctly passes funct3 for BNE
    decode(dut, 0x00209463, "BNE  x1, x2, +8");
    CHECK(dut, is_branch,    1,  "BNE");
    CHECK(dut, branch_op,    1,  "BNE");       // funct3=001 → BNE
    CHECK(dut, imm,          8,  "BNE");

    // 12. LUI x1, 0x12345
    //     imm[31:12]=0x12345 rd=00001 op=0110111
    //     Tests: U-type immediate, reg_write, alu_src2_sel=imm
    decode(dut, 0x123450B7, "LUI  x1, 0x12345");
    CHECK(dut, reg_write_en, 1,  "LUI");
    CHECK(dut, alu_src2_sel, 1,  "LUI");       // immediate
    CHECK(dut, imm,  0x12345000ULL, "LUI");    // U-type: imm << 12
    CHECK(dut, wb_sel,       0,  "LUI");       // ALU result
    CHECK(dut, rd_addr,      1,  "LUI");

    // 13. AUIPC x2, 0x00001
    //     imm[31:12]=0x00001 rd=00010 op=0010111
    //     Tests: alu_src1_sel=PC (distinguishes from LUI)
    decode(dut, 0x00001117, "AUIPC x2, 0x1");
    CHECK(dut, reg_write_en, 1,  "AUIPC");
    CHECK(dut, alu_src1_sel, 1,  "AUIPC");     // PC  ← key difference from LUI
    CHECK(dut, alu_src2_sel, 1,  "AUIPC");     // immediate
    CHECK(dut, imm,  0x1000ULL,  "AUIPC");     // 0x00001 << 12 = 0x1000

    // 14. JAL x1, +8
    //     J-type imm encoding: imm[20|10:1|11|19:12]
    //     offset=8 → imm[20]=0 imm[10:1]=0000000100 imm[11]=0 imm[19:12]=00000000
    //     rd=00001 op=1101111
    //     Tests: is_jump, wb_sel=PC+4, alu_src1=PC
    decode(dut, 0x008000EF, "JAL  x1, +8");
    CHECK(dut, is_jump,      1,  "JAL");
    CHECK(dut, reg_write_en, 1,  "JAL");
    CHECK(dut, alu_src1_sel, 1,  "JAL");       // PC
    CHECK(dut, alu_src2_sel, 1,  "JAL");       // immediate
    CHECK(dut, wb_sel,       2,  "JAL");       // writeback PC+4
    CHECK(dut, imm,          8,  "JAL");

    // 15. JALR x1, x5, 4
    //     imm=000000000100 rs1=00101 funct3=000 rd=00001 op=1100111
    //     Tests: is_jump, alu_src1=rs1 (distinguishes from JAL)
    decode(dut, 0x004280E7, "JALR x1, x5, 4");
    CHECK(dut, is_jump,      1,  "JALR");
    CHECK(dut, reg_write_en, 1,  "JALR");
    CHECK(dut, alu_src1_sel, 0,  "JALR");      // rs1  ← key difference from JAL
    CHECK(dut, alu_src2_sel, 1,  "JALR");      // immediate
    CHECK(dut, wb_sel,       2,  "JALR");      // writeback PC+4
    CHECK(dut, imm,          4,  "JALR");
    CHECK(dut, rs1_addr,     5,  "JALR");

    // 16. ADDIW x1, x2, 10  (RV64 word-width I-type)
    //     imm=000000001010 rs1=00010 funct3=000 rd=00001 op=0011011
    //     Tests: is_word_op flag for 32-bit operation
    decode(dut, 0x00A1009B, "ADDIW x1, x2, 10");
    CHECK(dut, reg_write_en, 1,  "ADDIW");
    CHECK(dut, is_word_op,   1,  "ADDIW");     // 32-bit + sext
    CHECK(dut, alu_op,       0,  "ADDIW");     // ALU_ADD
    CHECK(dut, alu_src2_sel, 1,  "ADDIW");     // immediate
    CHECK(dut, imm,          10, "ADDIW");

    // 17. ADDW x3, x1, x2  (RV64 word-width R-type)
    //     funct7=0000000 rs2=00010 rs1=00001 funct3=000 rd=00011 op=0111011
    //     Tests: OP_32 path, is_word_op + R-type (alu_src2=rs2)
    decode(dut, 0x002081BB, "ADDW x3, x1, x2");
    CHECK(dut, reg_write_en, 1,  "ADDW");
    CHECK(dut, is_word_op,   1,  "ADDW");
    CHECK(dut, alu_op,       0,  "ADDW");      // ALU_ADD
    CHECK(dut, alu_src2_sel, 0,  "ADDW");      // rs2 (R-type!)

    // 18. SUBW x3, x1, x2  (RV64 word-width R-type with funct7[5]=1)
    //     funct7=0100000 rs2=00010 rs1=00001 funct3=000 rd=00011 op=0111011
    decode(dut, 0x402081BB, "SUBW x3, x1, x2");
    CHECK(dut, alu_op,       8,  "SUBW");      // ALU_SUB
    CHECK(dut, is_word_op,   1,  "SUBW");

    // 19. CSRRW x1, mstatus(0x300), x2
    //     imm=001100000000 rs1=00010 funct3=001 rd=00001 op=1110011
    //     Tests: is_csr, csr_op=funct3, reg_write=1
    decode(dut, 0x300110F3, "CSRRW x1, mstatus, x2");
    CHECK(dut, is_csr,       1,  "CSRRW");
    CHECK(dut, csr_op,       1,  "CSRRW");     // funct3=001 → CSRRW
    CHECK(dut, reg_write_en, 1,  "CSRRW");
    CHECK(dut, illegal_instr,0,  "CSRRW");

    // 20. ECALL
    //     all-zero fields + opcode=1110011
    decode(dut, 0x00000073, "ECALL");
    CHECK(dut, is_ecall,     1,  "ECALL");
    CHECK(dut, reg_write_en, 0,  "ECALL");
    CHECK(dut, illegal_instr,0,  "ECALL");

    // 21. EBREAK
    //     funct7=0000000 rs2=00001 rs1=00000 funct3=000 rd=00000 op=1110011
    decode(dut, 0x00100073, "EBREAK");
    CHECK(dut, is_ebreak,    1,  "EBREAK");
    CHECK(dut, reg_write_en, 0,  "EBREAK");

    // 22. MRET
    //     funct7=0011000 rs2=00010 rs1=00000 funct3=000 rd=00000 op=1110011
    //     Tests: privileged instruction decode from {funct7, rs2}
    decode(dut, 0x30200073, "MRET");
    CHECK(dut, is_mret,      1,  "MRET");
    CHECK(dut, reg_write_en, 0,  "MRET");
    CHECK(dut, illegal_instr,0,  "MRET");

    // 23. SRET
    //     funct7=0001000 rs2=00010 rs1=00000 funct3=000 rd=00000 op=1110011
    decode(dut, 0x10200073, "SRET");
    CHECK(dut, is_sret,      1,  "SRET");
    CHECK(dut, reg_write_en, 0,  "SRET");

    // 24. WFI
    //     funct7=0001000 rs2=00101 rs1=00000 funct3=000 rd=00000 op=1110011
    decode(dut, 0x10500073, "WFI");
    CHECK(dut, is_wfi,       1,  "WFI");

    // 25. FENCE
    //     pred=1111 succ=1111 rs1=00000 funct3=000 rd=00000 op=0001111
    //     Tests: MISC_MEM path, is_fence flag
    decode(dut, 0x0FF0000F, "FENCE");
    CHECK(dut, is_fence,     1,  "FENCE");
    CHECK(dut, illegal_instr,0,  "FENCE");

    // 26. FENCE.I (Zifencei)
    //     funct3=001 op=0001111
    //     Tests: is_fence_i flag (requires I-Cache flush in pipeline)
    decode(dut, 0x0000100F, "FENCE.I");
    CHECK(dut, is_fence_i,   1,  "FENCE.I");

    // 27. Illegal instruction (opcode=0000000)
    //     Tests: default case sets illegal_instr, reg_write stays 0
    decode(dut, 0x00000000, "Illegal (all-zero)");
    CHECK(dut, illegal_instr,1,  "Illegal");
    CHECK(dut, reg_write_en, 0,  "Illegal");

    //  Results
    std::cout << "═══════════════════════════════════════════════" << std::endl;
    if (fail_count == 0) {
        std::cout << "\033[32mAll " << test_count << " tests passed!\033[0m" << std::endl;
    } else {
        std::cout << "\033[31m" << fail_count << " check(s) FAILED out of "
                  << test_count << " tests.\033[0m" << std::endl;
    }

    delete dut;
    return (fail_count > 0) ? 1 : 0;
}