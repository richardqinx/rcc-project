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

// File path: rtl/core/decoder.sv
// Description: Instruction decoder for RV64IMAFD base ISA + Zicsr + Zifencei
//              Translates 32-bit instructions into control signals for the pipeline.
//              C is handled by the IF-stage decompressor before this decoder.

module decoder
    import rv64_pkg::*;
(
    input  logic [31:0]       instr,          // 32-bit instruction
    input  logic [1:0]        priv_mode,      // Current privilege mode from CSR
    input  logic              mstatus_tsr,    // Trap SRET in S-mode
    input  logic              mstatus_tw,     // Trap WFI in S/U-mode
    input  logic              mstatus_tvm,    // Trap supervisor memory-management ops in S-mode

    // Register File
    output logic [4:0]        rs1_addr,       // Source register 1 address
    output logic [4:0]        rs2_addr,       // Source register 2 address
    output logic [4:0]        rd_addr,        // Destination register address
    output logic              reg_write_en,      // Register file write enable

    // ALU Control
    output alu_op_t           alu_op,         // ALU operation select
    output logic              alu_src1_sel,   // 0 = rs1,  1 = PC
    output logic              alu_src2_sel,   // 0 = rs2,  1 = immediate

    // Immediate
    output logic [XLEN-1:0]   imm,            // Sign-extended immediate

    // Memory Access
    output logic              is_load,        // MEM stage: read from memory
    output logic              is_store,       // MEM stage: write to memory
    output logic [2:0]        mem_size,       // funct3 → byte/half/word/double + sign
    output logic              is_amo,         // A extension atomic memory operation
    output amo_op_t           amo_op,         // LR/SC/AMO operation selector
    output logic              is_fp,
    output logic              fp_reg_write_en,
    output logic              fp_store_data_sel,
    output fp_op_t            fp_op,
    output fp_fmt_t           fp_fmt,
    output logic [2:0]        fp_rm,

    // Branch / Jump
    output logic              is_branch,      // EX: conditional branch
    output logic              is_jump,        // EX: unconditional jump (JAL/JALR)
    output logic [2:0]        branch_op,      // funct3 → BEQ/BNE/BLT/BGE/BLTU/BGEU

    // Writeback Source
    output logic [1:0]        wb_sel,         // 0 = ALU result, 1 = MEM data, 2 = PC+4

    // RV64 Word Operations
    output logic              is_word_op,     // EX: operate on lower 32 bits, sext result

    // System / Privilege
    output logic              is_csr,         // CSR instruction
    output logic [2:0]        csr_op,         // funct3 → CSRRW/CSRRS/CSRRC/CSRRWI/CSRRSI/CSRRCI
    output logic              is_ecall,       // ECALL
    output logic              is_ebreak,      // EBREAK
    output logic              is_mret,        // MRET
    output logic              is_sret,        // SRET
    output logic              is_wfi,         // WFI
    output logic              is_sfence_vma,  // SFENCE.VMA
    output logic              is_fence,       // FENCE
    output logic              is_fence_i,     // FENCE.I (Zifencei)

    // Error
    output logic              illegal_instr   // Unrecognised or malformed instruction
);

    // ============================
    // Instruction Field Extraction
    // ============================
    typedef enum logic [1:0] {
        PRIV_MODE_U = 2'b00,
        PRIV_MODE_S = 2'b01,
        PRIV_MODE_M = 2'b11
    } priv_mode_t;

    logic [6:0] opcode;
    logic [2:0] funct3;
    logic [6:0] funct7;
    logic [4:0] amo_funct5;

    assign opcode   = instr[6:0];
    assign rd_addr  = instr[11:7];
    assign funct3   = instr[14:12];
    assign rs1_addr = (opcode == OPCODE_LUI) ? 5'b00000 : instr[19:15];
    assign rs2_addr = instr[24:20];
    assign funct7   = instr[31:25];
    assign amo_funct5 = instr[31:27];

    // ============================
    // Immediate Generation
    // ============================
    logic [XLEN-1:0] imm_i, imm_s, imm_b, imm_u, imm_j;
    logic [11:0]     csr_addr_field;

    // I-type:  instr[31:20]
    assign imm_i = {{52{instr[31]}}, instr[31:20]};

    // S-type:  {instr[31:25], instr[11:7]}
    assign imm_s = {{52{instr[31]}}, instr[31:25], instr[11:7]};

    // B-type:  {instr[31], instr[7], instr[30:25], instr[11:8], 1'b0}
    assign imm_b = {{51{instr[31]}}, instr[31], instr[7], instr[30:25], instr[11:8], 1'b0};

    // U-type:  {instr[31:12], 12'b0}
    assign imm_u = {{32{instr[31]}}, instr[31:12], 12'b0};

    // J-type:  {instr[31], instr[19:12], instr[20], instr[30:21], 1'b0}
    assign imm_j = {{43{instr[31]}}, instr[31], instr[19:12], instr[20], instr[30:21], 1'b0};
    assign csr_addr_field = instr[31:20];

    function automatic int priv_rank(logic [1:0] mode);
        case (mode)
            PRIV_MODE_U: return 0;
            PRIV_MODE_S: return 1;
            2'b10:       return 2; // Reserved/H-mode, treat as higher than S
            default:     return 3; // Machine mode (or anything else) highest
        endcase
    endfunction

    function automatic logic priv_ge(logic [1:0] cur, logic [1:0] req);
        return priv_rank(cur) >= priv_rank(req);
    endfunction

    function automatic logic csr_access_allowed(logic [1:0] cur, logic [11:0] addr);
        logic [1:0] req_priv;
        req_priv = addr[9:8];
        return priv_ge(cur, req_priv);
    endfunction

    // ============================
    // Main Decode Logic
    // ============================
    always_comb begin
        // Default values (all signals inactive)
        alu_op        = ALU_ADD;
        alu_src1_sel  = 1'b0;   // rs1
        alu_src2_sel  = 1'b0;   // rs2
        reg_write_en  = 1'b0;
        imm           = '0;
        is_load       = 1'b0;
        is_store      = 1'b0;
        mem_size      = 3'b0;
        is_amo        = 1'b0;
        amo_op        = AMO_NONE;
        is_fp         = 1'b0;
        fp_reg_write_en  = 1'b0;
        fp_store_data_sel = 1'b0;
        fp_op         = FP_OP_NONE;
        fp_fmt        = FP_FMT_S;
        fp_rm         = funct3;
        is_branch     = 1'b0;
        is_jump       = 1'b0;
        branch_op     = 3'b0;
        wb_sel        = 2'b00;  // ALU result
        is_word_op    = 1'b0;
        is_csr        = 1'b0;
        csr_op        = 3'b0;
        is_ecall      = 1'b0;
        is_ebreak     = 1'b0;
        is_mret       = 1'b0;
        is_sret       = 1'b0;
        is_wfi        = 1'b0;
        is_sfence_vma = 1'b0;
        is_fence      = 1'b0;
        is_fence_i    = 1'b0;
        illegal_instr = 1'b0;

        case (opcode)

            // LUI — rd = imm_u

            OPCODE_LUI: begin
                reg_write_en = 1'b1;
                alu_src1_sel = 1'b0;    // doesn't matter,  ALU_ADD(0, imm_u)
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_u;
                alu_op       = ALU_ADD; // result = 0 + imm_u  (rs1 forced to x0 externally or use alu_src1_sel trick)
                // NOTE: EX stage should feed 0 to ALU operand 1 when alu_src1_sel==0 and opcode==LUI.
                // Alternative: add a dedicated alu_src1_sel=2 for "zero". For now, the pipeline
                // can detect LUI by opcode and zero out the operand.
            end

            // AUIPC — rd = PC + imm_u


            OPCODE_AUIPC: begin
                reg_write_en = 1'b1;
                alu_src1_sel = 1'b1;    // PC
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_u;
                alu_op       = ALU_ADD; // result = PC + imm_u
            end

            // -----------------
            // JAL — rd = PC+4;  PC = PC + imm_j
            OPCODE_JAL: begin
                reg_write_en = 1'b1;
                is_jump      = 1'b1;
                alu_src1_sel = 1'b1;    // PC
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_j;
                alu_op       = ALU_ADD; // target = PC + imm_j
                wb_sel       = 2'b10;   // writeback PC+4
            end

            // ------------------
            // JALR — rd = PC+4;  PC = (rs1 + imm_i) & ~1

            OPCODE_JALR: begin
                reg_write_en = 1'b1;
                is_jump      = 1'b1;
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_i;
                alu_op       = ALU_ADD; // target = rs1 + imm_i  (LSB cleared by EX stage)
                wb_sel       = 2'b10;   // writeback PC+4
            end

            // -----------------
            // Branch — compare rs1, rs2;  if taken, PC = PC + imm_b
            OPCODE_BRANCH: begin
                reg_write_en = 1'b0;    // branches never write rd
                is_branch    = 1'b1;
                alu_src1_sel = 1'b1;    // PC  (ALU computes branch target)
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_b;
                alu_op       = ALU_ADD; // branch target = PC + imm_b
                branch_op    = funct3;  // BEQ/BNE/BLT/BGE/BLTU/BGEU
            end

            // -----------------
            // Load — rd = Mem[rs1 + imm_i]
            OPCODE_LOAD: begin
                reg_write_en = 1'b1;
                is_load      = 1'b1;
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_i;
                alu_op       = ALU_ADD; // effective address = rs1 + imm_i
                mem_size     = funct3;  // LB/LH/LW/LD/LBU/LHU/LWU
                wb_sel       = 2'b01;   // writeback from memory
            end

            // -----------------
            // Store — Mem[rs1 + imm_s] = rs2
            OPCODE_STORE: begin
                reg_write_en = 1'b0;
                is_store     = 1'b1;
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b1;    // immediate  (NOT rs2 — rs2 is store data)
                imm          = imm_s;
                alu_op       = ALU_ADD; // effective address = rs1 + imm_s
                mem_size     = funct3;  // SB/SH/SW/SD
            end

            // -----------------
            // OP-IMM (I-type ALU) — rd = rs1 OP imm
            // ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI
            OPCODE_OP_IMM: begin
                reg_write_en = 1'b1;
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_i;

                case (funct3)
                    3'b000:  alu_op = ALU_ADD;                              // ADDI
                    3'b010:  alu_op = ALU_SLT;                              // SLTI
                    3'b011:  alu_op = ALU_SLTU;                             // SLTIU
                    3'b100:  alu_op = ALU_XOR;                              // XORI
                    3'b110:  alu_op = ALU_OR;                               // ORI
                    3'b111:  alu_op = ALU_AND;                              // ANDI
                    3'b001:  alu_op = ALU_SLL;                              // SLLI
                    3'b101:  alu_op = (funct7[5]) ? ALU_SRA : ALU_SRL;      // SRAI / SRLI
                    default: alu_op = ALU_ADD;
                endcase
            end

            // -----------------
            // OP-IMM-32 (I-type ALU, 32-bit word variants for RV64)
            // ADDIW, SLLIW, SRLIW, SRAIW
            OPCODE_OP_IMM_32: begin
                reg_write_en = 1'b1;
                is_word_op   = 1'b1;    // EX stage: operate on lower 32 bits, sext result
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b1;    // immediate
                imm          = imm_i;

                case (funct3)
                    3'b000:  alu_op = ALU_ADD;                              // ADDIW
                    3'b001:  alu_op = ALU_SLL;                              // SLLIW
                    3'b101:  alu_op = (funct7[5]) ? ALU_SRA : ALU_SRL;      // SRAIW / SRLIW
                    default: begin
                        alu_op = ALU_ADD;
                        illegal_instr = 1'b1;
                    end
                endcase
            end

            // -----------------
            // OP (R-type ALU) — rd = rs1 OP rs2
            // ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND
            OPCODE_OP: begin
                reg_write_en = 1'b1;
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b0;    // rs2

                if (funct7 == 7'b0000001) begin
                    case (funct3)
                        3'b000:  alu_op = ALU_MUL;
                        3'b001:  alu_op = ALU_MULH;
                        3'b010:  alu_op = ALU_MULHSU;
                        3'b011:  alu_op = ALU_MULHU;
                        3'b100:  alu_op = ALU_DIV;
                        3'b101:  alu_op = ALU_DIVU;
                        3'b110:  alu_op = ALU_REM;
                        3'b111:  alu_op = ALU_REMU;
                        default: illegal_instr = 1'b1;
                    endcase
                end else begin
                    case (funct3)
                        3'b000:  alu_op = (funct7[5]) ? ALU_SUB : ALU_ADD;  // SUB / ADD
                        3'b001:  alu_op = ALU_SLL;                          // SLL
                        3'b010:  alu_op = ALU_SLT;                          // SLT
                        3'b011:  alu_op = ALU_SLTU;                         // SLTU
                        3'b100:  alu_op = ALU_XOR;                          // XOR
                        3'b101:  alu_op = (funct7[5]) ? ALU_SRA : ALU_SRL;  // SRA / SRL
                        3'b110:  alu_op = ALU_OR;                           // OR
                        3'b111:  alu_op = ALU_AND;                          // AND
                        default: alu_op = ALU_ADD;
                    endcase
                end
            end

            // -----------------
            // OP-32 (R-type ALU, 32-bit word variants for RV64)
            // ADDW, SUBW, SLLW, SRLW, SRAW
            OPCODE_OP_32: begin
                reg_write_en = 1'b1;
                is_word_op   = 1'b1;
                alu_src1_sel = 1'b0;    // rs1
                alu_src2_sel = 1'b0;    // rs2

                if (funct7 == 7'b0000001) begin
                    case (funct3)
                        3'b000:  alu_op = ALU_MUL;
                        3'b100:  alu_op = ALU_DIV;
                        3'b101:  alu_op = ALU_DIVU;
                        3'b110:  alu_op = ALU_REM;
                        3'b111:  alu_op = ALU_REMU;
                        default: illegal_instr = 1'b1;
                    endcase
                end else begin
                    case (funct3)
                        3'b000:  alu_op = (funct7[5]) ? ALU_SUB : ALU_ADD;  // SUBW / ADDW
                        3'b001:  alu_op = ALU_SLL;                          // SLLW
                        3'b101:  alu_op = (funct7[5]) ? ALU_SRA : ALU_SRL;  // SRAW / SRLW
                        default: begin
                            alu_op = ALU_ADD;
                            illegal_instr = 1'b1;
                        end
                    endcase
                end
            end

            // -----------------
            // MISC-MEM — FENCE / FENCE.I
            OPCODE_MISC_MEM: begin
                case (funct3)
                    3'b000:  is_fence   = 1'b1;  // FENCE  (NOP in single-core in-order)
                    3'b001:  is_fence_i = 1'b1;  // FENCE.I — flush I-Cache
                    default: illegal_instr = 1'b1;
                endcase
            end

            // -----------------
            // SYSTEM — ECALL / EBREAK / xRET / WFI / SFENCE.VMA / CSR*
            OPCODE_SYSTEM: begin
                if (funct3 == 3'b000) begin
                    // Privileged instructions (non-CSR)
                    reg_write_en = 1'b0;
                    case ({funct7, instr[24:20]})    // {funct7, rs2}
                        12'b0000000_00000: is_ecall      = 1'b1;   // ECALL
                        12'b0000000_00001: is_ebreak     = 1'b1;   // EBREAK
                        12'b0011000_00010: begin                  // MRET
                            if (priv_mode == PRIV_MODE_M)
                                is_mret = 1'b1;
                            else
                                illegal_instr = 1'b1;
                        end
                        12'b0001000_00010: begin                  // SRET
                            if (!priv_ge(priv_mode, PRIV_MODE_S))
                                illegal_instr = 1'b1;
                            else if ((priv_mode == PRIV_MODE_S) && mstatus_tsr)
                                illegal_instr = 1'b1;
                            else
                                is_sret = 1'b1;
                        end
                        12'b0001000_00101: begin                  // WFI
                            if ((priv_mode != PRIV_MODE_M) && mstatus_tw)
                                illegal_instr = 1'b1;
                            else
                                is_wfi = 1'b1;
                        end
                        default: begin
                            if (funct7 == 7'b0001001) begin
                                if (!priv_ge(priv_mode, PRIV_MODE_S))
                                    illegal_instr = 1'b1;
                                else if ((priv_mode == PRIV_MODE_S) && mstatus_tvm)
                                    illegal_instr = 1'b1;
                                else
                                    is_sfence_vma = 1'b1;           // SFENCE.VMA
                            end else begin
                                illegal_instr = 1'b1;
                            end
                        end
                    endcase
                end else begin
                    // CSR instructions
                    // funct3: 001=CSRRW, 010=CSRRS, 011=CSRRC,
                    //         101=CSRRWI, 110=CSRRSI, 111=CSRRCI
                    reg_write_en = 1'b1;
                    is_csr       = 1'b1;
                    csr_op       = funct3;
                    imm          = imm_i;   // CSR address = instr[31:20] (reuse imm_i path)
                    if (!csr_access_allowed(priv_mode, csr_addr_field)) begin
                        reg_write_en  = 1'b0;
                        is_csr        = 1'b0;
                        illegal_instr = 1'b1;
                    end
                end
            end

            // -----------------
            // AMO — A extension (LR/SC/AMO*)
            OPCODE_AMO: begin
                reg_write_en = 1'b1;
                is_amo       = 1'b1;
                alu_src1_sel = 1'b0;
                alu_src2_sel = 1'b1;
                imm          = '0;
                alu_op       = ALU_ADD;
                wb_sel       = 2'b01;

                unique case (funct3)
                    3'b010:  mem_size = 3'b010; // AMO*.W
                    3'b011:  mem_size = 3'b011; // AMO*.D
                    default: illegal_instr = 1'b1;
                endcase

                unique case (amo_funct5)
                    5'b00010: begin
                        amo_op = AMO_LR;
                        if (rs2_addr != 5'd0) begin
                            illegal_instr = 1'b1;
                        end
                    end
                    5'b00011: amo_op = AMO_SC;
                    5'b00001: amo_op = AMO_SWAP;
                    5'b00000: amo_op = AMO_ADD;
                    5'b00100: amo_op = AMO_XOR;
                    5'b01100: amo_op = AMO_AND;
                    5'b01000: amo_op = AMO_OR;
                    5'b10000: amo_op = AMO_MIN;
                    5'b10100: amo_op = AMO_MAX;
                    5'b11000: amo_op = AMO_MINU;
                    5'b11100: amo_op = AMO_MAXU;
                    default: begin
                        amo_op        = AMO_NONE;
                        illegal_instr = 1'b1;
                    end
                endcase

                if (illegal_instr) begin
                    reg_write_en = 1'b0;
                    is_amo       = 1'b0;
                    amo_op       = AMO_NONE;
                end
            end

            // -----------------
            // Floating-Point — F/D extensions
            OPCODE_LOAD_FP: begin
                is_fp           = 1'b1;
                is_load         = 1'b1;
                fp_reg_write_en = 1'b1;
                alu_src1_sel    = 1'b0;
                alu_src2_sel    = 1'b1;
                imm             = imm_i;
                alu_op          = ALU_ADD;
                wb_sel          = 2'b01;
                unique case (funct3)
                    3'b010: begin
                        mem_size = 3'b010; // FLW
                        fp_fmt   = FP_FMT_S;
                    end
                    3'b011: begin
                        mem_size = 3'b011; // FLD
                        fp_fmt   = FP_FMT_D;
                    end
                    default: begin
                        is_fp           = 1'b0;
                        fp_reg_write_en = 1'b0;
                        illegal_instr   = 1'b1;
                    end
                endcase
            end

            OPCODE_STORE_FP: begin
                is_fp             = 1'b1;
                is_store          = 1'b1;
                fp_store_data_sel = 1'b1;
                alu_src1_sel      = 1'b0;
                alu_src2_sel      = 1'b1;
                imm               = imm_s;
                alu_op            = ALU_ADD;
                unique case (funct3)
                    3'b010: begin
                        mem_size = 3'b010; // FSW
                        fp_fmt   = FP_FMT_S;
                    end
                    3'b011: begin
                        mem_size = 3'b011; // FSD
                        fp_fmt   = FP_FMT_D;
                    end
                    default: begin
                        is_fp             = 1'b0;
                        fp_store_data_sel = 1'b0;
                        illegal_instr     = 1'b1;
                    end
                endcase
            end

            OPCODE_MADD,
            OPCODE_MSUB,
            OPCODE_NMSUB,
            OPCODE_NMADD: begin
                is_fp           = 1'b1;
                fp_reg_write_en = 1'b1;
                fp_rm           = funct3;
                unique case (instr[26:25])
                    2'b00: fp_fmt = FP_FMT_S;
                    2'b01: fp_fmt = FP_FMT_D;
                    default: illegal_instr = 1'b1;
                endcase
                unique case (opcode)
                    OPCODE_MADD:  fp_op = FP_OP_MADD;
                    OPCODE_MSUB:  fp_op = FP_OP_MSUB;
                    OPCODE_NMSUB: fp_op = FP_OP_NMSUB;
                    default:      fp_op = FP_OP_NMADD;
                endcase
                if (illegal_instr) begin
                    is_fp           = 1'b0;
                    fp_reg_write_en = 1'b0;
                    fp_op           = FP_OP_NONE;
                end
            end

            OPCODE_OP_FP: begin
                is_fp = 1'b1;
                fp_rm = funct3;
                unique case (instr[26:25])
                    2'b00: fp_fmt = FP_FMT_S;
                    2'b01: fp_fmt = FP_FMT_D;
                    default: illegal_instr = 1'b1;
                endcase

                unique case (funct7)
                    7'b0000000, 7'b0000001: begin fp_op = FP_OP_ADD; fp_reg_write_en = 1'b1; end
                    7'b0000100, 7'b0000101: begin fp_op = FP_OP_SUB; fp_reg_write_en = 1'b1; end
                    7'b0001000, 7'b0001001: begin fp_op = FP_OP_MUL; fp_reg_write_en = 1'b1; end
                    7'b0001100, 7'b0001101: begin fp_op = FP_OP_DIV; fp_reg_write_en = 1'b1; end
                    7'b0101100, 7'b0101101: begin
                        fp_op = FP_OP_SQRT;
                        fp_reg_write_en = 1'b1;
                        if (rs2_addr != 5'd0) illegal_instr = 1'b1;
                    end
                    7'b0010000, 7'b0010001: begin
                        fp_reg_write_en = 1'b1;
                        unique case (funct3)
                            3'b000: fp_op = FP_OP_SGNJ;
                            3'b001: fp_op = FP_OP_SGNJN;
                            3'b010: fp_op = FP_OP_SGNJX;
                            default: illegal_instr = 1'b1;
                        endcase
                    end
                    7'b0010100, 7'b0010101: begin
                        fp_reg_write_en = 1'b1;
                        unique case (funct3)
                            3'b000: fp_op = FP_OP_MIN;
                            3'b001: fp_op = FP_OP_MAX;
                            default: illegal_instr = 1'b1;
                        endcase
                    end
                    7'b0100000, 7'b0100001: begin
                        fp_op = FP_OP_CVT_F2F;
                        fp_reg_write_en = 1'b1;
                    end
                    7'b1010000, 7'b1010001: begin
                        reg_write_en = 1'b1;
                        unique case (funct3)
                            3'b010: fp_op = FP_OP_EQ;
                            3'b001: fp_op = FP_OP_LT;
                            3'b000: fp_op = FP_OP_LE;
                            default: illegal_instr = 1'b1;
                        endcase
                    end
                    7'b1100000, 7'b1100001: begin
                        fp_op = FP_OP_CVT_F2I;
                        reg_write_en = 1'b1;
                    end
                    7'b1101000, 7'b1101001: begin
                        fp_op = FP_OP_CVT_I2F;
                        fp_reg_write_en = 1'b1;
                    end
                    7'b1110000, 7'b1110001: begin
                        if (rs2_addr == 5'd0 && funct3 == 3'b000) begin
                            fp_op = FP_OP_MV_F2X;
                            reg_write_en = 1'b1;
                        end else if (rs2_addr == 5'd0 && funct3 == 3'b001) begin
                            fp_op = FP_OP_CLASS;
                            reg_write_en = 1'b1;
                        end else begin
                            illegal_instr = 1'b1;
                        end
                    end
                    7'b1111000, 7'b1111001: begin
                        if (rs2_addr == 5'd0 && funct3 == 3'b000) begin
                            fp_op = FP_OP_MV_X2F;
                            fp_reg_write_en = 1'b1;
                        end else begin
                            illegal_instr = 1'b1;
                        end
                    end
                    default: illegal_instr = 1'b1;
                endcase

                if (illegal_instr) begin
                    is_fp           = 1'b0;
                    fp_reg_write_en = 1'b0;
                    reg_write_en    = 1'b0;
                    fp_op           = FP_OP_NONE;
                end
            end

            // -----------------
            // Unrecognised opcode
            default: begin
                illegal_instr = 1'b1;
            end

        endcase
    end

endmodule
