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

// File path: rtl/core/alu.sv
// Description: 64-bit Arithmetic Logic Unit (ALU)

module alu
    import rv64_pkg::*;
(
    input  alu_op_t          alu_op,     // ALU operation select (from decoder)
    input  logic [XLEN-1:0]  rs1_val,    // Operand 1 (register rs1 value or PC)
    input  logic [XLEN-1:0]  rs2_val,    // Operand 2 (register rs2 value or immediate)

    output logic [XLEN-1:0]  alu_res     // ALU result
);

    // Shift amount: RV64I uses lower 6 bits of rs2 (shamt[5:0])
    logic [5:0] shamt;
    assign shamt = rs2_val[5:0];

    always_comb begin
        alu_res = '0;

        case (alu_op)
            ALU_ADD:  alu_res = rs1_val + rs2_val;
            ALU_SUB:  alu_res = rs1_val - rs2_val;

            // Logical left shift: the shift amount is the lower 6 bits of rs2
            ALU_SLL:  alu_res = rs1_val << shamt;

            // Set less than (signed)
            ALU_SLT:  alu_res = {63'b0, $signed(rs1_val) < $signed(rs2_val)};

            // Set less than (unsigned)
            ALU_SLTU: alu_res = {63'b0, rs1_val < rs2_val};

            ALU_XOR:  alu_res = rs1_val ^ rs2_val;

            // Logical right shift: the shift amount is the lower 6 bits of rs2
            ALU_SRL:  alu_res = rs1_val >> shamt;

            // Arithmetic right shift: the shift amount is the lower 6 bits of rs2
            ALU_SRA:  alu_res = $unsigned($signed(rs1_val) >>> shamt);

            ALU_OR:   alu_res = rs1_val | rs2_val;
            ALU_AND:  alu_res = rs1_val & rs2_val;

            default:  alu_res = '0;
        endcase
    end

endmodule
