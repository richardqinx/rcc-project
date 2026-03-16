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
    input  logic [XLEN-1:0]  operand_a,    // Operand 1 (register rs1 value or PC)
    input  logic [XLEN-1:0]  operand_b,    // Operand 2 (register rs2 value or immediate)

    output logic [XLEN-1:0]  alu_result     // ALU result
);

    // Shift amount: RV64I uses lower 6 bits of rs2 (shamt[5:0])
    logic [5:0] shamt;
    assign shamt = operand_b[5:0];

    always_comb begin
        alu_result = '0;

        case (alu_op)
            ALU_ADD:  alu_result = operand_a + operand_b;
            ALU_SUB:  alu_result = operand_a - operand_b;

            // Logical left shift: the shift amount is the lower 6 bits of rs2
            ALU_SLL:  alu_result = operand_a << shamt;

            // Set less than (signed)
            ALU_SLT:  alu_result = {63'b0, $signed(operand_a) < $signed(operand_b)};

            // Set less than (unsigned)
            ALU_SLTU: alu_result = {63'b0, operand_a < operand_b};

            ALU_XOR:  alu_result = operand_a ^ operand_b;

            // Logical right shift: the shift amount is the lower 6 bits of rs2
            ALU_SRL:  alu_result = operand_a >> shamt;

            // Arithmetic right shift: the shift amount is the lower 6 bits of rs2
            ALU_SRA:  alu_result = $unsigned($signed(operand_a) >>> shamt);

            ALU_OR:   alu_result = operand_a | operand_b;
            ALU_AND:  alu_result = operand_a & operand_b;

            default:  alu_result = '0;
        endcase
    end

endmodule
