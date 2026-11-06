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

// File path: rtl/core/mem_wb_reg.sv
// Description: MEM/WB pipeline stage register (Memory Access to Write Back)
//              Passes the final computation results, memory read data, and write-back control signals to the write-back stage

module mem_wb_reg
    import rv64_pkg::*;
(
    input  logic               clk,
    input  logic               rst,

    // -----------------------
    // Control Signals from Hazard Unit
    input  logic               stall_en,
    input  logic               flush_en,

    // -----------------------
    // Inputs from MEM stage
    // -----------------------
    // Data and addresses
    input  logic [XLEN-1:0]        mem_pc,
    input  logic [XLEN-1:0]        mem_alu_result,
    input  logic [XLEN-1:0]        mem_mem_rdata,
    input  logic [XLEN-1:0]        mem_fp_result,
    input  logic [4:0]             mem_fp_fflags,
    input  logic [INST_WIDTH-1:0]  mem_instr,
    input  logic [2:0]             mem_instr_len,

    // Write back control
    input  logic                   mem_reg_write_en,
    input  logic                   mem_fp_reg_write_en,
    input  logic [4:0]             mem_rd_addr,
    input  logic [1:0]             mem_wb_sel,

    // CSR & System
    input  logic                   mem_is_csr,
    input  logic [2:0]             mem_csr_op,
    input  logic [XLEN-1:0]        mem_rs1_rdata,
    input  logic [4:0]             mem_rs1_addr,
    input  logic [XLEN-1:0]        mem_imm,
    input  logic                   mem_illegal_instr,
    input  logic                   mem_instr_access_fault,
    input  logic                   mem_instr_page_fault,
    input  logic                   mem_is_ecall,
    input  logic                   mem_is_ebreak,
    input  logic                   mem_is_mret,
    input  logic                   mem_is_sret,
    input  logic                   mem_load_fault,
    input  logic                   mem_store_fault,
    input  logic                   mem_load_page_fault,
    input  logic                   mem_store_page_fault,
    input  logic [XLEN-1:0]        mem_fault_addr,

    // -----------------------
    // Outputs to WB Stage
    // -----------------------
    // Data and address
    output logic [XLEN-1:0]        wb_pc,
    output logic [XLEN-1:0]        wb_alu_result,
    output logic [XLEN-1:0]        wb_mem_rdata,
    output logic [XLEN-1:0]        wb_fp_result,
    output logic [4:0]             wb_fp_fflags,
    output logic [INST_WIDTH-1:0]  wb_instr,
    output logic [2:0]             wb_instr_len,

    // Write back control
    output logic                   wb_reg_write_en,
    output logic                   wb_fp_reg_write_en,
    output logic [4:0]             wb_rd_addr,
    output logic [1:0]             wb_wb_sel,

    // CSR & System
    output logic                   wb_is_csr,
    output logic [2:0]             wb_csr_op,
    output logic [XLEN-1:0]        wb_rs1_rdata,
    output logic [4:0]             wb_rs1_addr,
    output logic [XLEN-1:0]        wb_imm,
    output logic                   wb_illegal_instr,
    output logic                   wb_instr_access_fault,
    output logic                   wb_instr_page_fault,
    output logic                   wb_is_ecall,
    output logic                   wb_is_ebreak,
    output logic                   wb_is_mret,
    output logic                   wb_is_sret,
    output logic                   wb_load_fault,
    output logic                   wb_store_fault,
    output logic                   wb_load_page_fault,
    output logic                   wb_store_page_fault,
    output logic [XLEN-1:0]        wb_fault_addr

);
    // Sequential Logic: State Update
    always_ff @(posedge clk) begin
        if (rst) begin
            wb_pc                  <= '0;
            wb_alu_result          <= '0;
            wb_mem_rdata           <= '0;
            wb_fp_result           <= '0;
            wb_fp_fflags           <= '0;
            wb_instr               <= '0;
            wb_instr_len           <= 3'd4;
            wb_reg_write_en        <= '0;
            wb_fp_reg_write_en     <= '0;
            wb_rd_addr             <= '0;
            wb_wb_sel              <= '0;
            wb_is_csr              <= '0;
            wb_csr_op              <= '0;
            wb_rs1_rdata           <= '0;
            wb_rs1_addr            <= '0;
            wb_imm                 <= '0;
            wb_illegal_instr       <= '0;
            wb_instr_access_fault <= '0;
            wb_instr_page_fault   <= '0;
            wb_is_ecall           <= '0;
            wb_is_ebreak          <= '0;
            wb_is_mret            <= '0;
            wb_is_sret            <= '0;
            wb_load_fault         <= '0;
            wb_store_fault        <= '0;
            wb_load_page_fault    <= '0;
            wb_store_page_fault   <= '0;
            wb_fault_addr         <= '0;
        end else if (flush_en) begin
            // Target specific control signals to clear. Leave datapath elements intact
            // to avoid extreme power consumption from massive flip-flop state changes array.
            wb_reg_write_en       <= '0;
            wb_fp_reg_write_en    <= '0;
            wb_is_csr             <= '0;
            wb_illegal_instr      <= '0;
            wb_instr_access_fault <= '0;
            wb_instr_page_fault   <= '0;
            wb_is_ecall           <= '0;
            wb_is_ebreak          <= '0;
            wb_is_mret            <= '0;
            wb_is_sret            <= '0;
            wb_load_fault         <= '0;
            wb_store_fault        <= '0;
            wb_load_page_fault    <= '0;
            wb_store_page_fault   <= '0;
            wb_fault_addr         <= '0;
        end else if (!stall_en) begin
            wb_pc                  <= mem_pc;
            wb_alu_result          <= mem_alu_result;
            wb_mem_rdata           <= mem_mem_rdata;
            wb_fp_result           <= mem_fp_result;
            wb_fp_fflags           <= mem_fp_fflags;
            wb_instr               <= mem_instr;
            wb_instr_len           <= mem_instr_len;
            wb_reg_write_en        <= mem_reg_write_en;
            wb_fp_reg_write_en     <= mem_fp_reg_write_en;
            wb_rd_addr             <= mem_rd_addr;
            wb_wb_sel              <= mem_wb_sel;
            wb_is_csr              <= mem_is_csr;
            wb_csr_op              <= mem_csr_op;
            wb_rs1_rdata           <= mem_rs1_rdata;
            wb_rs1_addr            <= mem_rs1_addr;
            wb_imm                 <= mem_imm;
            wb_illegal_instr       <= mem_illegal_instr;
            wb_instr_access_fault <= mem_instr_access_fault;
            wb_instr_page_fault   <= mem_instr_page_fault;
            wb_is_ecall           <= mem_is_ecall;
            wb_is_ebreak          <= mem_is_ebreak;
            wb_is_mret            <= mem_is_mret;
            wb_is_sret            <= mem_is_sret;
            wb_load_fault         <= mem_load_fault;
            wb_store_fault        <= mem_store_fault;
            wb_load_page_fault    <= mem_load_page_fault;
            wb_store_page_fault   <= mem_store_page_fault;
            wb_fault_addr         <= mem_fault_addr;
        end
    end

endmodule
