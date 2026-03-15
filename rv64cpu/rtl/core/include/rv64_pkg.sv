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

// File path: rtl/core/include/rv64_pkg.sv
// Description: RISC-V 64-bit base instruction set global definition package

package rv64_pkg;

    localparam XLEN       = 64;
    localparam INST_WIDTH = 32;

    // RISC-V Base Opcode Map (inst[6:0])
    // Reference: RISC-V Unprivileged ISA V20240411, Table 34
    typedef enum logic [6:0] {
        // Loads
        OPCODE_LOAD      = 7'b000_0011,  // LB/LH/LW/LD/LBU/LHU/LWU
        OPCODE_LOAD_FP   = 7'b000_0111,  // FLW/FLD

        // Misc-Mem
        OPCODE_MISC_MEM  = 7'b000_1111,  // FENCE / FENCE.I (Zifencei)

        // Immediate ALU
        OPCODE_OP_IMM    = 7'b001_0011,  // ADDI/SLTI/ANDI/ORI/XORI/SLLI/SRLI/SRAI...
        OPCODE_OP_IMM_32 = 7'b001_1011,  // ADDIW/SLLIW/SRLIW/SRAIW (RV64 only)

        // Upper Immediate
        OPCODE_AUIPC     = 7'b001_0111,  // AUIPC
        OPCODE_LUI       = 7'b011_0111,  // LUI

        // Stores
        OPCODE_STORE     = 7'b010_0011,  // SB/SH/SW/SD
        OPCODE_STORE_FP  = 7'b010_0111,  // FSW/FSD

        // Atomic (A extension)
        OPCODE_AMO       = 7'b010_1111,  // LR/SC/AMOSWAP/AMOADD/...

        // Register ALU
        OPCODE_OP        = 7'b011_0011,  // ADD/SUB/AND/OR/XOR/SLL/SRL/SRA/SLT/SLTU/MUL/DIV...
        OPCODE_OP_32     = 7'b011_1011,  // ADDW/SUBW/SLLW/SRLW/SRAW/MULW/DIVW... (RV64 only)

        // Floating-point fused multiply-add
        OPCODE_MADD      = 7'b100_0011,  // FMADD.S/D
        OPCODE_MSUB      = 7'b100_0111,  // FMSUB.S/D
        OPCODE_NMSUB     = 7'b100_1011,  // FNMSUB.S/D
        OPCODE_NMADD     = 7'b100_1111,  // FNMADD.S/D

        // Floating-point arithmetic
        OPCODE_OP_FP     = 7'b101_0011,  // FADD/FSUB/FMUL/FDIV/FSQRT/FCMP/FCVT/FMV/FCLASS...

        // Branches
        OPCODE_BRANCH    = 7'b110_0011,  // BEQ/BNE/BLT/BGE/BLTU/BGEU

        // Jumps
        OPCODE_JALR      = 7'b110_0111,  // JALR
        OPCODE_JAL       = 7'b110_1111,  // JAL

        // System
        OPCODE_SYSTEM    = 7'b111_0011,  // ECALL/EBREAK/CSR*/MRET/SRET/WFI/SFENCE.VMA

        // Custom (reserved for extensions)
        OPCODE_CUSTOM_0  = 7'b000_1011,
        OPCODE_CUSTOM_1  = 7'b010_1011,
        OPCODE_CUSTOM_2  = 7'b101_1011,
        OPCODE_CUSTOM_3  = 7'b111_1011
    } opcode_t;

    // ALU Operation Encoding
    // Derived from funct3 + funct7[5] of R-type / I-type instructions
    typedef enum logic [3:0] {
        ALU_ADD  = 4'b0000,  // Add
        ALU_SUB  = 4'b1000,  // Subtract
        ALU_SLL  = 4'b0001,  // Shift Left Logical
        ALU_SLT  = 4'b0010,  // Set Less Than (signed)
        ALU_SLTU = 4'b0011,  // Set Less Than (unsigned)
        ALU_XOR  = 4'b0100,  // Exclusive Or
        ALU_SRL  = 4'b0101,  // Shift Right Logical
        ALU_SRA  = 4'b1101,  // Shift Right Arithmetic
        ALU_OR   = 4'b0110,  // Or
        ALU_AND  = 4'b0111   // And
    } alu_op_t;

endpackage
