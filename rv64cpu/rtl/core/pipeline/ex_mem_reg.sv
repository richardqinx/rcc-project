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

// File path: rtl/core/ex_mem_reg.sv
// Description: Execute to Memory Pipeline Register
//              Latches results from the EX stage and bypassed control signals from ID/EX.
//              Supports stall and flush operations.

module ex_mem_reg
    import rv64_pkg::*;
(
    input  logic                   clk,
    input  logic                   rst,

    // Pipeline Control
    input  logic                   stall_en,
    input  logic                   flush_en,

    // ----------------------
    // Inputs from EX Stage (computed results)
    input  logic [XLEN-1:0]        ex_alu_result,
    input  logic                   ex_branch_taken,
    input  logic [XLEN-1:0]        ex_branch_target,

    // ----------------------
    // Bypassed Inputs from ID/EX Register

    // Data/Operands
    input  logic [XLEN-1:0]        ex_rs1_rdata,  // Needed for CSRRW/RS/RC
    input  logic [XLEN-1:0]        ex_rs2_rdata,  // Needed for store data
    input  logic [XLEN-1:0]        ex_fs1_rdata,
    input  logic [XLEN-1:0]        ex_fs2_rdata,
    input  logic [XLEN-1:0]        ex_fs3_rdata,
    input  logic [4:0]             ex_rs1_addr,   // Serves as zimm for CSRRWI/SI/CI
    input  logic [4:0]             ex_fs3_addr,
    input  logic [XLEN-1:0]        ex_pc,         // PC passed along for exceptions/CSRs
    input  logic [XLEN-1:0]        ex_imm,        // Imm passed for CSR writes
    input  logic [INST_WIDTH-1:0]  ex_instr,
    input  logic [2:0]             ex_instr_len,

    // Memory Control
    input  logic                   ex_is_load,
    input  logic                   ex_is_store,
    input  logic [2:0]             ex_mem_size,
    input  logic                   ex_is_amo,
    input  amo_op_t                ex_amo_op,
    input  logic                   ex_is_fp,
    input  logic                   ex_fp_reg_write_en,
    input  logic                   ex_fp_store_data_sel,
    input  fp_op_t                 ex_fp_op,
    input  fp_fmt_t                ex_fp_fmt,
    input  logic [2:0]             ex_fp_rm,
    input  logic [XLEN-1:0]        ex_fp_result,
    input  logic [4:0]             ex_fp_fflags,

    // Writeback Control
    input  logic                   ex_reg_write_en,
    input  logic [4:0]             ex_rd_addr,
    input  logic [1:0]             ex_wb_sel,

    // System / Exception Control
    input  logic                   ex_is_csr,
    input  logic [2:0]             ex_csr_op,
    input  logic                   ex_illegal_instr,
    input  logic                   ex_instr_access_fault,
    input  logic                   ex_instr_page_fault,
    input  logic                   ex_is_ecall,
    input  logic                   ex_is_ebreak,
    input  logic                   ex_is_mret,
    input  logic                   ex_is_sret,
    input  logic                   ex_is_wfi,
    input  logic                   ex_is_sfence_vma,
    input  logic                   ex_is_fence,
    input  logic                   ex_is_fence_i,


    // ----------------------
    // Outputs to MEM Stage

    // Data/Operands
    output logic [XLEN-1:0]        mem_alu_result,
    output logic                   mem_branch_taken,
    output logic [XLEN-1:0]        mem_branch_target,
    output logic [XLEN-1:0]        mem_rs1_rdata,
    output logic [XLEN-1:0]        mem_rs2_rdata,
    output logic [XLEN-1:0]        mem_fs1_rdata,
    output logic [XLEN-1:0]        mem_fs2_rdata,
    output logic [XLEN-1:0]        mem_fs3_rdata,
    output logic [4:0]             mem_rs1_addr,
    output logic [4:0]             mem_fs3_addr,
    output logic [XLEN-1:0]        mem_pc,
    output logic [XLEN-1:0]        mem_imm,
    output logic [INST_WIDTH-1:0]  mem_instr,
    output logic [2:0]             mem_instr_len,

    // Memory Control
    output logic                   mem_is_load,
    output logic                   mem_is_store,
    output logic [2:0]             mem_mem_size,
    output logic                   mem_is_amo,
    output amo_op_t                mem_amo_op,
    output logic                   mem_is_fp,
    output logic                   mem_fp_reg_write_en,
    output logic                   mem_fp_store_data_sel,
    output fp_op_t                 mem_fp_op,
    output fp_fmt_t                mem_fp_fmt,
    output logic [2:0]             mem_fp_rm,
    output logic [XLEN-1:0]        mem_fp_result,
    output logic [4:0]             mem_fp_fflags,

    // Writeback Control
    output logic                   mem_reg_write_en,
    output logic [4:0]             mem_rd_addr,
    output logic [1:0]             mem_wb_sel,

    // System / Exception Control
    output logic                   mem_is_csr,
    output logic [2:0]             mem_csr_op,
    output logic                   mem_illegal_instr,
    output logic                   mem_instr_access_fault,
    output logic                   mem_instr_page_fault,
    output logic                   mem_is_ecall,
    output logic                   mem_is_ebreak,
    output logic                   mem_is_mret,
    output logic                   mem_is_sret,
    output logic                   mem_is_wfi,
    output logic                   mem_is_sfence_vma,
    output logic                   mem_is_fence,
    output logic                   mem_is_fence_i
);

    always_ff @(posedge clk) begin
        if (rst) begin
            mem_instr        <= '0;
            mem_instr_len      <= 3'd4;
            mem_alu_result    <= '0;
            mem_branch_taken  <= '0;
            mem_branch_target <= '0;
            mem_rs1_rdata     <= '0;
            mem_rs2_rdata     <= '0;
            mem_fs1_rdata     <= '0;
            mem_fs2_rdata     <= '0;
            mem_fs3_rdata     <= '0;
            mem_rs1_addr      <= '0;
            mem_fs3_addr      <= '0;
            mem_pc            <= '0;
            mem_imm           <= '0;

            // Critical control signals must be zeroed out
            mem_is_load       <= '0;
            mem_is_store      <= '0;
            mem_mem_size      <= '0;
            mem_is_amo        <= '0;
            mem_amo_op        <= AMO_NONE;
            mem_is_fp         <= '0;
            mem_fp_reg_write_en <= '0;
            mem_fp_store_data_sel <= '0;
            mem_fp_op         <= FP_OP_NONE;
            mem_fp_fmt        <= FP_FMT_S;
            mem_fp_rm         <= '0;
            mem_fp_result     <= '0;
            mem_fp_fflags     <= '0;
            mem_reg_write_en  <= '0;
            mem_rd_addr       <= '0;
            mem_wb_sel        <= '0;

            // Exceptions and system control
            mem_is_csr             <= '0;
            mem_csr_op             <= '0;
            mem_illegal_instr      <= '0;
            mem_instr_access_fault <= '0;
            mem_instr_page_fault   <= '0;
            mem_is_ecall           <= '0;
            mem_is_ebreak          <= '0;
            mem_is_mret            <= '0;
            mem_is_sret            <= '0;
            mem_is_wfi             <= '0;
            mem_is_sfence_vma      <= '0;
            mem_is_fence           <= '0;
            mem_is_fence_i         <= '0;

        end else if (flush_en) begin
            mem_instr        <= '0;
            mem_instr_len      <= 3'd4;
            // Critical control signals must be zeroed out
            mem_branch_taken  <= '0;
            mem_is_load       <= '0;
            mem_is_store      <= '0;
            mem_mem_size      <= '0;
            mem_is_amo        <= '0;
            mem_amo_op        <= AMO_NONE;
            mem_is_fp         <= '0;
            mem_fp_reg_write_en <= '0;
            mem_fp_store_data_sel <= '0;
            mem_fp_op         <= FP_OP_NONE;
            mem_fp_fmt        <= FP_FMT_S;
            mem_fp_rm         <= '0;
            mem_fp_result     <= '0;
            mem_fp_fflags     <= '0;
            mem_reg_write_en  <= '0;
            mem_rd_addr       <= '0;
            mem_wb_sel        <= '0;

            // Exceptions and system control
            mem_is_csr             <= '0;
            mem_csr_op             <= '0;
            mem_illegal_instr      <= '0;
            mem_instr_access_fault <= '0;
            mem_instr_page_fault   <= '0;
            mem_is_ecall           <= '0;
            mem_is_ebreak          <= '0;
            mem_is_mret            <= '0;
            mem_is_sret            <= '0;
            mem_is_wfi             <= '0;
            mem_is_sfence_vma      <= '0;
            mem_is_fence           <= '0;
            mem_is_fence_i         <= '0;
        end else if (~stall_en) begin
            mem_alu_result         <= ex_alu_result;
            mem_branch_taken       <= ex_branch_taken;
            mem_branch_target      <= ex_branch_target;
            mem_rs1_rdata          <= ex_rs1_rdata;
            mem_rs2_rdata          <= ex_rs2_rdata;
            mem_fs1_rdata          <= ex_fs1_rdata;
            mem_fs2_rdata          <= ex_fs2_rdata;
            mem_fs3_rdata          <= ex_fs3_rdata;
            mem_rs1_addr           <= ex_rs1_addr;
            mem_fs3_addr           <= ex_fs3_addr;
            mem_pc                 <= ex_pc;
            mem_imm                <= ex_imm;
            mem_instr              <= ex_instr;
            mem_instr_len          <= ex_instr_len;

            mem_is_load            <= ex_is_load;
            mem_is_store           <= ex_is_store;
            mem_mem_size           <= ex_mem_size;
            mem_is_amo             <= ex_is_amo;
            mem_amo_op             <= ex_amo_op;
            mem_is_fp              <= ex_is_fp;
            mem_fp_reg_write_en    <= ex_fp_reg_write_en;
            mem_fp_store_data_sel  <= ex_fp_store_data_sel;
            mem_fp_op              <= ex_fp_op;
            mem_fp_fmt             <= ex_fp_fmt;
            mem_fp_rm              <= ex_fp_rm;
            mem_fp_result          <= ex_fp_result;
            mem_fp_fflags          <= ex_fp_fflags;
            mem_reg_write_en       <= ex_reg_write_en;
            mem_rd_addr            <= ex_rd_addr;
            mem_wb_sel             <= ex_wb_sel;

            mem_is_csr             <= ex_is_csr;
            mem_csr_op             <= ex_csr_op;
            mem_illegal_instr      <= ex_illegal_instr;
            mem_instr_access_fault <= ex_instr_access_fault;
            mem_instr_page_fault   <= ex_instr_page_fault;
            mem_is_ecall           <= ex_is_ecall;
            mem_is_ebreak          <= ex_is_ebreak;
            mem_is_mret            <= ex_is_mret;
            mem_is_sret            <= ex_is_sret;
            mem_is_wfi             <= ex_is_wfi;
            mem_is_sfence_vma      <= ex_is_sfence_vma;
            mem_is_fence           <= ex_is_fence;
            mem_is_fence_i         <= ex_is_fence_i;
        end
        // If stall_en && !flush_en, maintain current state
    end

endmodule
