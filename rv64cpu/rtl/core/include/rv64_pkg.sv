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
    typedef enum logic [4:0] {
        ALU_ADD    = 5'd0,   // Add
        ALU_SUB    = 5'd8,   // Subtract
        ALU_SLL    = 5'd1,   // Shift Left Logical
        ALU_SLT    = 5'd2,   // Set Less Than (signed)
        ALU_SLTU   = 5'd3,   // Set Less Than (unsigned)
        ALU_XOR    = 5'd4,   // Exclusive Or
        ALU_SRL    = 5'd5,   // Shift Right Logical
        ALU_SRA    = 5'd13,  // Shift Right Arithmetic
        ALU_OR     = 5'd6,   // Or
        ALU_AND    = 5'd7,   // And
        ALU_MUL    = 5'd16,  // Low XLEN bits of signed multiplication
        ALU_MULH   = 5'd17,  // High XLEN bits of signed x signed multiplication
        ALU_MULHSU = 5'd18,  // High XLEN bits of signed x unsigned multiplication
        ALU_MULHU  = 5'd19,  // High XLEN bits of unsigned x unsigned multiplication
        ALU_DIV    = 5'd20,  // Signed division
        ALU_DIVU   = 5'd21,  // Unsigned division
        ALU_REM    = 5'd22,  // Signed remainder
        ALU_REMU   = 5'd23   // Unsigned remainder
    } alu_op_t;


    // Branch Operation Encoding
    // Derived from funct3 of B-type instructions
    typedef enum logic [2:0] {
        BRANCH_EQ  = 3'b000,  // BEQ:  Branch if Equal
        BRANCH_NE  = 3'b001,  // BNE:  Branch if Not Equal
        BRANCH_LT  = 3'b100,  // BLT:  Branch if Less Than (signed)
        BRANCH_GE  = 3'b101,  // BGE:  Branch if Greater Than or Equal (signed)
        BRANCH_LTU = 3'b110,  // BLTU: Branch if Less Than (unsigned)
        BRANCH_GEU = 3'b111   // BGEU: Branch if Greater Than or Equal (unsigned)
    } branch_op_t;

    typedef enum logic [3:0] {
        AMO_NONE = 4'd0,
        AMO_LR   = 4'd1,
        AMO_SC   = 4'd2,
        AMO_SWAP = 4'd3,
        AMO_ADD  = 4'd4,
        AMO_XOR  = 4'd5,
        AMO_AND  = 4'd6,
        AMO_OR   = 4'd7,
        AMO_MIN  = 4'd8,
        AMO_MAX  = 4'd9,
        AMO_MINU = 4'd10,
        AMO_MAXU = 4'd11
    } amo_op_t;

    typedef enum logic [4:0] {
        FP_OP_NONE     = 5'd0,
        FP_OP_ADD      = 5'd1,
        FP_OP_SUB      = 5'd2,
        FP_OP_MUL      = 5'd3,
        FP_OP_DIV      = 5'd4,
        FP_OP_SQRT     = 5'd5,
        FP_OP_SGNJ     = 5'd6,
        FP_OP_SGNJN    = 5'd7,
        FP_OP_SGNJX    = 5'd8,
        FP_OP_MIN      = 5'd9,
        FP_OP_MAX      = 5'd10,
        FP_OP_CVT_F2I  = 5'd11,
        FP_OP_CVT_I2F  = 5'd12,
        FP_OP_CVT_F2F  = 5'd13,
        FP_OP_MV_F2X   = 5'd14,
        FP_OP_MV_X2F   = 5'd15,
        FP_OP_EQ       = 5'd16,
        FP_OP_LT       = 5'd17,
        FP_OP_LE       = 5'd18,
        FP_OP_CLASS    = 5'd19,
        FP_OP_MADD     = 5'd20,
        FP_OP_MSUB     = 5'd21,
        FP_OP_NMSUB    = 5'd22,
        FP_OP_NMADD    = 5'd23
    } fp_op_t;

    typedef enum logic [1:0] {
        FP_FMT_S = 2'd0,
        FP_FMT_D = 2'd1
    } fp_fmt_t;

endpackage
