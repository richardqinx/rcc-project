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

// File path: rtl/core/wb_stage.sv
// Description: Write Back Stage
// Responsible for writing back to the general-purpose register file after multiplexing the final data (ALU result, memory read data, or PC+4)

module wb_stage
    import rv64_pkg::*;
(
    // Inputs from MEM/WB pipeline register
    input  logic [XLEN-1:0]        mem_wb_alu_result,
    input  logic [XLEN-1:0]        mem_wb_mem_rdata,
    input  logic [XLEN-1:0]        mem_wb_pc,
    input  logic [XLEN-1:0]        mem_wb_pc_plus_4,
    input  logic [INST_WIDTH-1:0]  mem_wb_instr,
    
    input  logic                   mem_wb_reg_write_en,
    input  logic [4:0]             mem_wb_rd_addr,
    input  logic [1:0]             mem_wb_wb_sel,

    input  logic                   mem_wb_is_csr,
    input  logic [2:0]             mem_wb_csr_op,
    input  logic [XLEN-1:0]        mem_wb_rs1_rdata,
    input  logic [4:0]             mem_wb_rs1_addr,
    input  logic [XLEN-1:0]        mem_wb_imm,
    
    input  logic                   mem_wb_illegal_instr,
    input  logic                   mem_wb_is_ecall,
    input  logic                   mem_wb_is_ebreak,
    input  logic                   mem_wb_is_mret,
    input  logic                   mem_wb_is_sret,
    input  logic                   mem_wb_load_fault,
    input  logic                   mem_wb_store_fault,
    input  logic [XLEN-1:0]        mem_wb_fault_addr,

    // Inputs from CSR Unit
    input  logic [XLEN-1:0]        csr_rdata,         // Data read from CSR unit
    input  logic                   csr_illegal_access,// CSR rejected the request

    // Outputs to Register File
    output logic [XLEN-1:0]        wb_rd_wdata,
    output logic [4:0]             wb_rd_addr,
    output logic                   wb_reg_write_en,

    // Outputs to CSR Unit
    output logic                   csr_req,           // CSR request valid
    output logic [2:0]             csr_op,            // CSR operation type
    output logic [11:0]            csr_addr,          // CSR address
    output logic [XLEN-1:0]        csr_rs1_data,      // Source data (from rs1_rdata or zimm)

    // Outputs to Exception Handler (Trap Controller)
    output logic                   trap_illegal_instr,
    output logic                   trap_is_ecall,
    output logic                   trap_is_ebreak,
    output logic                   trap_is_mret,
    output logic                   trap_is_sret,
    output logic [XLEN-1:0]        trap_epc,
    output logic                   trap_load_fault,
    output logic                   trap_store_fault,
    output logic [XLEN-1:0]        trap_bad_addr,
    output logic [INST_WIDTH-1:0]  trap_bad_instr
);

    // --------------------------
    // Exception/Trap Detection
    // --------------------------
    logic has_trap;
    logic trap_blocker;
    assign has_trap     = mem_wb_illegal_instr | mem_wb_is_ecall | mem_wb_is_ebreak |
                          mem_wb_load_fault | mem_wb_store_fault;
    assign trap_blocker = has_trap | csr_illegal_access;

    // --------------------------
    // CSR Signal Routing
    // --------------------------
    assign csr_req  = mem_wb_is_csr & ~has_trap;
    assign csr_op   = mem_wb_csr_op;
    assign csr_addr = mem_wb_imm[11:0];
    
    // For immediate CSR instructions (CSRRWI, CSRRSI, CSRRCI), 
    // the source data is the zero-extended rs1_addr field.
    // mem_wb_csr_op[2] is 1 for immediate variants.
    assign csr_rs1_data = (mem_wb_csr_op[2]) ? {59'b0, mem_wb_rs1_addr} : mem_wb_rs1_rdata;

    // --------------------------
    // Data Multiplexer for Register File
    // --------------------------
    always_comb begin
        if (mem_wb_is_csr) begin
            // CSR reads write the previous CSR value to the destination register
            wb_rd_wdata = csr_rdata;
        end else begin
            case (mem_wb_wb_sel)
                2'b00: wb_rd_wdata = mem_wb_alu_result;
                2'b01: wb_rd_wdata = mem_wb_mem_rdata;
                2'b10: wb_rd_wdata = mem_wb_pc_plus_4;
                default: wb_rd_wdata = '0;
            endcase
        end
    end

    // --------------------------
    // Register File Write Control
    // --------------------------
    // We only write back if the instruction didn't trap.
    assign wb_rd_addr      = mem_wb_rd_addr;
    assign wb_reg_write_en = mem_wb_reg_write_en & ~trap_blocker;

    // --------------------------
    // Trap / Exception Signals
    // --------------------------
    assign trap_illegal_instr = mem_wb_illegal_instr | csr_illegal_access;
    assign trap_is_ecall      = mem_wb_is_ecall;
    assign trap_is_ebreak     = mem_wb_is_ebreak;
    assign trap_is_mret       = mem_wb_is_mret;
    assign trap_is_sret       = mem_wb_is_sret;
    assign trap_epc           = mem_wb_pc;
    assign trap_load_fault    = mem_wb_load_fault;
    assign trap_store_fault   = mem_wb_store_fault;
    assign trap_bad_addr      = mem_wb_fault_addr;
    assign trap_bad_instr     = mem_wb_instr;

endmodule
