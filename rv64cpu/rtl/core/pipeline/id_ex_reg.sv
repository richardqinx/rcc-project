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

// File path: rtl/core/id_ex_reg.sv
// Description: ID/EX Pipeline Register
//              Latches control signals and data from ID stage and passes them to EX stage.
//              Supports stall (freeze) and flush.

module id_ex_reg
    import rv64_pkg::*;
(
    input  logic                   clk,
    input  logic                   rst,

    // Pipeline Control
    input  logic                   stall_en,
    input  logic                   flush_en,


    // Inputs from ID Stage

    // Instruction payload
    input  logic [INST_WIDTH-1:0]  id_instr,
    input  logic [2:0]             id_instr_len,

    // Data/Operands
    input  logic [XLEN-1:0]        id_pc,
    input  logic [XLEN-1:0]        id_rs1_rdata,
    input  logic [XLEN-1:0]        id_rs2_rdata,
    input  logic [XLEN-1:0]        id_fs1_rdata,
    input  logic [XLEN-1:0]        id_fs2_rdata,
    input  logic [XLEN-1:0]        id_fs3_rdata,
    input  logic [4:0]             id_rs1_addr,
    input  logic [4:0]             id_rs2_addr,
    input  logic [4:0]             id_fs3_addr,
    input  logic [XLEN-1:0]        id_imm,

    // Execution Control
    input  alu_op_t                id_alu_op,
    input  logic                   id_alu_src1_sel,
    input  logic                   id_alu_src2_sel,
    input  logic                   id_is_word_op,
    input  logic                   id_is_branch,
    input  branch_op_t             id_branch_op,
    input  logic                   id_is_jump,

    // Memory & Writeback Control
    input  logic                   id_is_load,
    input  logic                   id_is_store,
    input  logic [2:0]             id_mem_size,
    input  logic                   id_is_amo,
    input  amo_op_t                id_amo_op,
    input  logic                   id_is_fp,
    input  logic                   id_fp_reg_write_en,
    input  logic                   id_fp_store_data_sel,
    input  fp_op_t                 id_fp_op,
    input  fp_fmt_t                id_fp_fmt,
    input  logic [2:0]             id_fp_rm,
    input  logic                   id_reg_write_en,
    input  logic [4:0]             id_rd_addr,
    input  logic [1:0]             id_wb_sel,

    // System / Exception
    input  logic                   id_is_csr,
    input  logic [2:0]             id_csr_op,
    input  logic                   id_illegal_instr,
    input  logic                   id_instr_access_fault,
    input  logic                   id_instr_page_fault,
    input  logic                   id_is_ecall,
    input  logic                   id_is_ebreak,
    input  logic                   id_is_mret,
    input  logic                   id_is_sret,
    input  logic                   id_is_wfi,
    input  logic                   id_is_sfence_vma,
    input  logic                   id_is_fence,
    input  logic                   id_is_fence_i,


    // Outputs to EX Stage

    // Instruction payload
    output logic [INST_WIDTH-1:0]  ex_instr,
    output logic [2:0]             ex_instr_len,

    // Data/Operands
    output logic [XLEN-1:0]        ex_pc,
    output logic [XLEN-1:0]        ex_rs1_rdata,
    output logic [XLEN-1:0]        ex_rs2_rdata,
    output logic [XLEN-1:0]        ex_fs1_rdata,
    output logic [XLEN-1:0]        ex_fs2_rdata,
    output logic [XLEN-1:0]        ex_fs3_rdata,
    output logic [4:0]             ex_rs1_addr,
    output logic [4:0]             ex_rs2_addr,
    output logic [4:0]             ex_fs3_addr,
    output logic [XLEN-1:0]        ex_imm,

    // Execution Control
    output alu_op_t                ex_alu_op,
    output logic                   ex_alu_src1_sel,
    output logic                   ex_alu_src2_sel,
    output logic                   ex_is_word_op,
    output logic                   ex_is_branch,
    output branch_op_t             ex_branch_op,
    output logic                   ex_is_jump,

    // Memory & Writeback Control
    output logic                   ex_is_load,
    output logic                   ex_is_store,
    output logic [2:0]             ex_mem_size,
    output logic                   ex_is_amo,
    output amo_op_t                ex_amo_op,
    output logic                   ex_is_fp,
    output logic                   ex_fp_reg_write_en,
    output logic                   ex_fp_store_data_sel,
    output fp_op_t                 ex_fp_op,
    output fp_fmt_t                ex_fp_fmt,
    output logic [2:0]             ex_fp_rm,
    output logic                   ex_reg_write_en,
    output logic [4:0]             ex_rd_addr,
    output logic [1:0]             ex_wb_sel,

    // System / Exception
    output logic                   ex_is_csr,
    output logic [2:0]             ex_csr_op,
    output logic                   ex_illegal_instr,
    output logic                   ex_instr_access_fault,
    output logic                   ex_instr_page_fault,
    output logic                   ex_is_ecall,
    output logic                   ex_is_ebreak,
    output logic                   ex_is_mret,
    output logic                   ex_is_sret,
    output logic                   ex_is_wfi,
    output logic                   ex_is_sfence_vma,
    output logic                   ex_is_fence,
    output logic                   ex_is_fence_i
);

    always_ff @(posedge clk) begin
        if (rst) begin
            ex_instr         <= '0;
            ex_instr_len     <= 3'd4;
            ex_pc            <= '0;
            ex_rs1_rdata     <= '0;
            ex_rs2_rdata     <= '0;
            ex_fs1_rdata     <= '0;
            ex_fs2_rdata     <= '0;
            ex_fs3_rdata     <= '0;
            ex_rs1_addr      <= '0;
            ex_rs2_addr      <= '0;
            ex_fs3_addr      <= '0;
            ex_imm           <= '0;
            ex_alu_op        <= ALU_ADD; // NOP is effectively ADD x0, x0, 0
            ex_alu_src1_sel  <= '0;
            ex_alu_src2_sel  <= '0;
            ex_is_word_op    <= '0;
            ex_is_branch     <= '0;
            ex_branch_op     <= BRANCH_EQ;
            ex_is_jump       <= '0;
            ex_is_load       <= '0;
            ex_is_store      <= '0;
            ex_mem_size      <= '0;
            ex_is_amo        <= '0;
            ex_amo_op        <= AMO_NONE;
            ex_is_fp         <= '0;
            ex_fp_reg_write_en <= '0;
            ex_fp_store_data_sel <= '0;
            ex_fp_op         <= FP_OP_NONE;
            ex_fp_fmt        <= FP_FMT_S;
            ex_fp_rm         <= '0;
            ex_reg_write_en  <= '0; // VERY IMPORTANT: Do not write to regfile on NOP
            ex_rd_addr       <= '0;
            ex_wb_sel        <= '0;
            ex_is_csr             <= '0;
            ex_csr_op             <= '0;
            ex_illegal_instr      <= '0;
            ex_instr_access_fault <= '0;
            ex_instr_page_fault   <= '0;
            ex_is_ecall           <= '0;
            ex_is_ebreak          <= '0;
            ex_is_mret            <= '0;
            ex_is_sret            <= '0;
            ex_is_wfi             <= '0;
            ex_is_sfence_vma      <= '0;
            ex_is_fence           <= '0;
            ex_is_fence_i         <= '0;
        end else if (flush_en) begin
            ex_instr         <= '0;
            ex_instr_len     <= 3'd4;
            ex_alu_op        <= ALU_ADD; // NOP is effectively ADD x0, x0, 0
            ex_alu_src1_sel  <= '0;
            ex_alu_src2_sel  <= '0;
            ex_is_word_op    <= '0;
            ex_is_branch     <= '0;
            ex_branch_op     <= BRANCH_EQ;
            ex_is_jump       <= '0;
            ex_is_load       <= '0;
            ex_is_store      <= '0;
            ex_mem_size      <= '0;
            ex_is_amo        <= '0;
            ex_amo_op        <= AMO_NONE;
            ex_is_fp         <= '0;
            ex_fp_reg_write_en <= '0;
            ex_fp_store_data_sel <= '0;
            ex_fp_op         <= FP_OP_NONE;
            ex_fp_fmt        <= FP_FMT_S;
            ex_fp_rm         <= '0;
            ex_reg_write_en  <= '0; // VERY IMPORTANT: Do not write to regfile on NOP
            ex_rd_addr       <= '0;
            ex_wb_sel        <= '0;
            ex_is_csr             <= '0;
            ex_csr_op             <= '0;
            ex_illegal_instr      <= '0;
            ex_instr_access_fault <= '0;
            ex_instr_page_fault   <= '0;
            ex_is_ecall           <= '0;
            ex_is_ebreak          <= '0;
            ex_is_mret            <= '0;
            ex_is_sret            <= '0;
            ex_is_wfi             <= '0;
            ex_is_sfence_vma      <= '0;
            ex_is_fence           <= '0;
            ex_is_fence_i         <= '0;
        end else if (~stall_en) begin
            ex_instr         <= id_instr;
            ex_instr_len     <= id_instr_len;
            ex_pc            <= id_pc;
            ex_rs1_rdata     <= id_rs1_rdata;
            ex_rs2_rdata     <= id_rs2_rdata;
            ex_fs1_rdata     <= id_fs1_rdata;
            ex_fs2_rdata     <= id_fs2_rdata;
            ex_fs3_rdata     <= id_fs3_rdata;
            ex_rs1_addr      <= id_rs1_addr;
            ex_rs2_addr      <= id_rs2_addr;
            ex_fs3_addr      <= id_fs3_addr;
            ex_imm           <= id_imm;
            ex_alu_op        <= id_alu_op;
            ex_alu_src1_sel  <= id_alu_src1_sel;
            ex_alu_src2_sel  <= id_alu_src2_sel;
            ex_is_word_op    <= id_is_word_op;
            ex_is_branch     <= id_is_branch;
            ex_branch_op     <= id_branch_op;
            ex_is_jump       <= id_is_jump;
            ex_is_load       <= id_is_load;
            ex_is_store      <= id_is_store;
            ex_mem_size      <= id_mem_size;
            ex_is_amo        <= id_is_amo;
            ex_amo_op        <= id_amo_op;
            ex_is_fp         <= id_is_fp;
            ex_fp_reg_write_en <= id_fp_reg_write_en;
            ex_fp_store_data_sel <= id_fp_store_data_sel;
            ex_fp_op         <= id_fp_op;
            ex_fp_fmt        <= id_fp_fmt;
            ex_fp_rm         <= id_fp_rm;
            ex_reg_write_en  <= id_reg_write_en;
            ex_rd_addr       <= id_rd_addr;
            ex_wb_sel        <= id_wb_sel;
            ex_is_csr             <= id_is_csr;
            ex_csr_op             <= id_csr_op;
            ex_illegal_instr      <= id_illegal_instr;
            ex_instr_access_fault <= id_instr_access_fault;
            ex_instr_page_fault   <= id_instr_page_fault;
            ex_is_ecall           <= id_is_ecall;
            ex_is_ebreak          <= id_is_ebreak;
            ex_is_mret            <= id_is_mret;
            ex_is_sret            <= id_is_sret;
            ex_is_wfi             <= id_is_wfi;
            ex_is_sfence_vma      <= id_is_sfence_vma;
            ex_is_fence           <= id_is_fence;
            ex_is_fence_i         <= id_is_fence_i;
        end
        // If stall_en && !flush_en, maintain current state
    end

endmodule
