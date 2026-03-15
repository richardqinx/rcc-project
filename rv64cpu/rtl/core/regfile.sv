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

// File path: rtl/core/regfile.sv
// Description: 64-bit Register File

module regfile
    import rv64_pkg::*;
(
    input  logic                      clk,  // Clock signal
    input  logic                      rst,  // Reset signal active high

    // Read port 1
    input  logic [4:0]                rs1_addr, // Read address 1
    output logic [XLEN-1:0]           rs1_val,  // Read data 1

    // Read port 2
    input  logic [4:0]                rs2_addr, // Read address 2
    output logic [XLEN-1:0]           rs2_val,  // Read data 2

    // Write port
    input  logic [4:0]                rd_addr,  // Write address
    input  logic [XLEN-1:0]           rd_val,   // Write data
    input  logic                      we        // Write enable signal
);

    // Register file storage
    logic [XLEN-1:0] regs [0:31];

    // Read Logic (combinational logic)
    assign rs1_val = (rs1_addr == 5'b0) ? '0 : regs[rs1_addr];
    assign rs2_val = (rs2_addr == 5'b0) ? '0 : regs[rs2_addr];

    // Write Logic (sequential logic)
    always_ff @(posedge clk) begin
        if (rst) begin
            for (int i = 0; i < 32; i++) begin
                regs[i] <= '0;
            end
        end else begin
            if (we && rd_addr != 5'b0) begin
                regs[rd_addr] <= rd_val;
            end
        end
    end

endmodule
