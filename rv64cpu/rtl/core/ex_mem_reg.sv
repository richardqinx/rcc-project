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
    input  logic [4:0]             ex_rs1_addr,   // Serves as zimm for CSRRWI/SI/CI
    input  logic [XLEN-1:0]        ex_pc,         // PC passed along for exceptions/CSRs
    input  logic [XLEN-1:0]        ex_imm,        // Imm passed for CSR writes

    // Memory Control
    input  logic                   ex_is_load,
    input  logic                   ex_is_store,
    input  logic [2:0]             ex_mem_size,

    // Writeback Control
    input  logic                   ex_reg_write_en,
    input  logic [4:0]             ex_rd_addr,
    input  logic [1:0]             ex_wb_sel,

    // System / Exception Control
    input  logic                   ex_is_csr,
    input  logic [2:0]             ex_csr_op,
    input  logic                   ex_illegal_instr,
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
    output logic [4:0]             mem_rs1_addr,
    output logic [XLEN-1:0]        mem_pc,
    output logic [XLEN-1:0]        mem_imm,

    // Memory Control
    output logic                   mem_is_load,
    output logic                   mem_is_store,
    output logic [2:0]             mem_mem_size,

    // Writeback Control
    output logic                   mem_reg_write_en,
    output logic [4:0]             mem_rd_addr,
    output logic [1:0]             mem_wb_sel,

    // System / Exception Control
    output logic                   mem_is_csr,
    output logic [2:0]             mem_csr_op,
    output logic                   mem_illegal_instr,
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
            mem_alu_result    <= '0;
            mem_branch_taken  <= '0;
            mem_branch_target <= '0;
            mem_rs1_rdata     <= '0;
            mem_rs2_rdata     <= '0;
            mem_rs1_addr      <= '0;
            mem_pc            <= '0;
            mem_imm           <= '0;
            
            // Critical control signals must be zeroed out
            mem_is_load       <= '0;
            mem_is_store      <= '0;
            mem_mem_size      <= '0;
            mem_reg_write_en  <= '0;
            mem_rd_addr       <= '0;
            mem_wb_sel        <= '0;

            // Exceptions and system control
            mem_is_csr        <= '0;
            mem_csr_op        <= '0;
            mem_illegal_instr <= '0;
            mem_is_ecall      <= '0;
            mem_is_ebreak     <= '0;
            mem_is_mret       <= '0;
            mem_is_sret       <= '0;
            mem_is_wfi        <= '0;
            mem_is_sfence_vma <= '0;
            mem_is_fence      <= '0;
            mem_is_fence_i    <= '0;

        end else if (flush_en) begin
            // Critical control signals must be zeroed out
            mem_branch_taken  <= '0;
            mem_is_load       <= '0;
            mem_is_store      <= '0;
            mem_mem_size      <= '0;
            mem_reg_write_en  <= '0;
            mem_rd_addr       <= '0;
            mem_wb_sel        <= '0;

            // Exceptions and system control
            mem_is_csr        <= '0;
            mem_csr_op        <= '0;
            mem_illegal_instr <= '0;
            mem_is_ecall      <= '0;
            mem_is_ebreak     <= '0;
            mem_is_mret       <= '0;
            mem_is_sret       <= '0;
            mem_is_wfi        <= '0;
            mem_is_sfence_vma <= '0;
            mem_is_fence      <= '0;
            mem_is_fence_i    <= '0;
        end else if (~stall_en) begin
             mem_alu_result    <= ex_alu_result;
             mem_branch_taken  <= ex_branch_taken;
             mem_branch_target <= ex_branch_target;
             mem_rs1_rdata     <= ex_rs1_rdata;
             mem_rs2_rdata     <= ex_rs2_rdata;
             mem_rs1_addr      <= ex_rs1_addr;
             mem_pc            <= ex_pc;
             mem_imm           <= ex_imm;
             
             mem_is_load       <= ex_is_load;
             mem_is_store      <= ex_is_store;
             mem_mem_size      <= ex_mem_size;
             mem_reg_write_en  <= ex_reg_write_en;
             mem_rd_addr       <= ex_rd_addr;
             mem_wb_sel        <= ex_wb_sel;
 
             mem_is_csr        <= ex_is_csr;
             mem_csr_op        <= ex_csr_op;
             mem_illegal_instr <= ex_illegal_instr;
             mem_is_ecall      <= ex_is_ecall;
             mem_is_ebreak     <= ex_is_ebreak;
             mem_is_mret       <= ex_is_mret;
             mem_is_sret       <= ex_is_sret;
             mem_is_wfi        <= ex_is_wfi;
             mem_is_sfence_vma <= ex_is_sfence_vma;
             mem_is_fence      <= ex_is_fence;
             mem_is_fence_i    <= ex_is_fence_i;
        end
        // If stall_en && !flush_en, maintain current state
    end

endmodule
