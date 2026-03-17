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

// File path: rtl/core/id_stage.sv
// Description: Instruction Decode Stage, instantiate the Decoder and RegFile, extract control signals and operands

module id_stage
    import rv64_pkg::*;
(
    input  logic                   clk,
    input  logic                   rst,

    // Inputs from IF Stage (via IF/ID Pipeline Register)
    input  logic [XLEN-1:0]        if_pc,        // PC of the current instruction
    input  logic [INST_WIDTH-1:0]  if_instr,     // The 32-bit instruction fetched

    // Write-back Inputs from WB Stage (to RegFile)
    input  logic                   wb_reg_write_en, // Register write enable from WB stage
    input  logic [4:0]             wb_rd_addr,     // Destination register address from WB
    input  logic [XLEN-1:0]        wb_rd_wdata,    // Data to write back to register
    
    // Outputs to EX Stage
    // (Notice the 'id_' prefix to signify signals leaving the ID stage)
    // Data/Operands
    output logic [XLEN-1:0]        id_pc,          // Pass-through PC for branch/jump calculation
    output logic [XLEN-1:0]        id_rs1_rdata,   // Source register 1 data
    output logic [XLEN-1:0]        id_rs2_rdata,   // Source register 2 data
    output logic [4:0]             id_rs1_addr,    // Rs1 index (for Forwarding & CSR zimm)
    output logic [4:0]             id_rs2_addr,    // Rs2 index (for Forwarding)
    output logic [XLEN-1:0]        id_imm,         // Sign-extended generic immediate

    // Execution Control (ALU & Branch)
    output alu_op_t                id_alu_op,      // ALU operation code
    output logic                   id_alu_src1_sel,// 0: rs1_rdata, 1: PC
    output logic                   id_alu_src2_sel,// 0: rs2_rdata, 1: imm
    output logic                   id_is_word_op,  // 1: RV64 32-bit operation (e.g. ADDW)
    output logic                   id_is_branch,   // 1: B-type branch instruction
    output branch_op_t             id_branch_op,   // Specific condition for branch (BEQ, BNE...)
    output logic                   id_is_jump,     // 1: JAL or JALR

    // Memory & Writeback Control (Passed down to MEM/WB stages eventually)
    output logic                   id_is_load,     // 1: Load instruction
    output logic                   id_is_store,    // 1: Store instruction
    output logic [2:0]             id_mem_size,    // Memory access size (Byte/Half/Word/Double)
    output logic                   id_reg_write_en,// 1: Instruction will write to a register
    output logic [4:0]             id_rd_addr,     // Destination register index
    output logic [1:0]             id_wb_sel,      // Writeback source (0: ALU, 1: MEM, 2: PC+4)

    // System / Exception
    output logic                   id_is_csr,
    output logic [2:0]             id_csr_op,
    output logic                   id_illegal_instr,
    output logic                   id_is_ecall,
    output logic                   id_is_ebreak,
    output logic                   id_is_mret,
    output logic                   id_is_sret,
    output logic                   id_is_wfi,
    output logic                   id_is_sfence_vma,
    output logic                   id_is_fence,
    output logic                   id_is_fence_i
);
    // Internal Wires for connecting sub-modules
    logic [4:0] rs1_addr;
    logic [4:0] rs2_addr;

    // Pass-through PC and reg indices directly to next stage
    assign id_pc = if_pc;
    assign id_rs1_addr = rs1_addr;
    assign id_rs2_addr = rs2_addr;

    // Instruction Decoder Instantiation

    decoder u_decoder (
        .instr          (if_instr),

        // Outputs to RegFile read ports
        .rs1_addr       (rs1_addr),
        .rs2_addr       (rs2_addr),
        .rd_addr        (id_rd_addr),

        // Outputs to EX stage (ALU & execution control)
        .alu_op         (id_alu_op),
        .alu_src1_sel   (id_alu_src1_sel),
        .alu_src2_sel   (id_alu_src2_sel),
        .is_word_op     (id_is_word_op),
        .imm            (id_imm),
        
        // Branch & Jump control
        .is_branch      (id_is_branch),
        .branch_op      (id_branch_op),
        .is_jump        (id_is_jump),
        
        // Memory control
        .is_load        (id_is_load),
        .is_store       (id_is_store),
        .mem_size       (id_mem_size),
        
        // Writeback control
        .reg_write_en   (id_reg_write_en),
        .wb_sel         (id_wb_sel),
        
        // System Control
        .is_csr         (id_is_csr),
        .csr_op         (id_csr_op),
        .is_ecall       (id_is_ecall),
        .is_ebreak      (id_is_ebreak),
        .is_mret        (id_is_mret),
        .is_sret        (id_is_sret),
        .is_wfi         (id_is_wfi),
        .is_sfence_vma  (id_is_sfence_vma),
        .is_fence       (id_is_fence),
        .is_fence_i     (id_is_fence_i),
        .illegal_instr  (id_illegal_instr)
    );

    // Register File Instantiation
    regfile u_regfile (
        .clk            (clk),
        .rst            (rst),
        
        // Read ports (from Decoder)
        .rs1_addr       (rs1_addr),
        .rs2_addr       (rs2_addr),
        .rs1_rdata      (id_rs1_rdata),
        .rs2_rdata      (id_rs2_rdata),
        
        // Write port (from WB stage)
        .reg_write_en   (wb_reg_write_en),
        .rd_addr        (wb_rd_addr),
        .rd_wdata       (wb_rd_wdata)
    );

endmodule
