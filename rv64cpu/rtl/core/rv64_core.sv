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

module rv64_core
    import rv64_pkg::*;
    import csr_pkg::*;
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
    input  logic [63:0] dmem_rdata,

    // Interface 3: External Interrupt Sources
    input  logic        m_timer_interrupt,
    input  logic        m_software_interrupt,
    input  logic        m_external_interrupt,
    input  logic        s_timer_interrupt,
    input  logic        s_software_interrupt,
    input  logic        s_external_interrupt
);
    localparam int CSR_MSTATUS_MIE_BIT = 3;
    localparam int CSR_MSTATUS_SUM_BIT = 18;
    localparam int CSR_MSTATUS_MXR_BIT = 19;
    localparam int CSR_MSTATUS_TVM_BIT = 20;
    localparam int CSR_MSTATUS_TW_BIT  = 21;
    localparam int CSR_MSTATUS_TSR_BIT = 22;

    // -----------------------
    // Internal Wire Declarations
    // -----------------------

    // (1) Hazard Unit Controls
    logic [1:0]        hu_forward_a_sel, hu_forward_b_sel;
    logic              hu_if_stall_en, hu_if_id_stall_en, hu_id_ex_stall_en, hu_ex_mem_stall_en;
    logic              hu_if_id_flush_en, hu_id_ex_flush_en;
    logic              hu_pc_redirect_sel, hu_pc_redirect_is_trap;
    logic [XLEN-1:0]   hu_pc_redirect_target;

    // (2) IF Stage Outputs
    logic [XLEN-1:0]   if_pc_out;
    logic [INST_WIDTH-1:0] if_instr_to_decode;
    logic [INST_WIDTH-1:0] if_compressed_expanded_instr;
    logic              if_instr_compressed;
    logic [2:0]        if_instr_len;
    logic              if_compressed_illegal;
    // imem interface (ports)

    // (3) IF/ID Pipeline Register Outputs
    logic [XLEN-1:0]   if_id_pc;
    logic [31:0]       if_id_instr;
    logic [2:0]        if_id_instr_len;

    // (4) ID Stage Outputs
    logic [XLEN-1:0]   id_pc_out;
    logic [2:0]        id_instr_len;
    logic [XLEN-1:0]   id_rs1_rdata, id_rs2_rdata;
    logic [XLEN-1:0]   id_fs1_rdata, id_fs2_rdata, id_fs3_rdata;
    logic [4:0]        id_rs1_addr, id_rs2_addr;
    logic [4:0]        id_fs3_addr;
    logic [XLEN-1:0]   id_imm;
    alu_op_t           id_alu_op;
    logic              id_alu_src1_sel, id_alu_src2_sel;
    logic              id_is_word_op, id_is_branch, id_is_jump;
    branch_op_t        id_branch_op;
    logic              id_is_load, id_is_store;
    logic [2:0]        id_mem_size;
    logic              id_is_amo;
    amo_op_t           id_amo_op;
    logic              id_is_fp, id_fp_reg_write_en, id_fp_store_data_sel;
    fp_op_t            id_fp_op;
    fp_fmt_t           id_fp_fmt;
    logic [2:0]        id_fp_rm;
    logic              id_reg_write_en;
    logic [4:0]        id_rd_addr;
    logic [1:0]        id_wb_sel;
    logic              id_is_csr;
    logic [2:0]        id_csr_op;
    logic              id_illegal_instr, id_is_ecall, id_is_ebreak, id_is_mret, id_is_sret;
    logic              id_is_wfi, id_is_sfence_vma, id_is_fence, id_is_fence_i;

    // (5) ID/EX Pipeline Register Outputs
    logic [XLEN-1:0]   ex_pc_in;
    logic [INST_WIDTH-1:0] ex_instr_in;
    logic [2:0]        ex_instr_len_in;
    logic [XLEN-1:0]   ex_rs1_rdata_in, ex_rs2_rdata_in;
    logic [XLEN-1:0]   ex_fs1_rdata_in, ex_fs2_rdata_in, ex_fs3_rdata_in;
    logic [4:0]        ex_rs1_addr_in, ex_rs2_addr_in;
    logic [4:0]        ex_fs3_addr_in;
    logic [XLEN-1:0]   ex_imm_in;
    alu_op_t           ex_alu_op_in;
    logic              ex_alu_src1_sel_in, ex_alu_src2_sel_in;
    logic              ex_is_word_op_in, ex_is_branch_in, ex_is_jump_in;
    branch_op_t        ex_branch_op_in;
    logic              ex_is_load_in, ex_is_store_in;
    logic [2:0]        ex_mem_size_in;
    logic              ex_is_amo_in;
    amo_op_t           ex_amo_op_in;
    logic              ex_is_fp_in, ex_fp_reg_write_en_in, ex_fp_store_data_sel_in;
    fp_op_t            ex_fp_op_in;
    fp_fmt_t           ex_fp_fmt_in;
    logic [2:0]        ex_fp_rm_in;
    logic              ex_reg_write_en_in;
    logic [4:0]        ex_rd_addr_in;
    logic [1:0]        ex_wb_sel_in;
    logic              ex_is_csr_in;
    logic [2:0]        ex_csr_op_in;
    logic              ex_illegal_instr_in, ex_is_ecall_in, ex_is_ebreak_in, ex_is_mret_in, ex_is_sret_in;
    logic              ex_is_wfi_in, ex_is_sfence_vma_in, ex_is_fence_in, ex_is_fence_i_in;

    // (6) EX Stage Outputs (and Forwarding Muxes)
    logic [XLEN-1:0]   ex_rs1_rdata_fwd, ex_rs2_rdata_fwd;
    logic [XLEN-1:0]   ex_fs1_rdata_fwd, ex_fs2_rdata_fwd, ex_fs3_rdata_fwd;
    logic [XLEN-1:0]   mem_forward_data;
    logic [XLEN-1:0]   ex_alu_result_out;
    logic [XLEN-1:0]   ex_result_to_mem;
    logic [XLEN-1:0]   ex_fp_result_out;
    logic [XLEN-1:0]   ex_fp_int_result_out;
    logic [4:0]        ex_fp_fflags_out;
    logic              ex_fp_illegal_out;
    logic              ex_branch_taken_out;
    logic [XLEN-1:0]   ex_branch_target_out;
    logic              ex_redirect_taken;
    logic [XLEN-1:0]   ex_redirect_target;

    // (7) EX/MEM Pipeline Register Outputs
    logic [XLEN-1:0]   mem_pc_in;
    logic [INST_WIDTH-1:0] mem_instr_in;
    logic [2:0]        mem_instr_len_in;
    logic [XLEN-1:0]   mem_alu_result_in;
    logic              mem_branch_taken_in;
    logic [XLEN-1:0]   mem_branch_target_in;
    logic [XLEN-1:0]   mem_rs1_rdata_in, mem_rs2_rdata_in;
    logic [XLEN-1:0]   mem_fs1_rdata_in, mem_fs2_rdata_in, mem_fs3_rdata_in;
    logic [4:0]        mem_rs1_addr_in;
    logic [4:0]        mem_fs3_addr_in;
    logic [XLEN-1:0]   mem_imm_in;
    logic              mem_is_load_in, mem_is_store_in;
    logic [2:0]        mem_mem_size_in;
    logic              mem_is_amo_in;
    amo_op_t           mem_amo_op_in;
    logic              mem_is_fp_in, mem_fp_reg_write_en_in, mem_fp_store_data_sel_in;
    fp_op_t            mem_fp_op_in;
    fp_fmt_t           mem_fp_fmt_in;
    logic [2:0]        mem_fp_rm_in;
    logic [XLEN-1:0]   mem_fp_result_in;
    logic [4:0]        mem_fp_fflags_in;
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
    logic [XLEN-1:0]   mw_reg_fp_result_in;
    logic [4:0]        mw_reg_fp_fflags_in;
    logic [XLEN-1:0]   mw_reg_pc_in;
    logic [INST_WIDTH-1:0] mw_reg_instr_in;
    logic [2:0]        mw_reg_instr_len_in;
    logic [7:0]        mw_reg_dmem_wstrb_in;
    logic              mw_reg_reg_write_en_in;
    logic              mw_reg_fp_reg_write_en_in;
    logic [4:0]        mw_reg_rd_addr_in;
    logic [1:0]        mw_reg_wb_sel_in;
    logic              mw_reg_is_csr_in;
    logic [2:0]        mw_reg_csr_op_in;
    logic [XLEN-1:0]   mw_reg_rs1_rdata_in;
    logic [4:0]        mw_reg_rs1_addr_in;
    logic [XLEN-1:0]   mw_reg_imm_in;
    logic              mw_reg_illegal_instr_in, mw_reg_is_ecall_in, mw_reg_is_ebreak_in, mw_reg_is_mret_in, mw_reg_is_sret_in;
    logic              mw_reg_load_fault_in, mw_reg_store_fault_in;
    logic              mw_reg_load_page_fault_in, mw_reg_store_page_fault_in;
    logic [XLEN-1:0]   mw_reg_fault_addr_in;
    logic              mem_stall_req;

    // (9) MEM/WB Pipeline Register Outputs
    logic [XLEN-1:0]   wb_pc_in;
    logic [XLEN-1:0]   wb_alu_result_in;
    logic [XLEN-1:0]   wb_mem_rdata_in;
    logic [XLEN-1:0]   wb_fp_result_in;
    logic [4:0]        wb_fp_fflags_in;
    logic [INST_WIDTH-1:0] wb_instr_in;
    logic [2:0]        wb_instr_len_in;
    logic              wb_reg_write_en_in;
    logic              wb_fp_reg_write_en_in;
    logic [4:0]        wb_rd_addr_in;
    logic [1:0]        wb_wb_sel_in;
    logic              wb_is_csr_in;
    logic [2:0]        wb_csr_op_in;
    logic [XLEN-1:0]   wb_rs1_rdata_in;
    logic [4:0]        wb_rs1_addr_in;
    logic [XLEN-1:0]   wb_imm_in;
    logic              wb_illegal_instr_in, wb_is_ecall_in, wb_is_ebreak_in, wb_is_mret_in, wb_is_sret_in;
    logic              wb_load_fault, wb_store_fault;
    logic              wb_load_page_fault, wb_store_page_fault;
    logic [XLEN-1:0]   wb_fault_addr_in;

    // (10) WB Stage Outputs (to RegFile & CSR Unit)
    logic [XLEN-1:0]   wb_rd_wdata;
    logic [4:0]        wb_rd_addr;
    logic              wb_reg_write_en_out;
    logic [XLEN-1:0]   wb_fd_wdata;
    logic [4:0]        wb_fd_addr;
    logic              wb_fp_write_en;
    logic [4:0]        wb_fp_fflags;
    logic              wb_fp_state_dirty;
    logic              wb_fp_fflags_valid;
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
    logic [XLEN-1:0]   csr_mie_value;
    logic [XLEN-1:0]   csr_sie_value;
    logic [XLEN-1:0]   csr_mip_value;
    logic [XLEN-1:0]   csr_sip_value;
    logic [XLEN-1:0]   csr_mepc_value;
    logic [XLEN-1:0]   csr_sepc_value;
    logic [XLEN-1:0]   csr_satp_value;
    logic [2:0]        csr_frm_value;
    logic [XLEN-1:0]   csr_trap_vector_value;
    logic              csr_trap_to_s_mode;
    logic              csr_illegal_access;
    pmpcfg_entry_t     csr_pmpcfg_entries [PMP_ENTRY_COUNT];
    pmpaddr_entry_t    csr_pmpaddr_entries [PMP_ENTRY_COUNT];

    // (11) Exception / Trap bus
    logic              trap_illegal_instr, trap_is_ecall, trap_is_ebreak, trap_is_mret, trap_is_sret;
    logic [XLEN-1:0]   trap_epc;
    logic              trap_instr_access_fault;

    // (12) Trap Controller + interrupt wiring
    logic              trap_load_fault, trap_store_fault;
    logic              trap_instr_page_fault, trap_load_page_fault, trap_store_page_fault;
    logic [XLEN-1:0]   trap_bad_addr;
    logic [INST_WIDTH-1:0] trap_bad_instr;
    logic              trap_flush_req;
    logic              trap_pc_sel;
    logic [XLEN-1:0]   trap_next_pc;
    logic              trap_pc_sel_raw;
    logic [XLEN-1:0]   trap_next_pc_raw;
    logic              trap_ctrl_trigger;
    logic [XLEN-1:0]   trap_ctrl_mepc;
    logic [XLEN-1:0]   trap_ctrl_mcause;
    logic [XLEN-1:0]   trap_ctrl_mtval;
    logic              trap_ctrl_to_s_mode;
    logic              trap_ctrl_mret_en;
    logic              trap_ctrl_sret_en;
    logic              csr_mstatus_mie;
    logic              csr_mstatus_sum;
    logic              csr_mstatus_mxr;
    logic              if_instr_access_fault;
    logic              if_instr_page_fault;
    logic              if_id_instr_access_fault;
    logic              if_id_instr_page_fault;
    logic              ex_instr_access_fault_in;
    logic              ex_instr_page_fault_in;
    logic              mem_instr_access_fault_in;
    logic              mem_instr_page_fault_in;
    logic              mw_reg_instr_access_fault_in;
    logic              mw_reg_instr_page_fault_in;
    logic              wb_instr_access_fault;
    logic              wb_instr_page_fault;
    logic              dmem_access_fault;
    logic              dmem_load_access_fault;
    logic              dmem_store_access_fault;
    logic              dmem_load_page_fault;
    logic              dmem_store_page_fault;
    logic [3:0]        dmem_access_size;

    logic              mmu_i_req_valid;
    logic              mmu_i_resp_valid;
    logic              mmu_i_resp_page_fault;
    logic [XLEN-1:0]   mmu_i_resp_vaddr;
    logic [XLEN-1:0]   mmu_i_resp_paddr;
    logic              mmu_d_req_valid;
    logic              mmu_d_resp_valid;
    logic              mmu_d_resp_page_fault;
    logic [XLEN-1:0]   mmu_d_resp_paddr;
    logic              ptw_mem_req_valid;
    logic              ptw_mem_req_ready;
    logic [XLEN-1:0]   ptw_mem_req_addr;
    logic              ptw_mem_resp_valid;
    logic [XLEN-1:0]   ptw_mem_resp_data;
    logic              ptw_mem_resp_valid_q;
    logic [XLEN-1:0]   ptw_mem_resp_data_q;
    logic              raw_dmem_read_en;
    logic              raw_dmem_write_en;
    logic [XLEN-1:0]   raw_dmem_addr;
    logic [XLEN-1:0]   raw_dmem_wdata;
    logic [7:0]        raw_dmem_wstrb;
    logic              data_access_needs_translation;
    logic              data_translation_ready;
    logic              data_translation_page_fault;
    logic              data_memory_port_available;
    logic              if_translation_ready;
    logic              if_translation_page_fault;
    logic              mmu_stall_req;
    logic              dmem_req_is_store;

    assign csr_mstatus_mie        = csr_mstatus_value[CSR_MSTATUS_MIE_BIT];
    assign csr_mstatus_sum        = csr_mstatus_value[CSR_MSTATUS_SUM_BIT];
    assign csr_mstatus_mxr        = csr_mstatus_value[CSR_MSTATUS_MXR_BIT];
    assign ex_redirect_taken       = ex_branch_taken_out | ex_is_fence_i_in;
    assign ex_redirect_target      = ex_is_fence_i_in ?
                                     (ex_pc_in + {{(XLEN-3){1'b0}}, ex_instr_len_in}) :
                                     ex_branch_target_out;

    always_comb begin
        unique case (mem_mem_size_in)
            3'b000, 3'b100: dmem_access_size = 4'd1;
            3'b001, 3'b101: dmem_access_size = 4'd2;
            3'b010, 3'b110: dmem_access_size = 4'd4;
            3'b011:         dmem_access_size = 4'd8;
            default:        dmem_access_size = 4'd1;
        endcase
    end

    assign mmu_i_req_valid = !rst;
    assign dmem_req_is_store = mem_is_store_in | (mem_is_amo_in && (mem_amo_op_in != AMO_LR));

    assign mmu_d_req_valid = (mem_is_load_in | mem_is_store_in | mem_is_amo_in) &
                              ~mem_instr_access_fault_in & ~mem_instr_page_fault_in;

    assign if_translation_ready      = mmu_i_resp_valid & (mmu_i_resp_vaddr == if_pc_out);
    assign if_translation_page_fault = if_translation_ready & mmu_i_resp_page_fault;
    assign if_instr_page_fault       = if_translation_page_fault;

    assign data_access_needs_translation = mmu_d_req_valid;
    assign data_translation_ready        = !data_access_needs_translation | mmu_d_resp_valid;
    assign data_translation_page_fault   = data_access_needs_translation & mmu_d_resp_valid &
                                           mmu_d_resp_page_fault;
    assign data_memory_port_available    = !ptw_mem_req_valid;
    assign dmem_load_page_fault          = (mem_is_load_in | (mem_is_amo_in && (mem_amo_op_in == AMO_LR))) &
                                           data_translation_page_fault;
    assign dmem_store_page_fault         = dmem_req_is_store & data_translation_page_fault;
    assign mmu_stall_req                 = !if_translation_ready;

    assign ptw_mem_req_ready  = 1'b1;
    assign ptw_mem_resp_valid = ptw_mem_resp_valid_q;
    assign ptw_mem_resp_data  = ptw_mem_resp_data_q;

    always_ff @(posedge clk) begin
        if (rst) begin
            ptw_mem_resp_valid_q <= 1'b0;
            ptw_mem_resp_data_q  <= '0;
        end else begin
            ptw_mem_resp_valid_q <= ptw_mem_req_valid & ptw_mem_req_ready;
            if (ptw_mem_req_valid & ptw_mem_req_ready) begin
                ptw_mem_resp_data_q <= dmem_rdata;
            end
        end
    end

    mmu u_mmu_top (
        .clk                  (clk),
        .rst                  (rst),
        .satp_value           (csr_satp_value),
        .priv_mode            (csr_priv_mode_cur),
        .sum                  (csr_mstatus_sum),
        .mxr                  (csr_mstatus_mxr),
        .i_req_valid          (mmu_i_req_valid),
        .i_req_ready          (),
        .i_req_vaddr          (if_pc_out),
        .i_resp_valid         (mmu_i_resp_valid),
        .i_resp_hit           (),
        .i_resp_page_fault    (mmu_i_resp_page_fault),
        .i_resp_vaddr         (mmu_i_resp_vaddr),
        .i_resp_paddr         (mmu_i_resp_paddr),
        .d_req_valid          (mmu_d_req_valid),
        .d_req_ready          (),
        .d_req_vaddr          (mem_alu_result_in),
        .d_req_is_store       (dmem_req_is_store),
        .d_resp_valid         (mmu_d_resp_valid),
        .d_resp_hit           (),
        .d_resp_page_fault    (mmu_d_resp_page_fault),
        .d_resp_paddr         (mmu_d_resp_paddr),
        .flush_all            (mem_is_sfence_vma_in),
        .flush_asid_valid     (1'b0),
        .flush_asid           ('0),
        .flush_vpn_valid      (1'b0),
        .flush_vpn            ('0),
        .ptw_mem_req_valid    (ptw_mem_req_valid),
        .ptw_mem_req_ready    (ptw_mem_req_ready),
        .ptw_mem_req_addr     (ptw_mem_req_addr),
        .ptw_mem_resp_valid   (ptw_mem_resp_valid),
        .ptw_mem_resp_data    (ptw_mem_resp_data)
    );

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
        .ex_is_load         (ex_is_load_in | ex_is_amo_in),
        .ex_is_csr          (ex_is_csr_in),
        .ex_branch_taken    (ex_redirect_taken),
        .ex_branch_target   (ex_redirect_target),
        .mem_rd_addr        (mem_rd_addr_in),
        .mem_reg_write_en   (mem_reg_write_en_in),
        .mem_result_late    (mem_wb_sel_in == 2'b01),
        .mem_is_csr         (mem_is_csr_in),
        .mem_stall_req      (mem_stall_req | mmu_stall_req),
        .trap_flush_req     (trap_flush_req),
        .trap_pc_sel        (trap_pc_sel),
        .trap_target_pc     (trap_next_pc),
        .wb_rd_addr         (wb_rd_addr_in),
        .wb_reg_write_en    (wb_reg_write_en_out),
        .forward_a_sel      (hu_forward_a_sel),
        .forward_b_sel      (hu_forward_b_sel),
        .if_stall_en        (hu_if_stall_en),
        .if_id_stall_en     (hu_if_id_stall_en),
        .id_ex_stall_en     (hu_id_ex_stall_en),
        .ex_mem_stall_en    (hu_ex_mem_stall_en),
        .if_id_flush_en     (hu_if_id_flush_en),
        .id_ex_flush_en     (hu_id_ex_flush_en),
        .pc_redirect_sel    (hu_pc_redirect_sel),
        .pc_redirect_target (hu_pc_redirect_target),
        .pc_redirect_is_trap(hu_pc_redirect_is_trap)
    );

    // ---------------------------------------------------------
    // (B) IF Stage - Instruction Fetch
    // ---------------------------------------------------------
    if_stage u_if_stage (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_if_stall_en),
        .pc_redirect_sel    (hu_pc_redirect_sel),
        .pc_redirect_target (hu_pc_redirect_target),
        .pc_redirect_is_trap(hu_pc_redirect_is_trap),
        .if_instr_compressed(if_instr_compressed),
        .if_pc              (if_pc_out)
    );

    assign imem_addr = (if_translation_ready && !mmu_i_resp_page_fault) ? mmu_i_resp_paddr : if_pc_out;

    assign if_instr_compressed = (imem_rdata[1:0] != 2'b11);
    assign if_instr_len        = if_instr_compressed ? 3'd2 : 3'd4;

    compressed_decoder u_compressed_decoder (
        .compressed_instr (imem_rdata[15:0]),
        .expanded_instr   (if_compressed_expanded_instr),
        .illegal_instr    (if_compressed_illegal)
    );

    always_comb begin
        if_instr_to_decode = imem_rdata;
        if (if_instr_compressed) begin
            if_instr_to_decode = if_compressed_illegal ? 32'h0000_0000 : if_compressed_expanded_instr;
        end
    end

    pmp_checker u_pmp_checker_if (
        .req_valid       (if_translation_ready && !mmu_i_resp_page_fault),
        .req_paddr       (mmu_i_resp_paddr),
        .req_size        (4'd4),
        .req_is_fetch    (1'b1),
        .req_is_store    (1'b0),
        .priv_mode       (csr_priv_mode_cur),
        .pmpcfg_entries  (csr_pmpcfg_entries),
        .pmpaddr_entries (csr_pmpaddr_entries),
        .access_fault    (if_instr_access_fault)
    );

    if_id_reg u_if_id_reg (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_if_id_stall_en),
        .flush_en           (hu_if_id_flush_en),
        .if_pc                  (if_pc_out),
        .if_instr               (if_instr_to_decode),
        .if_instr_len           (if_instr_len),
        .if_instr_access_fault  (if_instr_access_fault),
        .if_instr_page_fault    (if_instr_page_fault),
        .id_pc                  (if_id_pc),
        .id_instr               (if_id_instr),
        .id_instr_len           (if_id_instr_len),
        .id_instr_access_fault  (if_id_instr_access_fault),
        .id_instr_page_fault    (if_id_instr_page_fault)
    );

    // ---------------------------------------------------------
    // (C) ID Stage - Instruction Decode & RegFile
    // ---------------------------------------------------------
    id_stage u_id_stage (
        .clk                (clk),
        .rst                (rst),
        .if_pc              (if_id_pc),
        .if_instr           (if_id_instr),
        .if_instr_len       (if_id_instr_len),
        .csr_priv_mode      (csr_priv_mode_cur),
        .csr_mstatus_tsr    (csr_mstatus_value[CSR_MSTATUS_TSR_BIT]),
        .csr_mstatus_tw     (csr_mstatus_value[CSR_MSTATUS_TW_BIT]),
        .csr_mstatus_tvm    (csr_mstatus_value[CSR_MSTATUS_TVM_BIT]),
        .wb_reg_write_en    (wb_reg_write_en_out),
        .wb_rd_addr         (wb_rd_addr),
        .wb_rd_wdata        (wb_rd_wdata),
        .wb_fp_write_en     (wb_fp_write_en),
        .wb_fd_addr         (wb_fd_addr),
        .wb_fd_wdata        (wb_fd_wdata),
        .id_pc              (id_pc_out),
        .id_instr_len       (id_instr_len),
        .id_rs1_rdata       (id_rs1_rdata),
        .id_rs2_rdata       (id_rs2_rdata),
        .id_fs1_rdata       (id_fs1_rdata),
        .id_fs2_rdata       (id_fs2_rdata),
        .id_fs3_rdata       (id_fs3_rdata),
        .id_rs1_addr        (id_rs1_addr),
        .id_rs2_addr        (id_rs2_addr),
        .id_fs3_addr        (id_fs3_addr),
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
        .id_is_amo          (id_is_amo),
        .id_amo_op          (id_amo_op),
        .id_is_fp           (id_is_fp),
        .id_fp_reg_write_en (id_fp_reg_write_en),
        .id_fp_store_data_sel(id_fp_store_data_sel),
        .id_fp_op           (id_fp_op),
        .id_fp_fmt          (id_fp_fmt),
        .id_fp_rm           (id_fp_rm),
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
        .id_instr           (if_id_instr),
        .id_instr_len       (id_instr_len),
        .id_pc              (id_pc_out),
        .id_rs1_rdata       (id_rs1_rdata),
        .id_rs2_rdata       (id_rs2_rdata),
        .id_fs1_rdata       (id_fs1_rdata),
        .id_fs2_rdata       (id_fs2_rdata),
        .id_fs3_rdata       (id_fs3_rdata),
        .id_rs1_addr        (id_rs1_addr),
        .id_rs2_addr        (id_rs2_addr),
        .id_fs3_addr        (id_fs3_addr),
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
        .id_is_amo          (id_is_amo),
        .id_amo_op          (id_amo_op),
        .id_is_fp           (id_is_fp),
        .id_fp_reg_write_en (id_fp_reg_write_en),
        .id_fp_store_data_sel(id_fp_store_data_sel),
        .id_fp_op           (id_fp_op),
        .id_fp_fmt          (id_fp_fmt),
        .id_fp_rm           (id_fp_rm),
        .id_reg_write_en    (id_reg_write_en),
        .id_rd_addr         (id_rd_addr),
        .id_wb_sel          (id_wb_sel),
        .id_is_csr              (id_is_csr),
        .id_csr_op              (id_csr_op),
        .id_illegal_instr       (id_illegal_instr),
        .id_instr_access_fault  (if_id_instr_access_fault),
        .id_instr_page_fault    (if_id_instr_page_fault),
        .id_is_ecall            (id_is_ecall),
        .id_is_ebreak       (id_is_ebreak),
        .id_is_mret         (id_is_mret),
        .id_is_sret         (id_is_sret),
        .id_is_wfi          (id_is_wfi),
        .id_is_sfence_vma   (id_is_sfence_vma),
        .id_is_fence        (id_is_fence),
        .id_is_fence_i      (id_is_fence_i),
        .ex_pc              (ex_pc_in),
        .ex_instr           (ex_instr_in),
        .ex_instr_len       (ex_instr_len_in),
        .ex_rs1_rdata       (ex_rs1_rdata_in),
        .ex_rs2_rdata       (ex_rs2_rdata_in),
        .ex_fs1_rdata       (ex_fs1_rdata_in),
        .ex_fs2_rdata       (ex_fs2_rdata_in),
        .ex_fs3_rdata       (ex_fs3_rdata_in),
        .ex_rs1_addr        (ex_rs1_addr_in),
        .ex_rs2_addr        (ex_rs2_addr_in),
        .ex_fs3_addr        (ex_fs3_addr_in),
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
        .ex_is_amo          (ex_is_amo_in),
        .ex_amo_op          (ex_amo_op_in),
        .ex_is_fp           (ex_is_fp_in),
        .ex_fp_reg_write_en (ex_fp_reg_write_en_in),
        .ex_fp_store_data_sel(ex_fp_store_data_sel_in),
        .ex_fp_op           (ex_fp_op_in),
        .ex_fp_fmt          (ex_fp_fmt_in),
        .ex_fp_rm           (ex_fp_rm_in),
        .ex_reg_write_en    (ex_reg_write_en_in),
        .ex_rd_addr         (ex_rd_addr_in),
        .ex_wb_sel          (ex_wb_sel_in),
        .ex_is_csr              (ex_is_csr_in),
        .ex_csr_op              (ex_csr_op_in),
        .ex_illegal_instr       (ex_illegal_instr_in),
        .ex_instr_access_fault  (ex_instr_access_fault_in),
        .ex_instr_page_fault    (ex_instr_page_fault_in),
        .ex_is_ecall            (ex_is_ecall_in),
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

    always_comb begin
        unique case (mem_wb_sel_in)
            2'b00:   mem_forward_data = mem_alu_result_in;
            2'b01:   mem_forward_data = mw_reg_mem_rdata_in;
            2'b10:   mem_forward_data = mem_pc_in + 64'd4;
            default: mem_forward_data = '0;
        endcase
    end
    
    // Operand A Forwarding Mux
    always_comb begin
        case (hu_forward_a_sel)
            2'b01:   ex_rs1_rdata_fwd = mem_forward_data;  // From MEM
            2'b10:   ex_rs1_rdata_fwd = wb_rd_wdata;       // From WB
            default: ex_rs1_rdata_fwd = ex_rs1_rdata_in;   // From RegFile
        endcase
    end

    // Operand B Forwarding Mux
    always_comb begin
        case (hu_forward_b_sel)
            2'b01:   ex_rs2_rdata_fwd = mem_forward_data;  // From MEM
            2'b10:   ex_rs2_rdata_fwd = wb_rd_wdata;       // From WB
            default: ex_rs2_rdata_fwd = ex_rs2_rdata_in;   // From RegFile
        endcase
    end

    always_comb begin
        if (mem_fp_reg_write_en_in && (mem_rd_addr_in == ex_rs1_addr_in)) begin
            ex_fs1_rdata_fwd = mw_reg_fp_result_in;
        end else if (wb_fp_write_en && (wb_fd_addr == ex_rs1_addr_in)) begin
            ex_fs1_rdata_fwd = wb_fd_wdata;
        end else begin
            ex_fs1_rdata_fwd = ex_fs1_rdata_in;
        end
    end

    always_comb begin
        if (mem_fp_reg_write_en_in && (mem_rd_addr_in == ex_rs2_addr_in)) begin
            ex_fs2_rdata_fwd = mw_reg_fp_result_in;
        end else if (wb_fp_write_en && (wb_fd_addr == ex_rs2_addr_in)) begin
            ex_fs2_rdata_fwd = wb_fd_wdata;
        end else begin
            ex_fs2_rdata_fwd = ex_fs2_rdata_in;
        end
    end

    always_comb begin
        if (mem_fp_reg_write_en_in && (mem_rd_addr_in == ex_fs3_addr_in)) begin
            ex_fs3_rdata_fwd = mw_reg_fp_result_in;
        end else if (wb_fp_write_en && (wb_fd_addr == ex_fs3_addr_in)) begin
            ex_fs3_rdata_fwd = wb_fd_wdata;
        end else begin
            ex_fs3_rdata_fwd = ex_fs3_rdata_in;
        end
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

    fpu u_fpu_unit (
        .fp_op          (ex_fp_op_in),
        .fp_fmt         (ex_fp_fmt_in),
        .rm             ((ex_fp_rm_in == 3'b111) ? csr_frm_value : ex_fp_rm_in),
        .funct3         (ex_instr_in[14:12]),
        .rs2_addr       (ex_rs2_addr_in),
        .fs1_data       (ex_fs1_rdata_fwd),
        .fs2_data       (ex_fs2_rdata_fwd),
        .fs3_data       (ex_fs3_rdata_fwd),
        .int_rs1_data   (ex_rs1_rdata_fwd),
        .fp_result      (ex_fp_result_out),
        .int_result     (ex_fp_int_result_out),
        .fflags         (ex_fp_fflags_out),
        .illegal_instr  (ex_fp_illegal_out)
    );

    assign ex_result_to_mem = (ex_is_fp_in && ex_reg_write_en_in) ?
                              ex_fp_int_result_out : ex_alu_result_out;

    ex_mem_reg u_ex_mem_reg (
        .clk                (clk),
        .rst                (rst),
        .stall_en           (hu_ex_mem_stall_en),
        .flush_en           (trap_flush_req),
        .ex_alu_result      (ex_result_to_mem),
        .ex_branch_taken    (ex_branch_taken_out),
        .ex_branch_target   (ex_branch_target_out),
        .ex_rs1_rdata       (ex_rs1_rdata_fwd),
        .ex_rs2_rdata       (ex_rs2_rdata_fwd),
        .ex_fs1_rdata       (ex_fs1_rdata_fwd),
        .ex_fs2_rdata       (ex_fs2_rdata_fwd),
        .ex_fs3_rdata       (ex_fs3_rdata_fwd),
        .ex_rs1_addr        (ex_rs1_addr_in),
        .ex_fs3_addr        (ex_fs3_addr_in),
        .ex_pc              (ex_pc_in),
        .ex_imm             (ex_imm_in),
        .ex_instr           (ex_instr_in),
        .ex_instr_len       (ex_instr_len_in),
        .ex_is_load         (ex_is_load_in),
        .ex_is_store        (ex_is_store_in),
        .ex_mem_size        (ex_mem_size_in),
        .ex_is_amo          (ex_is_amo_in),
        .ex_amo_op          (ex_amo_op_in),
        .ex_is_fp           (ex_is_fp_in),
        .ex_fp_reg_write_en (ex_fp_reg_write_en_in),
        .ex_fp_store_data_sel(ex_fp_store_data_sel_in),
        .ex_fp_op           (ex_fp_op_in),
        .ex_fp_fmt          (ex_fp_fmt_in),
        .ex_fp_rm           (ex_fp_rm_in),
        .ex_fp_result       (ex_fp_result_out),
        .ex_fp_fflags       (ex_fp_fflags_out),
        .ex_reg_write_en    (ex_reg_write_en_in),
        .ex_rd_addr         (ex_rd_addr_in),
        .ex_wb_sel          (ex_wb_sel_in),
        .ex_is_csr          (ex_is_csr_in),
        .ex_csr_op          (ex_csr_op_in),
        .ex_illegal_instr   (ex_illegal_instr_in | (ex_is_fp_in & ex_fp_illegal_out)),
        .ex_instr_access_fault(ex_instr_access_fault_in),
        .ex_instr_page_fault(ex_instr_page_fault_in),
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
        .mem_fs1_rdata       (mem_fs1_rdata_in),
        .mem_fs2_rdata       (mem_fs2_rdata_in),
        .mem_fs3_rdata       (mem_fs3_rdata_in),
        .mem_rs1_addr        (mem_rs1_addr_in),
        .mem_fs3_addr        (mem_fs3_addr_in),
        .mem_pc              (mem_pc_in),
        .mem_imm             (mem_imm_in),
        .mem_instr           (mem_instr_in),
        .mem_instr_len       (mem_instr_len_in),
        .mem_is_load         (mem_is_load_in),
        .mem_is_store        (mem_is_store_in),
        .mem_mem_size        (mem_mem_size_in),
        .mem_is_amo          (mem_is_amo_in),
        .mem_amo_op          (mem_amo_op_in),
        .mem_is_fp           (mem_is_fp_in),
        .mem_fp_reg_write_en (mem_fp_reg_write_en_in),
        .mem_fp_store_data_sel(mem_fp_store_data_sel_in),
        .mem_fp_op           (mem_fp_op_in),
        .mem_fp_fmt          (mem_fp_fmt_in),
        .mem_fp_rm           (mem_fp_rm_in),
        .mem_fp_result       (mem_fp_result_in),
        .mem_fp_fflags       (mem_fp_fflags_in),
        .mem_reg_write_en    (mem_reg_write_en_in),
        .mem_rd_addr         (mem_rd_addr_in),
        .mem_wb_sel          (mem_wb_sel_in),
        .mem_is_csr          (mem_is_csr_in),
        .mem_csr_op          (mem_csr_op_in),
        .mem_illegal_instr   (mem_illegal_instr_in),
        .mem_instr_access_fault(mem_instr_access_fault_in),
        .mem_instr_page_fault(mem_instr_page_fault_in),
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
    pmp_checker u_pmp_checker_dmem (
        .req_valid       (data_access_needs_translation && mmu_d_resp_valid && !mmu_d_resp_page_fault),
        .req_paddr       (mmu_d_resp_paddr),
        .req_size        (dmem_access_size),
        .req_is_fetch    (1'b0),
        .req_is_store    (dmem_req_is_store),
        .priv_mode       (csr_priv_mode_cur),
        .pmpcfg_entries  (csr_pmpcfg_entries),
        .pmpaddr_entries (csr_pmpaddr_entries),
        .access_fault    (dmem_access_fault)
    );

    assign dmem_load_access_fault  = dmem_access_fault &
                                     (mem_is_load_in | (mem_is_amo_in && (mem_amo_op_in == AMO_LR)));
    assign dmem_store_access_fault = dmem_access_fault & dmem_req_is_store;

    mem_stage u_mem_stage (
        .clk                (clk),
        .rst                (rst),
        .mem_alu_result      (mem_alu_result_in),
        .mem_branch_taken    (mem_branch_taken_in),
        .mem_branch_target   (mem_branch_target_in),
        .mem_rs1_rdata       (mem_rs1_rdata_in),
        .mem_rs2_rdata       (mem_rs2_rdata_in),
        .mem_fs2_rdata       (mem_fs2_rdata_in),
        .mem_rs1_addr        (mem_rs1_addr_in),
        .mem_pc              (mem_pc_in),
        .mem_imm             (mem_imm_in),
        .mem_instr           (mem_instr_in),
        .mem_instr_len       (mem_instr_len_in),
        .mem_is_load         (mem_is_load_in),
        .mem_is_store        (mem_is_store_in),
        .mem_mem_size        (mem_mem_size_in),
        .mem_is_amo          (mem_is_amo_in),
        .mem_amo_op          (mem_amo_op_in),
        .mem_is_fp           (mem_is_fp_in),
        .mem_fp_reg_write_en (mem_fp_reg_write_en_in),
        .mem_fp_store_data_sel(mem_fp_store_data_sel_in),
        .mem_fp_result       (mem_fp_result_in),
        .mem_fp_fflags       (mem_fp_fflags_in),
        .mem_reg_write_en    (mem_reg_write_en_in),
        .mem_rd_addr         (mem_rd_addr_in),
        .mem_wb_sel          (mem_wb_sel_in),
        .mem_is_csr          (mem_is_csr_in),
        .mem_csr_op          (mem_csr_op_in),
        .mem_illegal_instr   (mem_illegal_instr_in),
        .mem_instr_access_fault(mem_instr_access_fault_in),
        .mem_instr_page_fault(mem_instr_page_fault_in),
        .mem_is_ecall        (mem_is_ecall_in),
        .mem_is_ebreak       (mem_is_ebreak_in),
        .mem_is_mret         (mem_is_mret_in),
        .mem_is_sret         (mem_is_sret_in),
        .mem_is_wfi          (mem_is_wfi_in),
        .mem_is_sfence_vma   (mem_is_sfence_vma_in),
        .mem_is_fence        (mem_is_fence_in),
        .mem_is_fence_i      (mem_is_fence_i_in),
        .dmem_read_req       (raw_dmem_read_en),
        .dmem_write_req      (raw_dmem_write_en),
        .dmem_addr           (raw_dmem_addr),
        .dmem_wdata          (raw_dmem_wdata),
        .dmem_wstrb          (raw_dmem_wstrb),
        .dmem_req_ready      (data_translation_ready && !data_translation_page_fault &&
                              data_memory_port_available),
        .dmem_rsp_valid      (data_translation_ready && !data_translation_page_fault &&
                              data_memory_port_available),
        .dmem_rdata          (dmem_rdata),
        .dmem_load_access_fault(dmem_load_access_fault),
        .dmem_store_access_fault(dmem_store_access_fault),
        .dmem_load_page_fault(dmem_load_page_fault),
        .dmem_store_page_fault(dmem_store_page_fault),
        .mem_wb_alu_result   (mw_reg_alu_result_in),
        .mem_wb_mem_rdata    (mw_reg_mem_rdata_in),
        .mem_wb_fp_result    (mw_reg_fp_result_in),
        .mem_wb_fp_fflags    (mw_reg_fp_fflags_in),
        .mem_wb_pc           (mw_reg_pc_in),
        .mem_wb_instr        (mw_reg_instr_in),
        .mem_wb_instr_len    (mw_reg_instr_len_in),
        .mem_wb_reg_write_en (mw_reg_reg_write_en_in),
        .mem_wb_fp_reg_write_en(mw_reg_fp_reg_write_en_in),
        .mem_wb_rd_addr      (mw_reg_rd_addr_in),
        .mem_wb_wb_sel       (mw_reg_wb_sel_in),
        .mem_wb_is_csr       (mw_reg_is_csr_in),
        .mem_wb_csr_op       (mw_reg_csr_op_in),
        .mem_wb_rs1_rdata    (mw_reg_rs1_rdata_in),
        .mem_wb_rs1_addr     (mw_reg_rs1_addr_in),
        .mem_wb_imm          (mw_reg_imm_in),
        .mem_wb_illegal_instr(mw_reg_illegal_instr_in),
        .mem_wb_instr_access_fault(mw_reg_instr_access_fault_in),
        .mem_wb_instr_page_fault(mw_reg_instr_page_fault_in),
        .mem_wb_is_ecall     (mw_reg_is_ecall_in),
        .mem_wb_is_ebreak    (mw_reg_is_ebreak_in),
        .mem_wb_is_mret      (mw_reg_is_mret_in),
        .mem_wb_is_sret      (mw_reg_is_sret_in),
        .mem_wb_load_fault   (mw_reg_load_fault_in),
        .mem_wb_store_fault  (mw_reg_store_fault_in),
        .mem_wb_load_page_fault(mw_reg_load_page_fault_in),
        .mem_wb_store_page_fault(mw_reg_store_page_fault_in),
        .mem_wb_fault_addr   (mw_reg_fault_addr_in),
        .mem_stall_req       (mem_stall_req)
    );

    always_comb begin
        if (ptw_mem_req_valid) begin
            dmem_read_en  = 1'b1;
            dmem_write_en = 1'b0;
            dmem_addr     = ptw_mem_req_addr;
            dmem_wdata    = '0;
            dmem_size     = 3'b011;
        end else begin
            dmem_read_en  = raw_dmem_read_en && data_translation_ready &&
                            !data_translation_page_fault;
            dmem_write_en = raw_dmem_write_en && data_translation_ready &&
                            !data_translation_page_fault;
            dmem_addr     = data_access_needs_translation ? mmu_d_resp_paddr : raw_dmem_addr;
            dmem_wdata    = raw_dmem_wdata;
            dmem_size     = mem_mem_size_in;
        end
    end

    mem_wb_reg u_mem_wb_reg (
        .clk                 (clk),
        .rst                 (rst),
        .stall_en            (1'b0),
        .flush_en            (trap_flush_req | mem_stall_req),
        .mem_pc              (mw_reg_pc_in),
        .mem_alu_result      (mw_reg_alu_result_in),
        .mem_mem_rdata       (mw_reg_mem_rdata_in),
        .mem_fp_result       (mw_reg_fp_result_in),
        .mem_fp_fflags       (mw_reg_fp_fflags_in),
        .mem_instr           (mw_reg_instr_in),
        .mem_instr_len       (mw_reg_instr_len_in),
        .mem_reg_write_en    (mw_reg_reg_write_en_in),
        .mem_fp_reg_write_en (mw_reg_fp_reg_write_en_in),
        .mem_rd_addr         (mw_reg_rd_addr_in),
        .mem_wb_sel          (mw_reg_wb_sel_in),
        .mem_is_csr          (mw_reg_is_csr_in),
        .mem_csr_op          (mw_reg_csr_op_in),
        .mem_rs1_rdata       (mw_reg_rs1_rdata_in),
        .mem_rs1_addr        (mw_reg_rs1_addr_in),
        .mem_imm             (mw_reg_imm_in),
        .mem_illegal_instr   (mw_reg_illegal_instr_in),
        .mem_instr_access_fault(mw_reg_instr_access_fault_in),
        .mem_instr_page_fault(mw_reg_instr_page_fault_in),
        .mem_is_ecall        (mw_reg_is_ecall_in),
        .mem_is_ebreak       (mw_reg_is_ebreak_in),
        .mem_is_mret         (mw_reg_is_mret_in),
        .mem_is_sret         (mw_reg_is_sret_in),
        .mem_load_fault      (mw_reg_load_fault_in),
        .mem_store_fault     (mw_reg_store_fault_in),
        .mem_load_page_fault (mw_reg_load_page_fault_in),
        .mem_store_page_fault(mw_reg_store_page_fault_in),
        .mem_fault_addr      (mw_reg_fault_addr_in),
        .wb_pc               (wb_pc_in),
        .wb_alu_result       (wb_alu_result_in),
        .wb_mem_rdata        (wb_mem_rdata_in),
        .wb_fp_result        (wb_fp_result_in),
        .wb_fp_fflags        (wb_fp_fflags_in),
        .wb_instr            (wb_instr_in),
        .wb_instr_len        (wb_instr_len_in),
        .wb_reg_write_en     (wb_reg_write_en_in),
        .wb_fp_reg_write_en  (wb_fp_reg_write_en_in),
        .wb_rd_addr          (wb_rd_addr_in),
        .wb_wb_sel           (wb_wb_sel_in),
        .wb_is_csr           (wb_is_csr_in),
        .wb_csr_op           (wb_csr_op_in),
        .wb_rs1_rdata        (wb_rs1_rdata_in),
        .wb_rs1_addr         (wb_rs1_addr_in),
        .wb_imm              (wb_imm_in),
        .wb_illegal_instr    (wb_illegal_instr_in),
        .wb_instr_access_fault(wb_instr_access_fault),
        .wb_instr_page_fault (wb_instr_page_fault),
        .wb_is_ecall         (wb_is_ecall_in),
        .wb_is_ebreak        (wb_is_ebreak_in),
        .wb_is_mret          (wb_is_mret_in),
        .wb_is_sret          (wb_is_sret_in),
        .wb_load_fault       (wb_load_fault),
        .wb_store_fault      (wb_store_fault),
        .wb_load_page_fault  (wb_load_page_fault),
        .wb_store_page_fault (wb_store_page_fault),
        .wb_fault_addr       (wb_fault_addr_in)
    );

    // ---------------------------------------------------------
    // (F) WB Stage - Write Back & System Interaction
    // ---------------------------------------------------------
    wb_stage u_wb_stage (
        .mem_wb_alu_result  (wb_alu_result_in),
        .mem_wb_mem_rdata   (wb_mem_rdata_in),
        .mem_wb_fp_result   (wb_fp_result_in),
        .mem_wb_fp_fflags   (wb_fp_fflags_in),
        .mem_wb_pc          (wb_pc_in),
        .mem_wb_pc_plus_4   (wb_pc_in + {{(XLEN-3){1'b0}}, wb_instr_len_in}),
        .mem_wb_reg_write_en(wb_reg_write_en_in),
        .mem_wb_fp_reg_write_en(wb_fp_reg_write_en_in),
        .mem_wb_rd_addr     (wb_rd_addr_in),
        .mem_wb_wb_sel      (wb_wb_sel_in),
        .mem_wb_is_csr      (wb_is_csr_in),
        .mem_wb_csr_op      (wb_csr_op_in),
        .mem_wb_rs1_rdata   (wb_rs1_rdata_in),
        .mem_wb_rs1_addr    (wb_rs1_addr_in),
        .mem_wb_imm         (wb_imm_in),
        .mem_wb_instr       (wb_instr_in),
        .mem_wb_illegal_instr(wb_illegal_instr_in),
        .mem_wb_instr_access_fault(wb_instr_access_fault),
        .mem_wb_instr_page_fault(wb_instr_page_fault),
        .mem_wb_is_ecall    (wb_is_ecall_in),
        .mem_wb_is_ebreak   (wb_is_ebreak_in),
        .mem_wb_is_mret     (wb_is_mret_in),
        .mem_wb_is_sret     (wb_is_sret_in),
        .mem_wb_load_fault  (wb_load_fault),
        .mem_wb_store_fault (wb_store_fault),
        .mem_wb_load_page_fault(wb_load_page_fault),
        .mem_wb_store_page_fault(wb_store_page_fault),
        .mem_wb_fault_addr  (wb_fault_addr_in),
        .csr_rdata          (csr_rdata),
        .csr_illegal_access (csr_illegal_access),
        .wb_rd_wdata        (wb_rd_wdata),
        .wb_rd_addr         (wb_rd_addr),
        .wb_reg_write_en    (wb_reg_write_en_out),
        .wb_fd_wdata        (wb_fd_wdata),
        .wb_fd_addr         (wb_fd_addr),
        .wb_fp_write_en     (wb_fp_write_en),
        .wb_fp_fflags       (wb_fp_fflags),
        .wb_fp_state_dirty  (wb_fp_state_dirty),
        .wb_fp_fflags_valid (wb_fp_fflags_valid),
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
        .trap_instr_access_fault(trap_instr_access_fault),
        .trap_instr_page_fault(trap_instr_page_fault),
        .trap_load_fault    (trap_load_fault),
        .trap_store_fault   (trap_store_fault),
        .trap_load_page_fault(trap_load_page_fault),
        .trap_store_page_fault(trap_store_page_fault),
        .trap_bad_addr      (trap_bad_addr),
        .trap_bad_instr     (trap_bad_instr)
    );

    csr_unit u_csr_unit (
        .clk                (clk),
        .rst                (rst),
        .csr_req            (csr_req),
        .csr_op             (csr_op),
        .csr_addr           (csr_addr),
        .csr_rs1_data       (csr_rs1_data),
        .trap_ctrl_trigger  (trap_ctrl_trigger),
        .trap_ctrl_to_s_mode(trap_ctrl_to_s_mode),
        .trap_ctrl_mepc     (trap_ctrl_mepc),
        .trap_ctrl_mcause   (trap_ctrl_mcause),
        .trap_ctrl_mtval    (trap_ctrl_mtval),
        .trap_ctrl_mret_en  (trap_ctrl_mret_en),
        .trap_ctrl_sret_en  (trap_ctrl_sret_en),
        .m_timer_interrupt(m_timer_interrupt),
        .m_software_interrupt(m_software_interrupt),
        .m_external_interrupt(m_external_interrupt),
        .s_timer_interrupt  (s_timer_interrupt),
        .s_software_interrupt(s_software_interrupt),
        .s_external_interrupt(s_external_interrupt),
        .fp_state_dirty     (wb_fp_state_dirty),
        .fp_fflags_valid    (wb_fp_fflags_valid),
        .fp_fflags          (wb_fp_fflags),
        .csr_rdata          (csr_rdata),
        .csr_mstatus        (csr_mstatus_value),
        .csr_sstatus        (csr_sstatus_value),
        .csr_medeleg        (csr_medeleg_value),
        .csr_mideleg        (csr_mideleg_value),
        .csr_mie            (csr_mie_value),
        .csr_sie            (csr_sie_value),
        .csr_mip            (csr_mip_value),
        .csr_sip            (csr_sip_value),
        .csr_mtvec          (csr_mtvec_base),
        .csr_stvec          (csr_stvec_base),
        .csr_mepc           (csr_mepc_value),
        .csr_sepc           (csr_sepc_value),
        .csr_satp           (csr_satp_value),
        .csr_frm            (csr_frm_value),
        .csr_priv_mode          (csr_priv_mode_cur),
        .csr_trap_vector        (),
        .csr_trap_to_s_mode     (),
        .csr_trap_vector_next   (csr_trap_vector_value),
        .csr_trap_to_s_mode_next(csr_trap_to_s_mode),
        .csr_pmpcfg_entries     (csr_pmpcfg_entries),
        .csr_pmpaddr_entries    (csr_pmpaddr_entries),
        .csr_illegal_access     (csr_illegal_access)
    );

    trap_controller u_trap_ctrl (
        .wb_pc               (wb_pc_in),
        .trap_illegal_instr  (trap_illegal_instr),
        .trap_is_ecall       (trap_is_ecall),
        .trap_is_ebreak      (trap_is_ebreak),
        .trap_is_mret        (trap_is_mret),
        .trap_is_sret        (trap_is_sret),
        .trap_instr_access_fault(trap_instr_access_fault),
        .trap_load_fault     (trap_load_fault),
        .trap_store_fault    (trap_store_fault),
        .trap_instr_page_fault(trap_instr_page_fault),
        .trap_load_page_fault(trap_load_page_fault),
        .trap_store_page_fault(trap_store_page_fault),
        .trap_bad_addr       (trap_bad_addr),
        .trap_bad_instr      (trap_bad_instr),
        .csr_mstatus_mie     (csr_mstatus_mie),
        .csr_mie             (csr_mie_value),
        .csr_sie             (csr_sie_value),
        .csr_mip             (csr_mip_value),
        .csr_sip             (csr_sip_value),
        .csr_mstatus         (csr_mstatus_value),
        .csr_mtvec           (csr_mtvec_base),
        .csr_stvec           (csr_stvec_base),
        .csr_mepc            (csr_mepc_value),
        .csr_sepc            (csr_sepc_value),
        .csr_medeleg         (csr_medeleg_value),
        .csr_mideleg         (csr_mideleg_value),
        .csr_priv_mode       (csr_priv_mode_cur),
        .trap_trigger        (trap_ctrl_trigger),
        .trap_mepc           (trap_ctrl_mepc),
        .trap_mcause         (trap_ctrl_mcause),
        .trap_mtval          (trap_ctrl_mtval),
        .trap_to_s_mode      (trap_ctrl_to_s_mode),
        .mret_en             (trap_ctrl_mret_en),
        .sret_en             (trap_ctrl_sret_en),
        .trap_flush_req      (trap_flush_req),
        .trap_pc_sel         (trap_pc_sel_raw),
        .trap_next_pc        (trap_next_pc_raw)
    );

    assign trap_pc_sel = trap_pc_sel_raw;
    assign trap_next_pc = trap_next_pc_raw;

endmodule
