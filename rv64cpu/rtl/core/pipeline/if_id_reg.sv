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

// File path: rtl/core/if_id_reg.sv
// Description: IF/ID Pipeline Register
//              Latches PC and instruction from IF stage and passes them to ID stage.
//              Supports stall (freeze) and flush (insert NOP).

module if_id_reg
    import rv64_pkg::*;
(
    input  logic                   clk,
    input  logic                   rst,

    // Pipeline Control from Hazard Unit
    input  logic                   stall_en,   // 1: Freeze pipeline register state
    input  logic                   flush_en,   // 1: Clear register (insert NOP)

    // Inputs from IF Stage
    input  logic [XLEN-1:0]        if_pc,      // PC from IF stage
    input  logic [INST_WIDTH-1:0]  if_instr,   // Fetched instruction from IF stage
    input  logic [2:0]             if_instr_len,
    input  logic                   if_instr_access_fault,
    input  logic                   if_instr_page_fault,

    // Outputs to ID Stage
    output logic [XLEN-1:0]        id_pc,      // Passed to ID stage
    output logic [INST_WIDTH-1:0]  id_instr,   // Passed to ID stage
    output logic [2:0]             id_instr_len,
    output logic                   id_instr_access_fault,
    output logic                   id_instr_page_fault
);

    localparam logic [31:0] NOP_INSTR = 32'h0000_0013;

    always_ff @(posedge clk) begin
        if (rst) begin
            id_pc                  <= '0;
            id_instr               <= NOP_INSTR;
            id_instr_len           <= 3'd4;
            id_instr_access_fault <= 1'b0;
            id_instr_page_fault   <= 1'b0;
        end else if (flush_en) begin
            id_pc                  <= '0;
            id_instr               <= NOP_INSTR;
            id_instr_len           <= 3'd4;
            id_instr_access_fault <= 1'b0;
            id_instr_page_fault   <= 1'b0;
        end else if (~stall_en) begin
            id_pc                  <= if_pc;
            id_instr               <= if_instr;
            id_instr_len           <= if_instr_len;
            id_instr_access_fault <= if_instr_access_fault;
            id_instr_page_fault   <= if_instr_page_fault;
        end
        // If stall_en && !flush_en, maintain current state
    end

endmodule
