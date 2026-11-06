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

// File path: rtl/core/branch_unit.sv
// Description: Branch Unit, responsible for determining whether conditional jump instructions are effective

module branch_unit 
    import rv64_pkg::*;
(
    input   logic              is_branch,      // Indicates whether the current instruction is a branch instruction
    input   logic [2:0]        branch_op,      // Specific branch types func3
    input   logic [XLEN-1:0]   operand_a,      // operand 1
    input   logic [XLEN-1:0]   operand_b,      // operand 2
    output  logic              branch_taken    // Branch taken signal
);

    always_comb begin
        branch_taken = 1'b0;
        
        if (is_branch) begin
            case (branch_op)
                // 3'b000: BEQ (Branch if Equal)
                BRANCH_EQ: branch_taken = (operand_a == operand_b);
                // 3'b001: BNE (Branch if Not Equal)
                BRANCH_NE: branch_taken = (operand_a != operand_b);
                // 3'b100: BLT (Branch if Less Than, Signed)
                BRANCH_LT: branch_taken = ($signed(operand_a) < $signed(operand_b));
                // 3'b101: BGE (Branch if Greater Than or Equal, Signed)
                BRANCH_GE: branch_taken = ($signed(operand_a) >= $signed(operand_b));
                // 3'b110: BLTU (Branch if Less Than, Unsigned)
                BRANCH_LTU: branch_taken = (operand_a < operand_b);
                // 3'b111: BGEU (Branch if Greater Than or Equal, Unsigned)
                BRANCH_GEU: branch_taken = (operand_a >= operand_b);
                default: branch_taken = 1'b0;
            endcase
        end
    end

endmodule
