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

// File path: rtl/core/richard_core.sv
// Description: Richard CPU core top-level wrapper (Core Top)
// Assembles a 5-stage pipeline, 4 inter-stage registers, and Hazard Unit

module richard_core
    import rv64_pkg::*;
(
    input  logic        clk,
    input  logic        rst,

    // Interface 1: Instruction Memory (I-Mem)
    output logic [XLEN-1:0] imem_addr,
    input  logic [31:0]     imem_rdata,

    // Interface 2: Data Memory (D-Mem)
    output logic        dmem_read_en,
    output logic        dmem_write_en,
    output logic [63:0] dmem_addr,
    output logic [63:0] dmem_wdata,
    output logic [2:0]  dmem_size,
    input  logic [63:0] dmem_rdata
);
    // -----------------------
    // Internal Wire Declarations
    // -----------------------

    // (1) Hazard Unit Controls
    logic [1:0]        hu_forward_a_sel, hu_forward_b_sel;
    logic              hu_if_stall_en, hu_if_id_stall_en, hu_id_ex_stall_en, hu_ex_mem_stall_en;
    logic              hu_if_id_flush_en, hu_id_ex_flush_en;

    // (2) IF Stage Outputs
    logic [XLEN-1:0]   if_pc_out;
    // imem interface (ports)

    // (3) IF/ID Pipeline Register Outputs
    logic [XLEN-1:0]   if_id_pc;
    logic [31:0]       if_id_instr;

    // (4) ID Stage Outputs
    logic [XLEN-1:0]   id_pc_out;
    logic [XLEN-1:0]   id_rs1_rdata, id_rs2_rdata;
    logic [4:0]        id_rs1_addr, id_rs2_addr;
    logic [XLEN-1:0]   id_imm;
    alu_op_t           id_alu_op;
    logic              id_alu_src1_sel, id_alu_src2_sel;
    logic              id_is_word_op, id_is_branch, id_is_jump;
    branch_op_t        id_branch_op;
    logic              id_is_load, id_is_store;
    logic [2:0]        id_mem_size;
    logic              id_reg_write_en;
    logic [4:0]        id_rd_addr;
    logic [1:0]        id_wb_sel;
    logic              id_is_csr;
    logic [2:0]        id_csr_op;
    logic              id_illegal_instr, id_is_ecall, id_is_ebreak, id_is_mret, id_is_sret;
    logic              id_is_wfi, id_is_sfence_vma, id_is_fence, id_is_fence_i;

    // (5) ID/EX Pipeline Register Outputs
    logic [XLEN-1:0]   ex_pc_in;
    logic [XLEN-1:0]   ex_rs1_rdata_in, ex_rs2_rdata_in;
    logic [4:0]        ex_rs1_addr_in, ex_rs2_addr_in;
    logic [XLEN-1:0]   ex_imm_in;
    alu_op_t           ex_alu_op_in;
    logic              ex_alu_src1_sel_in, ex_alu_src2_sel_in;
    logic              ex_is_word_op_in, ex_is_branch_in, ex_is_jump_in;
    branch_op_t        ex_branch_op_in;
    logic              ex_is_load_in, ex_is_store_in;
    logic [2:0]        ex_mem_size_in;
    logic              ex_reg_write_en_in;
    logic [4:0]        ex_rd_addr_in;
    logic [1:0]        ex_wb_sel_in;
    logic              ex_is_csr_in;
    logic [2:0]        ex_csr_op_in;
    logic              ex_illegal_instr_in, ex_is_ecall_in, ex_is_ebreak_in, ex_is_mret_in, ex_is_sret_in;
    logic              ex_is_wfi_in, ex_is_sfence_vma_in, ex_is_fence_in, ex_is_fence_i_in;

    // (6) EX Stage Outputs (and Forwarding Muxes)
    logic [XLEN-1:0]   ex_rs1_rdata_fwd, ex_rs2_rdata_fwd;
    logic [XLEN-1:0]   ex_alu_result_out;
    logic              ex_branch_taken_out;
    logic [XLEN-1:0]   ex_branch_target_out;

    // (7) EX/MEM Pipeline Register Outputs
    logic [XLEN-1:0]   mem_pc_in;
    logic [XLEN-1:0]   mem_alu_result_in;
    logic              mem_branch_taken_in;
    logic [XLEN-1:0]   mem_branch_target_in;
    logic [XLEN-1:0]   mem_rs1_rdata_in, mem_rs2_rdata_in;
    logic [4:0]        mem_rs1_addr_in;
    logic [XLEN-1:0]   mem_imm_in;
    logic              mem_is_load_in, mem_is_store_in;
    logic [2:0]        mem_mem_size_in;
    logic              mem_reg_write_en_in;
    logic [4:0]        mem_rd_addr_in;
    logic [1:0]        mem_wb_sel_in;
    logic              mem_is_csr_in;
    logic [2:0]        mem_csr_op_in;
    logic              mem_illegal_instr_in, mem_is_ecall_in, mem_is_ebreak_in, mem_is_mret_in, mem_is_sret_in;
    logic              mem_is_wfi_in, mem_is_sfence_vma_in, mem_is_fence_in, mem_is_fence_i_in;

    // (8) MEM Stage Outputs (to MEM/WB Register)
    logic [XLEN-1:0]   mw_reg_alu_result_in;
    logic [XLEN-1:0]   mw_reg_mem_rdata_in;
    logic [XLEN-1:0]   mw_reg_pc_in;
    logic [7:0]        mw_reg_dmem_wstrb_in;
    logic              mw_reg_reg_write_en_in;
    logic [4:0]        mw_reg_rd_addr_in;
    logic [1:0]        mw_reg_wb_sel_in;
    logic              mw_reg_is_csr_in;
    logic [2:0]        mw_reg_csr_op_in;
    logic [XLEN-1:0]   mw_reg_rs1_rdata_in;
    logic [4:0]        mw_reg_rs1_addr_in;
    logic [XLEN-1:0]   mw_reg_imm_in;
    logic              mw_reg_illegal_instr_in, mw_reg_is_ecall_in, mw_reg_is_ebreak_in, mw_reg_is_mret_in, mw_reg_is_sret_in;
    logic              mem_stall_req;

    // (9) MEM/WB Pipeline Register Outputs
    logic [XLEN-1:0]   wb_pc_in;
    logic [XLEN-1:0]   wb_alu_result_in;
    logic [XLEN-1:0]   wb_mem_rdata_in;
    logic              wb_reg_write_en_in;
    logic [4:0]        wb_rd_addr_in;
    logic [1:0]        wb_wb_sel_in;
    logic              wb_is_csr_in;
    logic [2:0]        wb_csr_op_in;
    logic [XLEN-1:0]   wb_rs1_rdata_in;
    logic [4:0]        wb_rs1_addr_in;
    logic [XLEN-1:0]   wb_imm_in;
    logic              wb_illegal_instr_in, wb_is_ecall_in, wb_is_ebreak_in, wb_is_mret_in, wb_is_sret_in;

    // (10) WB Stage Outputs (to RegFile & CSR Unit)
    logic [XLEN-1:0]   wb_rd_wdata;
    logic [4:0]        wb_rd_addr;
    logic              wb_reg_write_en_out;
    logic              csr_req;
    logic [2:0]        csr_op;
    logic [11:0]       csr_addr;
    logic [XLEN-1:0]   csr_rs1_data;
    logic [XLEN-1:0]   csr_rdata; // Result from CSR unit
    logic [1:0]        csr_priv_mode_cur;
    logic [XLEN-1:0]   csr_mtvec_base;
    logic [XLEN-1:0]   csr_stvec_base;
    logic [XLEN-1:0]   csr_mstatus_value;
    logic [XLEN-1:0]   csr_sstatus_value;
    logic [XLEN-1:0]   csr_medeleg_value;
    logic [XLEN-1:0]   csr_mideleg_value;
    logic [XLEN-1:0]   csr_mepc_value;
    logic [XLEN-1:0]   csr_sepc_value;
    logic [XLEN-1:0]   csr_satp_value;
    logic [XLEN-1:0]   csr_trap_vector_value;
    logic              csr_trap_to_s_mode_flag;
    logic              csr_illegal_access_flag;

    // (11) Exception / Trap bus
    logic              trap_illegal_instr, trap_is_ecall, trap_is_ebreak, trap_is_mret, trap_is_sret;
    logic [XLEN-1:0]   trap_epc;

    // ---------------------------------------------------------
    // (A) Hazard Unit Implementation
    // ---------------------------------------------------------
    hazard_unit u_hazard_unit (
        .id_rs1_addr        (id_rs1_addr),
        .id_rs2_addr        (id_rs2_addr),
        .ex_rs1_addr        (ex_rs1_addr_in),
        .ex_rs2_addr        (ex_rs2_addr_in),
        .ex_rd_addr         (ex_rd_addr_in),
        .ex_reg_write_en    (ex_reg_write_en_in),
        .ex_is_load         (ex_is_load_in),
        .ex_branch_taken    (ex_branch_taken_out),
        .mem_rd_addr        (mem_rd_addr_in),
        .mem_reg_write_en   (mem_reg_write_en_in),
        .mem_stall_req      (mem_stall_req),
        .wb_rd_addr         (wb_rd_addr_in),
        .wb_reg_write_en    (wb_reg_write_en_out),
        .forward_a_sel      (hu_forward_a_sel),
        .forward_b_sel      (hu_forward_b_sel),
        .if_stall_en        (hu_if_stall_en),
        .if_id_stall_en     (hu_if_id_stall_en),
        .id_ex_stall_en     (hu_id_ex_stall_en),
        .ex_mem_stall_en    (hu_ex_mem_stall_en),
        .if_id_flush_en     (hu_if_id_flush_en),
        .id_ex_flush_en     (hu_id_ex_flush_en)
    );

    // ---------------------------------------------------------
    // (B) IF Stage - Instruction Fetch
    // ---------------------------------------------------------
    if_stage u_if_stage (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_if_stall_en),
        .ex_branch_taken    (ex_branch_taken_out),
        .ex_branch_target   (ex_branch_target_out),
        .if_pc              (if_pc_out)
    );

    assign imem_addr = if_pc_out;

    if_id_reg u_if_id_reg (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_if_id_stall_en),
        .flush_en           (hu_if_id_flush_en),
        .if_pc              (if_pc_out),
        .if_instr           (imem_rdata),
        .id_pc              (if_id_pc),
        .id_instr           (if_id_instr)
    );

    // ---------------------------------------------------------
    // (C) ID Stage - Instruction Decode & RegFile
    // ---------------------------------------------------------
    id_stage u_id_stage (
        .clk                (clk),
        .rst                (rst),
        .if_pc              (if_id_pc),
        .if_instr           (if_id_instr),
        .wb_reg_write_en    (wb_reg_write_en_out),
        .wb_rd_addr         (wb_rd_addr),
        .wb_rd_wdata        (wb_rd_wdata),
        .id_pc              (id_pc_out),
        .id_rs1_rdata       (id_rs1_rdata),
        .id_rs2_rdata       (id_rs2_rdata),
        .id_rs1_addr        (id_rs1_addr),
        .id_rs2_addr        (id_rs2_addr),
        .id_imm             (id_imm),
        .id_alu_op          (id_alu_op),
        .id_alu_src1_sel    (id_alu_src1_sel),
        .id_alu_src2_sel    (id_alu_src2_sel),
        .id_is_word_op      (id_is_word_op),
        .id_is_branch       (id_is_branch),
        .id_branch_op       (id_branch_op),
        .id_is_jump         (id_is_jump),
        .id_is_load         (id_is_load),
        .id_is_store        (id_is_store),
        .id_mem_size        (id_mem_size),
        .id_reg_write_en    (id_reg_write_en),
        .id_rd_addr         (id_rd_addr),
        .id_wb_sel          (id_wb_sel),
        .id_is_csr          (id_is_csr),
        .id_csr_op          (id_csr_op),
        .id_illegal_instr   (id_illegal_instr),
        .id_is_ecall        (id_is_ecall),
        .id_is_ebreak       (id_is_ebreak),
        .id_is_mret         (id_is_mret),
        .id_is_sret         (id_is_sret),
        .id_is_wfi          (id_is_wfi),
        .id_is_sfence_vma   (id_is_sfence_vma),
        .id_is_fence        (id_is_fence),
        .id_is_fence_i      (id_is_fence_i)
    );

    id_ex_reg u_id_ex_reg (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_id_ex_stall_en),
        .flush_en           (hu_id_ex_flush_en),
        .id_pc              (id_pc_out),
        .id_rs1_rdata       (id_rs1_rdata),
        .id_rs2_rdata       (id_rs2_rdata),
        .id_rs1_addr        (id_rs1_addr),
        .id_rs2_addr        (id_rs2_addr),
        .id_imm             (id_imm),
        .id_alu_op          (id_alu_op),
        .id_alu_src1_sel    (id_alu_src1_sel),
        .id_alu_src2_sel    (id_alu_src2_sel),
        .id_is_word_op      (id_is_word_op),
        .id_is_branch       (id_is_branch),
        .id_branch_op       (id_branch_op),
        .id_is_jump         (id_is_jump),
        .id_is_load         (id_is_load),
        .id_is_store        (id_is_store),
        .id_mem_size        (id_mem_size),
        .id_reg_write_en    (id_reg_write_en),
        .id_rd_addr         (id_rd_addr),
        .id_wb_sel          (id_wb_sel),
        .id_is_csr          (id_is_csr),
        .id_csr_op          (id_csr_op),
        .id_illegal_instr   (id_illegal_instr),
        .id_is_ecall        (id_is_ecall),
        .id_is_ebreak       (id_is_ebreak),
        .id_is_mret         (id_is_mret),
        .id_is_sret         (id_is_sret),
        .id_is_wfi          (id_is_wfi),
        .id_is_sfence_vma   (id_is_sfence_vma),
        .id_is_fence        (id_is_fence),
        .id_is_fence_i      (id_is_fence_i),
        .ex_pc              (ex_pc_in),
        .ex_rs1_rdata       (ex_rs1_rdata_in),
        .ex_rs2_rdata       (ex_rs2_rdata_in),
        .ex_rs1_addr        (ex_rs1_addr_in),
        .ex_rs2_addr        (ex_rs2_addr_in),
        .ex_imm             (ex_imm_in),
        .ex_alu_op          (ex_alu_op_in),
        .ex_alu_src1_sel    (ex_alu_src1_sel_in),
        .ex_alu_src2_sel    (ex_alu_src2_sel_in),
        .ex_is_word_op      (ex_is_word_op_in),
        .ex_is_branch       (ex_is_branch_in),
        .ex_branch_op       (ex_branch_op_in),
        .ex_is_jump         (ex_is_jump_in),
        .ex_is_load         (ex_is_load_in),
        .ex_is_store        (ex_is_store_in),
        .ex_mem_size        (ex_mem_size_in),
        .ex_reg_write_en    (ex_reg_write_en_in),
        .ex_rd_addr         (ex_rd_addr_in),
        .ex_wb_sel          (ex_wb_sel_in),
        .ex_is_csr          (ex_is_csr_in),
        .ex_csr_op          (ex_csr_op_in),
        .ex_illegal_instr   (ex_illegal_instr_in),
        .ex_is_ecall        (ex_is_ecall_in),
        .ex_is_ebreak       (ex_is_ebreak_in),
        .ex_is_mret         (ex_is_mret_in),
        .ex_is_sret         (ex_is_sret_in),
        .ex_is_wfi          (ex_is_wfi_in),
        .ex_is_sfence_vma   (ex_is_sfence_vma_in),
        .ex_is_fence        (ex_is_fence_in),
        .ex_is_fence_i      (ex_is_fence_i_in)
    );

    // ---------------------------------------------------------
    // (D) EX Stage - Execution & Forwarding
    // ---------------------------------------------------------
    
    // Operand A Forwarding Mux
    always_comb begin
        case (hu_forward_a_sel)
            2'b01:   ex_rs1_rdata_fwd = mem_alu_result_in; // From MEM
            2'b10:   ex_rs1_rdata_fwd = wb_rd_wdata;       // From WB
            default: ex_rs1_rdata_fwd = ex_rs1_rdata_in;   // From RegFile
        endcase
    end

    // Operand B Forwarding Mux
    always_comb begin
        case (hu_forward_b_sel)
            2'b01:   ex_rs2_rdata_fwd = mem_alu_result_in; // From MEM
            2'b10:   ex_rs2_rdata_fwd = wb_rd_wdata;       // From WB
            default: ex_rs2_rdata_fwd = ex_rs2_rdata_in;   // From RegFile
        endcase
    end

    ex_stage u_ex_stage (
        .ex_pc              (ex_pc_in),
        .ex_rs1_rdata       (ex_rs1_rdata_fwd),
        .ex_rs2_rdata       (ex_rs2_rdata_fwd),
        .ex_imm             (ex_imm_in),
        .ex_alu_op          (ex_alu_op_in),
        .ex_alu_src1_sel    (ex_alu_src1_sel_in),
        .ex_alu_src2_sel    (ex_alu_src2_sel_in),
        .ex_is_branch       (ex_is_branch_in),
        .ex_branch_op       (ex_branch_op_in),
        .ex_is_jump         (ex_is_jump_in),
        .ex_is_word_op      (ex_is_word_op_in),
        .ex_alu_result      (ex_alu_result_out),
        .ex_branch_taken    (ex_branch_taken_out),
        .ex_branch_target   (ex_branch_target_out)
    );

    ex_mem_reg u_ex_mem_reg (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_ex_mem_stall_en),
        .flush_en           (1'b0),
        .ex_alu_result      (ex_alu_result_out),
        .ex_branch_taken    (ex_branch_taken_out),
        .ex_branch_target   (ex_branch_target_out),
        .ex_rs1_rdata       (ex_rs1_rdata_fwd),
        .ex_rs2_rdata       (ex_rs2_rdata_fwd),
        .ex_rs1_addr        (ex_rs1_addr_in),
        .ex_pc              (ex_pc_in),
        .ex_imm             (ex_imm_in),
        .ex_is_load         (ex_is_load_in),
        .ex_is_store        (ex_is_store_in),
        .ex_mem_size        (ex_mem_size_in),
        .ex_reg_write_en    (ex_reg_write_en_in),
        .ex_rd_addr         (ex_rd_addr_in),
        .ex_wb_sel          (ex_wb_sel_in),
        .ex_is_csr          (ex_is_csr_in),
        .ex_csr_op          (ex_csr_op_in),
        .ex_illegal_instr   (ex_illegal_instr_in),
        .ex_is_ecall        (ex_is_ecall_in),
        .ex_is_ebreak       (ex_is_ebreak_in),
        .ex_is_mret         (ex_is_mret_in),
        .ex_is_sret         (ex_is_sret_in),
        .ex_is_wfi          (ex_is_wfi_in),
        .ex_is_sfence_vma   (ex_is_sfence_vma_in),
        .ex_is_fence        (ex_is_fence_in),
        .ex_is_fence_i      (ex_is_fence_i_in),
        .mem_alu_result      (mem_alu_result_in),
        .mem_branch_taken    (mem_branch_taken_in),
        .mem_branch_target   (mem_branch_target_in),
        .mem_rs1_rdata       (mem_rs1_rdata_in),
        .mem_rs2_rdata       (mem_rs2_rdata_in),
        .mem_rs1_addr        (mem_rs1_addr_in),
        .mem_pc              (mem_pc_in),
        .mem_imm             (mem_imm_in),
        .mem_is_load         (mem_is_load_in),
        .mem_is_store        (mem_is_store_in),
        .mem_mem_size        (mem_mem_size_in),
        .mem_reg_write_en    (mem_reg_write_en_in),
        .mem_rd_addr         (mem_rd_addr_in),
        .mem_wb_sel          (mem_wb_sel_in),
        .mem_is_csr          (mem_is_csr_in),
        .mem_csr_op          (mem_csr_op_in),
        .mem_illegal_instr   (mem_illegal_instr_in),
        .mem_is_ecall        (mem_is_ecall_in),
        .mem_is_ebreak       (mem_is_ebreak_in),
        .mem_is_mret         (mem_is_mret_in),
        .mem_is_sret         (mem_is_sret_in),
        .mem_is_wfi          (mem_is_wfi_in),
        .mem_is_sfence_vma   (mem_is_sfence_vma_in),
        .mem_is_fence        (mem_is_fence_in),
        .mem_is_fence_i      (mem_is_fence_i_in)
    );

    // ---------------------------------------------------------
    // (E) MEM Stage - Memory Access
    // ---------------------------------------------------------
    mem_stage u_mem_stage (
        .mem_alu_result      (mem_alu_result_in),
        .mem_branch_taken    (mem_branch_taken_in),
        .mem_branch_target   (mem_branch_target_in),
        .mem_rs1_rdata       (mem_rs1_rdata_in),
        .mem_rs2_rdata       (mem_rs2_rdata_in),
        .mem_rs1_addr        (mem_rs1_addr_in),
        .mem_pc              (mem_pc_in),
        .mem_imm             (mem_imm_in),
        .mem_is_load         (mem_is_load_in),
        .mem_is_store        (mem_is_store_in),
        .mem_mem_size        (mem_mem_size_in),
        .mem_reg_write_en    (mem_reg_write_en_in),
        .mem_rd_addr         (mem_rd_addr_in),
        .mem_wb_sel          (mem_wb_sel_in),
        .mem_is_csr          (mem_is_csr_in),
        .mem_csr_op          (mem_csr_op_in),
        .mem_illegal_instr   (mem_illegal_instr_in),
        .mem_is_ecall        (mem_is_ecall_in),
        .mem_is_ebreak       (mem_is_ebreak_in),
        .mem_is_mret         (mem_is_mret_in),
        .mem_is_sret         (mem_is_sret_in),
        .mem_is_wfi          (mem_is_wfi_in),
        .mem_is_sfence_vma   (mem_is_sfence_vma_in),
        .mem_is_fence        (mem_is_fence_in),
        .mem_is_fence_i      (mem_is_fence_i_in),
        .dmem_read_req       (dmem_read_en),
        .dmem_write_req      (dmem_write_en),
        .dmem_addr           (dmem_addr),
        .dmem_wdata          (dmem_wdata),
        .dmem_wstrb          (mw_reg_dmem_wstrb_in), 
        .dmem_req_ready      (1'b1),
        .dmem_rsp_valid      (1'b1),
        .dmem_rdata          (dmem_rdata),
        .mem_wb_alu_result   (mw_reg_alu_result_in),
        .mem_wb_mem_rdata    (mw_reg_mem_rdata_in),
        .mem_wb_pc           (mw_reg_pc_in),
        .mem_wb_reg_write_en (mw_reg_reg_write_en_in),
        .mem_wb_rd_addr      (mw_reg_rd_addr_in),
        .mem_wb_wb_sel       (mw_reg_wb_sel_in),
        .mem_wb_is_csr       (mw_reg_is_csr_in),
        .mem_wb_csr_op       (mw_reg_csr_op_in),
        .mem_wb_rs1_rdata    (mw_reg_rs1_rdata_in),
        .mem_wb_rs1_addr     (mw_reg_rs1_addr_in),
        .mem_wb_imm          (mw_reg_imm_in),
        .mem_wb_illegal_instr(mw_reg_illegal_instr_in),
        .mem_wb_is_ecall     (mw_reg_is_ecall_in),
        .mem_wb_is_ebreak    (mw_reg_is_ebreak_in),
        .mem_wb_is_mret      (mw_reg_is_mret_in),
        .mem_wb_is_sret      (mw_reg_is_sret_in),
        .mem_stall_req       (mem_stall_req)
    );

    assign dmem_size = mem_mem_size_in;

    mem_wb_reg u_mem_wb_reg (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (1'b0),
        .flush_en           (1'b0),
        .mem_pc              (mw_reg_pc_in),
        .mem_alu_result      (mw_reg_alu_result_in),
        .mem_mem_rdata       (mw_reg_mem_rdata_in),
        .mem_reg_write_en    (mw_reg_reg_write_en_in),
        .mem_rd_addr         (mw_reg_rd_addr_in),
        .mem_wb_sel          (mw_reg_wb_sel_in),
        .mem_is_csr          (mw_reg_is_csr_in),
        .mem_csr_op          (mw_reg_csr_op_in),
        .mem_rs1_rdata       (mw_reg_rs1_rdata_in),
        .mem_rs1_addr        (mw_reg_rs1_addr_in),
        .mem_imm             (mw_reg_imm_in),
        .mem_illegal_instr   (mw_reg_illegal_instr_in),
        .mem_is_ecall        (mw_reg_is_ecall_in),
        .mem_is_ebreak       (mw_reg_is_ebreak_in),
        .mem_is_mret         (mw_reg_is_mret_in),
        .mem_is_sret         (mw_reg_is_sret_in),
        .wb_pc               (wb_pc_in),
        .wb_alu_result       (wb_alu_result_in),
        .wb_mem_rdata        (wb_mem_rdata_in),
        .wb_reg_write_en     (wb_reg_write_en_in),
        .wb_rd_addr          (wb_rd_addr_in),
        .wb_wb_sel           (wb_wb_sel_in),
        .wb_is_csr           (wb_is_csr_in),
        .wb_csr_op           (wb_csr_op_in),
        .wb_rs1_rdata        (wb_rs1_rdata_in),
        .wb_rs1_addr         (wb_rs1_addr_in),
        .wb_imm              (wb_imm_in),
        .wb_illegal_instr    (wb_illegal_instr_in),
        .wb_is_ecall         (wb_is_ecall_in),
        .wb_is_ebreak        (wb_is_ebreak_in),
        .wb_is_mret          (wb_is_mret_in),
        .wb_is_sret          (wb_is_sret_in)
    );

    // ---------------------------------------------------------
    // (F) WB Stage - Write Back & System Interaction
    // ---------------------------------------------------------
    wb_stage u_wb_stage (
        .mem_wb_alu_result  (wb_alu_result_in),
        .mem_wb_mem_rdata   (wb_mem_rdata_in),
        .mem_wb_pc          (wb_pc_in),
        .mem_wb_pc_plus_4   (wb_pc_in + 64'd4),
        .mem_wb_reg_write_en(wb_reg_write_en_in),
        .mem_wb_rd_addr     (wb_rd_addr_in),
        .mem_wb_wb_sel      (wb_wb_sel_in),
        .mem_wb_is_csr      (wb_is_csr_in),
        .mem_wb_csr_op      (wb_csr_op_in),
        .mem_wb_rs1_rdata   (wb_rs1_rdata_in),
        .mem_wb_rs1_addr    (wb_rs1_addr_in),
        .mem_wb_imm         (wb_imm_in),
        .mem_wb_illegal_instr(wb_illegal_instr_in),
        .mem_wb_is_ecall    (wb_is_ecall_in),
        .mem_wb_is_ebreak   (wb_is_ebreak_in),
        .mem_wb_is_mret     (wb_is_mret_in),
        .mem_wb_is_sret     (wb_is_sret_in),
        .csr_rdata          (csr_rdata),
        .wb_rd_wdata        (wb_rd_wdata),
        .wb_rd_addr         (wb_rd_addr),
        .wb_reg_write_en    (wb_reg_write_en_out),
        .csr_req            (csr_req),
        .csr_op             (csr_op),
        .csr_addr           (csr_addr),
        .csr_rs1_data       (csr_rs1_data),
        .trap_illegal_instr (trap_illegal_instr),
        .trap_is_ecall      (trap_is_ecall),
        .trap_is_ebreak     (trap_is_ebreak),
        .trap_is_mret       (trap_is_mret),
        .trap_is_sret       (trap_is_sret),
        .trap_epc           (trap_epc)
    );

    csr_unit u_csr_unit (
        .clk                (clk),
        .rst                (rst),
        .csr_req            (csr_req),
        .csr_op             (csr_op),
        .csr_addr           (csr_addr),
        .csr_rs1_data       (csr_rs1_data),
        .trap_illegal_instr (trap_illegal_instr),
        .trap_is_ecall      (trap_is_ecall),
        .trap_is_ebreak     (trap_is_ebreak),
        .trap_is_mret       (trap_is_mret),
        .trap_is_sret       (trap_is_sret),
        .trap_epc           (trap_epc),
        .csr_rdata          (csr_rdata),
        .csr_mstatus        (csr_mstatus_value),
        .csr_sstatus        (csr_sstatus_value),
        .csr_medeleg        (csr_medeleg_value),
        .csr_mideleg        (csr_mideleg_value),
        .csr_mtvec          (csr_mtvec_base),
        .csr_stvec          (csr_stvec_base),
        .csr_mepc           (csr_mepc_value),
        .csr_sepc           (csr_sepc_value),
        .csr_satp           (csr_satp_value),
        .csr_priv_mode      (csr_priv_mode_cur),
        .csr_trap_vector    (csr_trap_vector_value),
        .csr_trap_to_s_mode (csr_trap_to_s_mode_flag),
        .csr_illegal_access (csr_illegal_access_flag)
    );

endmodule
