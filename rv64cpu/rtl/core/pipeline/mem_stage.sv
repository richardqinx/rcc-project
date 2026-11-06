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

// File path: rtl/core/mem_stage.sv
// Description: Memory Stage
//              Latches results from the EX stage and bypassed control signals from ID/EX.
//              Supports stall and flush operations.

module mem_stage
    import rv64_pkg::*;
(
    input  logic                   clk,
    input  logic                   rst,

    // Inputs from EX/MEM pipeline register
    input  logic [XLEN-1:0]        mem_alu_result,    // Used as memory address or ALU result
    input  logic                   mem_branch_taken,
    input  logic [XLEN-1:0]        mem_branch_target,
    input  logic [XLEN-1:0]        mem_rs1_rdata,
    input  logic [XLEN-1:0]        mem_rs2_rdata,     // Store data
    input  logic [XLEN-1:0]        mem_fs2_rdata,
    input  logic [4:0]             mem_rs1_addr,
    input  logic [XLEN-1:0]        mem_pc,
    input  logic [XLEN-1:0]        mem_imm,
    input  logic [INST_WIDTH-1:0]  mem_instr,
    input  logic [2:0]             mem_instr_len,

    // Memory Control
    input  logic                   mem_is_load,
    input  logic                   mem_is_store,
    input  logic [2:0]             mem_mem_size,      // LB/LH/LW/LD/LBU/LHU/LWU
    input  logic                   mem_is_amo,
    input  amo_op_t                mem_amo_op,
    input  logic                   mem_is_fp,
    input  logic                   mem_fp_reg_write_en,
    input  logic                   mem_fp_store_data_sel,
    input  logic [XLEN-1:0]        mem_fp_result,
    input  logic [4:0]             mem_fp_fflags,

    // Writeback Control
    input  logic                   mem_reg_write_en,
    input  logic [4:0]             mem_rd_addr,
    input  logic [1:0]             mem_wb_sel,

    // Exceptions and System (bypassed through or generated)
    input  logic                   mem_is_csr,
    input  logic [2:0]             mem_csr_op,
    input  logic                   mem_illegal_instr,
    input  logic                   mem_instr_access_fault,
    input  logic                   mem_instr_page_fault,
    input  logic                   mem_is_ecall,
    input  logic                   mem_is_ebreak,
    input  logic                   mem_is_mret,
    input  logic                   mem_is_sret,
    input  logic                   mem_is_wfi,
    input  logic                   mem_is_sfence_vma,
    input  logic                   mem_is_fence,
    input  logic                   mem_is_fence_i,

    // Data Memory Interface (to Data Cache / Memory Controller)
    output logic                   dmem_read_req,
    output logic                   dmem_write_req,
    output logic [XLEN-1:0]        dmem_addr,
    output logic [XLEN-1:0]        dmem_wdata,
    output logic [7:0]             dmem_wstrb,        // Write strobe depending on size
    input  logic                   dmem_req_ready,    // Wait signal from D-Cache
    input  logic                   dmem_rsp_valid,    // Data valid from D-Cache
    input  logic [XLEN-1:0]        dmem_rdata,
    input  logic                   dmem_load_access_fault,
    input  logic                   dmem_store_access_fault,
    input  logic                   dmem_load_page_fault,
    input  logic                   dmem_store_page_fault,

    // Outputs to MEM/WB Pipeline Register
    output logic [XLEN-1:0]        mem_wb_alu_result,
    output logic [XLEN-1:0]        mem_wb_mem_rdata,
    output logic [XLEN-1:0]        mem_wb_fp_result,
    output logic [4:0]             mem_wb_fp_fflags,
    output logic [XLEN-1:0]        mem_wb_pc,
    output logic [INST_WIDTH-1:0]  mem_wb_instr,
    output logic [2:0]             mem_wb_instr_len,

    output logic                   mem_wb_reg_write_en,
    output logic                   mem_wb_fp_reg_write_en,
    output logic [4:0]             mem_wb_rd_addr,
    output logic [1:0]             mem_wb_wb_sel,

    output logic                   mem_wb_is_csr,
    output logic [2:0]             mem_wb_csr_op,
    output logic [XLEN-1:0]        mem_wb_rs1_rdata,
    output logic [4:0]             mem_wb_rs1_addr,
    output logic [XLEN-1:0]        mem_wb_imm,

    output logic                   mem_wb_illegal_instr,
    output logic                   mem_wb_instr_access_fault,
    output logic                   mem_wb_instr_page_fault,
    output logic                   mem_wb_is_ecall,
    output logic                   mem_wb_is_ebreak,
    output logic                   mem_wb_is_mret,
    output logic                   mem_wb_is_sret,
    output logic                   mem_wb_load_fault,
    output logic                   mem_wb_store_fault,
    output logic                   mem_wb_load_page_fault,
    output logic                   mem_wb_store_page_fault,
    output logic [XLEN-1:0]        mem_wb_fault_addr,

    // Control to Hazard Unit / Stall Logic
    output logic                   mem_stall_req      // Stall pipeline if D-Cache doesn't respond
);

    logic [XLEN-1:0] load_data_aligned;
    logic [XLEN-1:0] store_data_aligned;
    logic [XLEN-1:0] amo_write_data;
    logic [XLEN-1:0] amo_wb_data;
    logic [XLEN-1:0] selected_store_data;
    logic [XLEN-1:0] fp_load_data;
    logic [7:0]      write_strobe;
    logic            load_addr_misaligned;
    logic            store_addr_misaligned;
    logic            amo_is_lr;
    logic            amo_is_sc;
    logic            amo_is_word;
    logic            amo_is_double;
    logic            amo_reads_memory;
    logic            amo_writes_memory;
    logic            amo_store_access;
    logic            amo_load_access;
    logic            amo_addr_misaligned;
    logic            amo_sc_success;
    logic            amo_rmw_op;
    logic            amo_rmw_read_phase;
    logic            amo_rmw_write_phase;
    logic [XLEN-1:0] amo_memory_rdata;
    logic            amo_rmw_busy_q;
    logic [XLEN-1:0] amo_rmw_old_data_q;
    logic            reservation_valid_q;
    logic [XLEN-1:0] reservation_addr_q;

    function automatic logic addr_misaligned(
        input logic [2:0]      size,
        input logic [XLEN-1:0] addr
    );
        logic result;
        case (size)
            3'b000, 3'b100: result = 1'b0;         // Byte L/S
            3'b001, 3'b101: result = addr[0];      // Halfword requires bit0 = 0
            3'b010, 3'b110: result = |addr[1:0];   // Word requires bits[1:0] = 0
            3'b011:          result = |addr[2:0];   // Doubleword requires bits[2:0] = 0
            default:         result = 1'b0;
        endcase
        return result;
    endfunction

    assign amo_is_lr          = mem_is_amo && (mem_amo_op == AMO_LR);
    assign amo_is_sc          = mem_is_amo && (mem_amo_op == AMO_SC);
    assign amo_is_word        = mem_is_amo && (mem_mem_size == 3'b010);
    assign amo_is_double      = mem_is_amo && (mem_mem_size == 3'b011);
    assign amo_rmw_op         = mem_is_amo && !amo_is_lr && !amo_is_sc;
    assign amo_rmw_read_phase = amo_rmw_op && !amo_rmw_busy_q;
    assign amo_rmw_write_phase = amo_rmw_op && amo_rmw_busy_q;
    assign amo_reads_memory   = amo_is_lr || amo_rmw_read_phase;
    assign amo_store_access   = mem_is_amo && !amo_is_lr;
    assign amo_load_access    = mem_is_amo && amo_is_lr;
    assign amo_addr_misaligned = mem_is_amo && addr_misaligned(mem_mem_size, mem_alu_result);
    assign amo_sc_success     = amo_is_sc && reservation_valid_q &&
                                (reservation_addr_q == mem_alu_result) &&
                                !amo_addr_misaligned && !dmem_store_access_fault &&
                                !dmem_store_page_fault && !mem_instr_access_fault &&
                                !mem_instr_page_fault;
    assign amo_writes_memory  = (amo_is_sc && amo_sc_success) || amo_rmw_write_phase;
    assign amo_memory_rdata   = amo_rmw_busy_q ? amo_rmw_old_data_q : dmem_rdata;

    assign load_addr_misaligned  = mem_is_load && addr_misaligned(mem_mem_size, mem_alu_result);
    assign store_addr_misaligned = (mem_is_store && addr_misaligned(mem_mem_size, mem_alu_result)) |
                                   (amo_store_access && amo_addr_misaligned);

    always_ff @(posedge clk) begin
        if (rst) begin
            amo_rmw_busy_q      <= 1'b0;
            amo_rmw_old_data_q  <= '0;
            reservation_valid_q <= 1'b0;
            reservation_addr_q  <= '0;
        end else begin
            if (amo_rmw_read_phase && dmem_read_req && dmem_rsp_valid && !dmem_load_access_fault &&
                !dmem_load_page_fault && !mem_instr_access_fault && !mem_instr_page_fault) begin
                amo_rmw_busy_q     <= 1'b1;
                amo_rmw_old_data_q <= dmem_rdata;
            end else if (amo_rmw_write_phase && (!dmem_write_req || dmem_req_ready)) begin
                amo_rmw_busy_q     <= 1'b0;
                amo_rmw_old_data_q <= '0;
            end else if (!mem_is_amo) begin
                amo_rmw_busy_q     <= 1'b0;
                amo_rmw_old_data_q <= '0;
            end

            if (amo_is_lr && dmem_read_req && dmem_rsp_valid && !dmem_load_access_fault &&
                !dmem_load_page_fault) begin
                reservation_valid_q <= 1'b1;
                reservation_addr_q  <= mem_alu_result;
            end else if (amo_is_sc && dmem_req_ready) begin
                reservation_valid_q <= 1'b0;
                reservation_addr_q  <= '0;
            end
        end
    end

    // --------------------------
    // Memory Request Logic
    assign dmem_read_req  = (mem_is_load | amo_reads_memory) &
                            ~(load_addr_misaligned | (amo_reads_memory & amo_addr_misaligned)) &
                            ~dmem_load_access_fault & ~dmem_load_page_fault &
                            ~mem_instr_access_fault & ~mem_instr_page_fault;
    assign dmem_write_req = (mem_is_store | amo_writes_memory) & ~store_addr_misaligned & ~dmem_store_access_fault &
                            ~dmem_store_page_fault & ~mem_instr_access_fault & ~mem_instr_page_fault;
    assign dmem_addr      = mem_alu_result; // ALU result is the computed address

    // --------------------------
    // Stall generation: Stall if memory request is not yet ready or valid
    // For a simple synchronous SRAM, this might be 0, but for cache it will depend on hits/misses
    // --------------------------
    always_comb begin
        if (amo_rmw_read_phase && dmem_read_req) begin
            mem_stall_req = 1'b1;
        end else if ((dmem_read_req || dmem_write_req) && (!dmem_req_ready || (dmem_read_req && !dmem_rsp_valid))) begin
            mem_stall_req = 1'b1;
        end else begin
            mem_stall_req = 1'b0;
        end
    end

    // --------------------------
    // Store Data Alignment and Strobe Generation
    // --------------------------
    always_comb begin
        logic [2:0] align_offset;
        store_data_aligned = '0;
        write_strobe       = 8'b0000_0000;
        selected_store_data = mem_fp_store_data_sel ? mem_fs2_rdata : mem_rs2_rdata;

        // Mem address lower 3 bits determine the alignment within the 64-bit word
        align_offset = mem_alu_result[2:0];

        if (mem_is_store || amo_writes_memory) begin
            case (mem_mem_size)
                // SB: Store Byte
                3'b000: begin
                    store_data_aligned = {56'b0, selected_store_data[7:0]} << (align_offset * 8);
                    write_strobe       = 8'b0000_0001 << align_offset;
                end
                // SH: Store Halfword
                3'b001: begin
                    store_data_aligned = {48'b0, selected_store_data[15:0]} << (align_offset * 8);
                    write_strobe       = 8'b0000_0011 << align_offset;
                end
                // SW: Store Word
                3'b010: begin
                    store_data_aligned = {32'b0, amo_writes_memory ? amo_write_data[31:0] :
                                          selected_store_data[31:0]} << (align_offset * 8);
                    write_strobe       = 8'b0000_1111 << align_offset;
                end
                // SD: Store Doubleword
                3'b011: begin
                    store_data_aligned = amo_writes_memory ? amo_write_data : selected_store_data;
                    write_strobe       = 8'b1111_1111;
                end
                default: begin
                    store_data_aligned = '0;
                    write_strobe       = 8'b0000_0000;
                end
            endcase
        end
    end

    assign dmem_wdata = store_data_aligned;
    assign dmem_wstrb = write_strobe;

    // --------------------------
    // Load Data Alignment and Sign/Zero Extension
    // --------------------------
    always_comb begin
        logic [2:0] align_offset;
        logic [XLEN-1:0] shifted_rdata;
        load_data_aligned = '0;

        align_offset = mem_alu_result[2:0];

        // Shift data down so the target bytes are at the bottom
        shifted_rdata = dmem_rdata >> (align_offset * 8);

        case (mem_mem_size)
            // LB: Load Byte (Sign extend)
            3'b000: load_data_aligned = { {56{shifted_rdata[7]}}, shifted_rdata[7:0] };
            // LH: Load Halfword (Sign extend)
            3'b001: load_data_aligned = { {48{shifted_rdata[15]}}, shifted_rdata[15:0] };
            // LW: Load Word (Sign extend)
            3'b010: load_data_aligned = { {32{shifted_rdata[31]}}, shifted_rdata[31:0] };
            // LD: Load Doubleword
            3'b011: load_data_aligned = shifted_rdata;
            // LBU: Load Byte Unsigned (Zero extend)
            3'b100: load_data_aligned = { 56'b0, shifted_rdata[7:0] };
            // LHU: Load Half Unsigned (Zero extend)
            3'b101: load_data_aligned = { 48'b0, shifted_rdata[15:0] };
            // LWU: Load Word Unsigned (Zero extend)
            3'b110: load_data_aligned = { 32'b0, shifted_rdata[31:0] };
            default: load_data_aligned = '0;
        endcase
    end

    always_comb begin
        fp_load_data = load_data_aligned;
        if (mem_is_fp && (mem_mem_size == 3'b010)) begin
            fp_load_data = {32'hFFFF_FFFF, load_data_aligned[31:0]};
        end
    end

    always_comb begin
        logic [2:0]        align_offset;
        logic [XLEN-1:0]   shifted_rdata;
        logic [31:0]       amo_old_word;
        logic [31:0]       amo_rs2_word;
        logic [31:0]       amo_result_word;
        logic signed [31:0] amo_old_word_signed;
        logic signed [31:0] amo_rs2_word_signed;
        logic signed [XLEN-1:0] amo_old_double_signed;
        logic signed [XLEN-1:0] amo_rs2_double_signed;

        align_offset = mem_alu_result[2:0];
        shifted_rdata = amo_memory_rdata >> (align_offset * 8);
        amo_old_word = shifted_rdata[31:0];
        amo_rs2_word = mem_rs2_rdata[31:0];
        amo_old_word_signed = amo_old_word;
        amo_rs2_word_signed = amo_rs2_word;
        amo_old_double_signed = amo_memory_rdata;
        amo_rs2_double_signed = mem_rs2_rdata;
        amo_result_word = amo_old_word;
        amo_write_data = mem_rs2_rdata;
        amo_wb_data = '0;

        if (amo_is_word) begin
            unique case (mem_amo_op)
                AMO_SWAP, AMO_SC: amo_result_word = amo_rs2_word;
                AMO_ADD:          amo_result_word = amo_old_word + amo_rs2_word;
                AMO_XOR:          amo_result_word = amo_old_word ^ amo_rs2_word;
                AMO_AND:          amo_result_word = amo_old_word & amo_rs2_word;
                AMO_OR:           amo_result_word = amo_old_word | amo_rs2_word;
                AMO_MIN:          amo_result_word = (amo_old_word_signed < amo_rs2_word_signed) ?
                                                     amo_old_word : amo_rs2_word;
                AMO_MAX:          amo_result_word = (amo_old_word_signed > amo_rs2_word_signed) ?
                                                     amo_old_word : amo_rs2_word;
                AMO_MINU:         amo_result_word = (amo_old_word < amo_rs2_word) ? amo_old_word : amo_rs2_word;
                AMO_MAXU:         amo_result_word = (amo_old_word > amo_rs2_word) ? amo_old_word : amo_rs2_word;
                default:          amo_result_word = amo_old_word;
            endcase
            amo_write_data = {{32{amo_result_word[31]}}, amo_result_word};
            amo_wb_data = amo_is_sc ? {63'b0, ~amo_sc_success} :
                          {{32{amo_old_word[31]}}, amo_old_word};
        end else if (amo_is_double) begin
            unique case (mem_amo_op)
                AMO_SWAP, AMO_SC: amo_write_data = mem_rs2_rdata;
                AMO_ADD:          amo_write_data = amo_memory_rdata + mem_rs2_rdata;
                AMO_XOR:          amo_write_data = amo_memory_rdata ^ mem_rs2_rdata;
                AMO_AND:          amo_write_data = amo_memory_rdata & mem_rs2_rdata;
                AMO_OR:           amo_write_data = amo_memory_rdata | mem_rs2_rdata;
                AMO_MIN:          amo_write_data = (amo_old_double_signed < amo_rs2_double_signed) ?
                                                   amo_memory_rdata : mem_rs2_rdata;
                AMO_MAX:          amo_write_data = (amo_old_double_signed > amo_rs2_double_signed) ?
                                                   amo_memory_rdata : mem_rs2_rdata;
                AMO_MINU:         amo_write_data = (amo_memory_rdata < mem_rs2_rdata) ? amo_memory_rdata :
                                                   mem_rs2_rdata;
                AMO_MAXU:         amo_write_data = (amo_memory_rdata > mem_rs2_rdata) ? amo_memory_rdata :
                                                   mem_rs2_rdata;
                default:          amo_write_data = amo_memory_rdata;
            endcase
            amo_wb_data = amo_is_sc ? {63'b0, ~amo_sc_success} : amo_memory_rdata;
        end
    end

    // --------------------------
    // Pass Output Signals to WB Stage
    // --------------------------
    assign mem_wb_alu_result    = mem_alu_result;
    assign mem_wb_mem_rdata     = mem_is_amo ? amo_wb_data : load_data_aligned;
    assign mem_wb_fp_result     = mem_is_load ? fp_load_data : mem_fp_result;
    assign mem_wb_fp_fflags     = mem_fp_fflags;
    assign mem_wb_pc            = mem_pc;
    assign mem_wb_instr         = mem_instr;
    assign mem_wb_instr_len     = mem_instr_len;

    assign mem_wb_reg_write_en  = mem_reg_write_en;
    assign mem_wb_fp_reg_write_en = mem_fp_reg_write_en;
    assign mem_wb_rd_addr       = mem_rd_addr;
    assign mem_wb_wb_sel        = mem_wb_sel;

    assign mem_wb_is_csr        = mem_is_csr;
    assign mem_wb_csr_op        = mem_csr_op;
    assign mem_wb_rs1_rdata     = mem_rs1_rdata;
    assign mem_wb_rs1_addr      = mem_rs1_addr;
    assign mem_wb_imm           = mem_imm;

    assign mem_wb_illegal_instr      = mem_illegal_instr;
    assign mem_wb_instr_access_fault = mem_instr_access_fault;
    assign mem_wb_instr_page_fault   = mem_instr_page_fault;
    assign mem_wb_is_ecall           = mem_is_ecall;
    assign mem_wb_is_ebreak          = mem_is_ebreak;
    assign mem_wb_is_mret            = mem_is_mret;
    assign mem_wb_is_sret            = mem_is_sret;
    assign mem_wb_load_fault         = load_addr_misaligned | (amo_load_access & amo_addr_misaligned) |
                                       dmem_load_access_fault;
    assign mem_wb_store_fault        = store_addr_misaligned | dmem_store_access_fault;
    assign mem_wb_load_page_fault    = dmem_load_page_fault;
    assign mem_wb_store_page_fault   = dmem_store_page_fault;
    assign mem_wb_fault_addr         = (mem_instr_access_fault | mem_instr_page_fault) ? mem_pc :
                                       ((dmem_load_page_fault | dmem_store_page_fault) ? mem_alu_result :
                                       ((load_addr_misaligned | store_addr_misaligned |
                                         dmem_load_access_fault | dmem_store_access_fault) ? mem_alu_result : '0));

endmodule
