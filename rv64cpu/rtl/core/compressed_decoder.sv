//     Richard CPU
//     Copyright (C) 2026  Richard Qin

// File path: rtl/core/compressed_decoder.sv
// Description: RV64C/Zca decompressor. Produces an equivalent 32-bit instruction
//              for the existing RV64 pipeline and flags reserved encodings.

module compressed_decoder
    import rv64_pkg::*;
(
    input  logic [15:0] compressed_instr,
    output logic [31:0] expanded_instr,
    output logic        illegal_instr
);

    logic [1:0] quadrant;
    logic [2:0] funct3;
    logic [4:0] rd_rs1;
    logic [4:0] rs2;
    logic [4:0] rd_prime;
    logic [4:0] rs1_prime;
    logic [4:0] rs2_prime;

    assign quadrant  = compressed_instr[1:0];
    assign funct3    = compressed_instr[15:13];
    assign rd_rs1    = compressed_instr[11:7];
    assign rs2       = compressed_instr[6:2];
    assign rd_prime  = {2'b01, compressed_instr[4:2]};
    assign rs1_prime = {2'b01, compressed_instr[9:7]};
    assign rs2_prime = {2'b01, compressed_instr[4:2]};

    function automatic logic [31:0] r_instr(
        input logic [6:0] funct7,
        input logic [4:0] rs2_i,
        input logic [4:0] rs1_i,
        input logic [2:0] funct3_i,
        input logic [4:0] rd_i,
        input logic [6:0] opcode_i
    );
        return {funct7, rs2_i, rs1_i, funct3_i, rd_i, opcode_i};
    endfunction

    function automatic logic [31:0] i_instr(
        input logic [11:0] imm_i,
        input logic [4:0]  rs1_i,
        input logic [2:0]  funct3_i,
        input logic [4:0]  rd_i,
        input logic [6:0]  opcode_i
    );
        return {imm_i, rs1_i, funct3_i, rd_i, opcode_i};
    endfunction

    function automatic logic [31:0] s_instr(
        input logic [11:0] imm_i,
        input logic [4:0]  rs2_i,
        input logic [4:0]  rs1_i,
        input logic [2:0]  funct3_i,
        input logic [6:0]  opcode_i
    );
        return {imm_i[11:5], rs2_i, rs1_i, funct3_i, imm_i[4:0], opcode_i};
    endfunction

    function automatic logic [31:0] b_instr(
        input logic [12:0] imm_i,
        input logic [4:0]  rs2_i,
        input logic [4:0]  rs1_i,
        input logic [2:0]  funct3_i
    );
        return {imm_i[12], imm_i[10:5], rs2_i, rs1_i, funct3_i,
                imm_i[4:1], imm_i[11], OPCODE_BRANCH};
    endfunction

    function automatic logic [31:0] u_instr(
        input logic [19:0] imm_i,
        input logic [4:0]  rd_i,
        input logic [6:0]  opcode_i
    );
        return {imm_i, rd_i, opcode_i};
    endfunction

    function automatic logic [31:0] j_instr(
        input logic [20:0] imm_i,
        input logic [4:0]  rd_i
    );
        return {imm_i[20], imm_i[10:1], imm_i[11], imm_i[19:12], rd_i, OPCODE_JAL};
    endfunction

    always_comb begin
        logic [11:0] ci_imm;
        logic [11:0] addi4spn_imm;
        logic [11:0] lw_imm;
        logic [11:0] ld_imm;
        logic [11:0] lwsp_imm;
        logic [11:0] ldsp_imm;
        logic [11:0] swsp_imm;
        logic [11:0] sdsp_imm;
        logic [12:0] branch_imm;
        logic [20:0] jump_imm;
        logic [5:0]  shamt;
        logic [19:0] lui_imm;

        ci_imm       = {{6{compressed_instr[12]}}, compressed_instr[12], compressed_instr[6:2]};
        addi4spn_imm = {2'b00, compressed_instr[10:7], compressed_instr[12:11],
                        compressed_instr[5], compressed_instr[6], 2'b00};
        lw_imm       = {5'b00000, compressed_instr[5], compressed_instr[12:10],
                        compressed_instr[6], 2'b00};
        ld_imm       = {4'b0000, compressed_instr[6:5], compressed_instr[12:10], 3'b000};
        lwsp_imm     = {4'b0000, compressed_instr[3:2], compressed_instr[12],
                        compressed_instr[6:4], 2'b00};
        ldsp_imm     = {3'b000, compressed_instr[4:2], compressed_instr[12],
                        compressed_instr[6:5], 3'b000};
        swsp_imm     = {4'b0000, compressed_instr[8:7], compressed_instr[12:9], 2'b00};
        sdsp_imm     = {3'b000, compressed_instr[9:7], compressed_instr[12:10], 3'b000};
        branch_imm   = {{4{compressed_instr[12]}}, compressed_instr[12],
                        compressed_instr[6:5], compressed_instr[2],
                        compressed_instr[11:10], compressed_instr[4:3], 1'b0};
        jump_imm     = {{9{compressed_instr[12]}}, compressed_instr[12],
                        compressed_instr[8], compressed_instr[10:9], compressed_instr[6],
                        compressed_instr[7], compressed_instr[2], compressed_instr[11],
                        compressed_instr[5:3], 1'b0};
        shamt        = {compressed_instr[12], compressed_instr[6:2]};
        lui_imm      = {{14{compressed_instr[12]}}, compressed_instr[12], compressed_instr[6:2]};

        expanded_instr = 32'h0000_0013;
        illegal_instr  = 1'b0;

        unique case (quadrant)
            2'b00: begin
                unique case (funct3)
                    3'b000: begin // C.ADDI4SPN
                        expanded_instr = i_instr(addi4spn_imm, 5'd2, 3'b000, rd_prime, OPCODE_OP_IMM);
                        illegal_instr  = (addi4spn_imm == 12'b0);
                    end
                    3'b001: expanded_instr = i_instr(ld_imm, rs1_prime, 3'b011, rd_prime, OPCODE_LOAD_FP);
                    3'b010: expanded_instr = i_instr(lw_imm, rs1_prime, 3'b010, rd_prime, OPCODE_LOAD);
                    3'b011: expanded_instr = i_instr(ld_imm, rs1_prime, 3'b011, rd_prime, OPCODE_LOAD);
                    3'b101: expanded_instr = s_instr(ld_imm, rs2_prime, rs1_prime, 3'b011, OPCODE_STORE_FP);
                    3'b110: expanded_instr = s_instr(lw_imm, rs2_prime, rs1_prime, 3'b010, OPCODE_STORE);
                    3'b111: expanded_instr = s_instr(ld_imm, rs2_prime, rs1_prime, 3'b011, OPCODE_STORE);
                    default: illegal_instr = 1'b1;
                endcase
            end

            2'b01: begin
                unique case (funct3)
                    3'b000: expanded_instr = i_instr(ci_imm, rd_rs1, 3'b000, rd_rs1, OPCODE_OP_IMM);
                    3'b001: expanded_instr = i_instr(ci_imm, rd_rs1, 3'b000, rd_rs1, OPCODE_OP_IMM_32);
                    3'b010: expanded_instr = i_instr(ci_imm, 5'd0, 3'b000, rd_rs1, OPCODE_OP_IMM);
                    3'b011: begin
                        if (rd_rs1 == 5'd2) begin
                            expanded_instr = i_instr({{2{compressed_instr[12]}}, compressed_instr[12],
                                                       compressed_instr[4:3], compressed_instr[5],
                                                       compressed_instr[2], compressed_instr[6], 4'b0000},
                                                      5'd2, 3'b000, 5'd2, OPCODE_OP_IMM);
                            illegal_instr = (compressed_instr[12:2] == 11'b0);
                        end else begin
                            expanded_instr = u_instr(lui_imm, rd_rs1, OPCODE_LUI);
                            illegal_instr = (rd_rs1 == 5'd0) || (lui_imm == 20'b0);
                        end
                    end
                    3'b100: begin
                        unique case (compressed_instr[11:10])
                            2'b00: begin
                                expanded_instr = i_instr({6'b000000, shamt}, rs1_prime, 3'b101, rs1_prime, OPCODE_OP_IMM);
                                illegal_instr = (shamt == 6'b0);
                            end
                            2'b01: begin
                                expanded_instr = i_instr({6'b010000, shamt}, rs1_prime, 3'b101, rs1_prime, OPCODE_OP_IMM);
                                illegal_instr = (shamt == 6'b0);
                            end
                            2'b10: expanded_instr = i_instr(ci_imm, rs1_prime, 3'b111, rs1_prime, OPCODE_OP_IMM);
                            2'b11: begin
                                unique case ({compressed_instr[12], compressed_instr[6:5]})
                                    3'b000: expanded_instr = r_instr(7'b0100000, rs2_prime, rs1_prime, 3'b000, rs1_prime, OPCODE_OP);
                                    3'b001: expanded_instr = r_instr(7'b0000000, rs2_prime, rs1_prime, 3'b100, rs1_prime, OPCODE_OP);
                                    3'b010: expanded_instr = r_instr(7'b0000000, rs2_prime, rs1_prime, 3'b110, rs1_prime, OPCODE_OP);
                                    3'b011: expanded_instr = r_instr(7'b0000000, rs2_prime, rs1_prime, 3'b111, rs1_prime, OPCODE_OP);
                                    3'b100: expanded_instr = r_instr(7'b0100000, rs2_prime, rs1_prime, 3'b000, rs1_prime, OPCODE_OP_32);
                                    3'b101: expanded_instr = r_instr(7'b0000000, rs2_prime, rs1_prime, 3'b000, rs1_prime, OPCODE_OP_32);
                                    default: illegal_instr = 1'b1;
                                endcase
                            end
                        endcase
                    end
                    3'b101: expanded_instr = j_instr(jump_imm, 5'd0);
                    3'b110: expanded_instr = b_instr(branch_imm, 5'd0, rs1_prime, 3'b000);
                    3'b111: expanded_instr = b_instr(branch_imm, 5'd0, rs1_prime, 3'b001);
                    default: illegal_instr = 1'b1;
                endcase
            end

            2'b10: begin
                unique case (funct3)
                    3'b000: begin
                        expanded_instr = i_instr({6'b000000, shamt}, rd_rs1, 3'b001, rd_rs1, OPCODE_OP_IMM);
                        illegal_instr = (shamt == 6'b0);
                    end
                    3'b001: begin
                        expanded_instr = i_instr(ldsp_imm, 5'd2, 3'b011, rd_rs1, OPCODE_LOAD_FP);
                    end
                    3'b010: begin
                        expanded_instr = i_instr(lwsp_imm, 5'd2, 3'b010, rd_rs1, OPCODE_LOAD);
                        illegal_instr = (rd_rs1 == 5'd0);
                    end
                    3'b011: begin
                        expanded_instr = i_instr(ldsp_imm, 5'd2, 3'b011, rd_rs1, OPCODE_LOAD);
                        illegal_instr = (rd_rs1 == 5'd0);
                    end
                    3'b100: begin
                        if (!compressed_instr[12] && (rs2 == 5'd0)) begin
                            expanded_instr = i_instr(12'b0, rd_rs1, 3'b000, 5'd0, OPCODE_JALR);
                            illegal_instr = (rd_rs1 == 5'd0);
                        end else if (!compressed_instr[12]) begin
                            expanded_instr = r_instr(7'b0000000, rs2, 5'd0, 3'b000, rd_rs1, OPCODE_OP);
                        end else if (compressed_instr[12] && (rd_rs1 == 5'd0) && (rs2 == 5'd0)) begin
                            expanded_instr = 32'h0010_0073; // EBREAK
                        end else if (compressed_instr[12] && (rs2 == 5'd0)) begin
                            expanded_instr = i_instr(12'b0, rd_rs1, 3'b000, 5'd1, OPCODE_JALR);
                        end else begin
                            expanded_instr = r_instr(7'b0000000, rs2, rd_rs1, 3'b000, rd_rs1, OPCODE_OP);
                        end
                    end
                    3'b101: expanded_instr = s_instr(sdsp_imm, rs2, 5'd2, 3'b011, OPCODE_STORE_FP);
                    3'b110: expanded_instr = s_instr(swsp_imm, rs2, 5'd2, 3'b010, OPCODE_STORE);
                    3'b111: expanded_instr = s_instr(sdsp_imm, rs2, 5'd2, 3'b011, OPCODE_STORE);
                    default: illegal_instr = 1'b1;
                endcase
            end

            default: illegal_instr = 1'b1;
        endcase
    end

endmodule
