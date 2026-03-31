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

// File path: rtl/core/if_stage.sv
// Description: Instruction Fetching Stage, includes PC registers and next-state PC calculation logic

module if_stage 
    import rv64_pkg::*;
(
    input logic                clk,
    input logic                rst,

    // Pipeline Control Signals (from Hazard Unit & Trap Controller)
    input logic                stall_en,          // Freeze PC (stall), 0: Normal operation
    input logic                pc_redirect_sel,   // Branch/Trap redirect request
    input logic [XLEN-1:0]     pc_redirect_target,// Redirect target address
    input logic                pc_redirect_is_trap, // 1: Redirect request comes from trap controller
    
    output logic [XLEN-1:0]    if_pc
);

    // PC Register
    logic [XLEN-1:0] pc_reg;

    // Combinational logic signal: the PC value to be written in the next clock cycle
    logic [XLEN-1:0] next_pc;

    always_comb begin
        next_pc = pc_reg + 64'd4;
    end

    // Sequential Logic: Update PC Register
    always_ff @(posedge clk) begin
        if (rst) begin
            pc_reg <= 64'h0000_0000_8000_0000;
        end else if (pc_redirect_sel && pc_redirect_is_trap) begin
            pc_reg <= pc_redirect_target;
        end else if (~stall_en) begin
            if (pc_redirect_sel) begin
                pc_reg <= pc_redirect_target;
            end else begin
                pc_reg <= next_pc;
            end
        end
    end
    
    // Output wiring
    assign if_pc = pc_reg;
    
endmodule
