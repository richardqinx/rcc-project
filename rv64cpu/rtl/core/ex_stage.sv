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

// File path: rtl/core/ex_stage.sv
// Description: Execute Stage
//              Performs ALU operations, branch/jump calculations, and generates memory addresses.

module ex_stage
    import rv64_pkg::*;
(
    // Input from ID/EX level pipeline register
    input  logic [XLEN-1:0]    ex_pc,
    input  logic [XLEN-1:0]    ex_rs1_rdata,
    input  logic [XLEN-1:0]    ex_rs2_rdata,
    input  logic [XLEN-1:0]    ex_imm,

    // Execution Control Signals
    input  alu_op_t            ex_alu_op,
    input  logic               ex_alu_src1_sel,    // 0: rs1_rdata, 1: PC
    input  logic               ex_alu_src2_sel,    // 0: rs2_rdata, 1: imm
    input  logic               ex_is_branch,
    input  branch_op_t         ex_branch_op,
    input  logic               ex_is_jump,
    input  logic               ex_is_word_op,

    // Output to EX/MEM pipeline register & hazard unit
    output logic [XLEN-1:0]    ex_alu_result,
    output logic               ex_branch_taken,
    output logic [XLEN-1:0]    ex_branch_target
);

    logic [XLEN-1:0]    alu_mux_a;
    logic [XLEN-1:0]    alu_mux_b;
    logic [XLEN-1:0]    alu_operand_a;
    logic [XLEN-1:0]    alu_operand_b;
    logic [XLEN-1:0]    alu_raw_result;
    logic               branch_unit_taken;

    // -------------
    // ALU Input Multiplexers (MUX)
    assign alu_mux_a = (ex_alu_src1_sel == 1'b1) ? ex_pc : ex_rs1_rdata;
    assign alu_mux_b = (ex_alu_src2_sel == 1'b1) ? ex_imm : ex_rs2_rdata;
    
    // -------------
    // RV64 Word Operation (32-bit) Pre-conditioning
    // RV64 requires word operations (ADDW/SLLW/SRLW/SRAW) to operate on the 
    // lower 32 bits and sign-extend the result.
    always_comb begin
        if (ex_is_word_op) begin
            // Shift operations require specific 32-bit extension forms
            if (ex_alu_op == ALU_SRA)
                alu_operand_a = { {32{alu_mux_a[31]}}, alu_mux_a[31:0] }; // Sign extend for arithmetic shift
            else if (ex_alu_op == ALU_SRL)
                alu_operand_a = { 32'b0, alu_mux_a[31:0] };              // Zero extend for logical shift
            else
                alu_operand_a = alu_mux_a;                               // ADDW/SUBW/SLLW don't care about upper bits
            
            // RV32 shift amount is strictly 5 bits (0-31), ignoring bit 5 which might be set in 64-bit registers
            if (ex_alu_op == ALU_SLL || ex_alu_op == ALU_SRL || ex_alu_op == ALU_SRA)
                alu_operand_b = { 59'b0, alu_mux_b[4:0] };
            else
                alu_operand_b = alu_mux_b;
        end else begin
            // 64-bit operations pass through unchanged
            alu_operand_a = alu_mux_a;
            alu_operand_b = alu_mux_b;
        end
    end

    // --------------
    // Instantiate the core computing engine ALU
    alu alu_inst (
        .alu_op         (ex_alu_op),
        .operand_a      (alu_operand_a),
        .operand_b      (alu_operand_b),
        .alu_result     (alu_raw_result)
    );

    // -------------
    // Branch Unit
    branch_unit bu_inst (
        .is_branch    (ex_is_branch),
        .branch_op    (ex_branch_op),
        .operand_a    (ex_rs1_rdata),
        .operand_b    (ex_rs2_rdata),
        .branch_taken (branch_unit_taken)
    );

    // ------------- 
    // 32-bit instruction result truncation and sign extension
    assign ex_alu_result = ex_is_word_op ? 
                           { {32{alu_raw_result[31]}}, alu_raw_result[31:0] } : 
                           alu_raw_result;

    // ------------- 
    // Branch Target Calculation
    assign ex_branch_target = alu_raw_result & ~64'b1;

    assign ex_branch_taken  = ex_is_jump | (ex_is_branch & branch_unit_taken);

endmodule
