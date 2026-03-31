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
    // Inputs from EX/MEM pipeline register
    input  logic [XLEN-1:0]        mem_alu_result,    // Used as memory address or ALU result
    input  logic                   mem_branch_taken,
    input  logic [XLEN-1:0]        mem_branch_target,
    input  logic [XLEN-1:0]        mem_rs1_rdata,
    input  logic [XLEN-1:0]        mem_rs2_rdata,     // Store data
    input  logic [4:0]             mem_rs1_addr,
    input  logic [XLEN-1:0]        mem_pc,
    input  logic [XLEN-1:0]        mem_imm,
    input  logic [INST_WIDTH-1:0]  mem_instr,

    // Memory Control
    input  logic                   mem_is_load,
    input  logic                   mem_is_store,
    input  logic [2:0]             mem_mem_size,      // LB/LH/LW/LD/LBU/LHU/LWU

    // Writeback Control
    input  logic                   mem_reg_write_en,
    input  logic [4:0]             mem_rd_addr,
    input  logic [1:0]             mem_wb_sel,

    // Exceptions and System (bypassed through or generated)
    input  logic                   mem_is_csr,
    input  logic [2:0]             mem_csr_op,
    input  logic                   mem_illegal_instr,
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

    // Outputs to MEM/WB Pipeline Register
    output logic [XLEN-1:0]        mem_wb_alu_result,
    output logic [XLEN-1:0]        mem_wb_mem_rdata,
    output logic [XLEN-1:0]        mem_wb_pc,
    output logic [INST_WIDTH-1:0]  mem_wb_instr,

    output logic                   mem_wb_reg_write_en,
    output logic [4:0]             mem_wb_rd_addr,
    output logic [1:0]             mem_wb_wb_sel,

    output logic                   mem_wb_is_csr,
    output logic [2:0]             mem_wb_csr_op,
    output logic [XLEN-1:0]        mem_wb_rs1_rdata,
    output logic [4:0]             mem_wb_rs1_addr,
    output logic [XLEN-1:0]        mem_wb_imm,

    output logic                   mem_wb_illegal_instr,
    output logic                   mem_wb_is_ecall,
    output logic                   mem_wb_is_ebreak,
    output logic                   mem_wb_is_mret,
    output logic                   mem_wb_is_sret,
    output logic                   mem_wb_load_fault,
    output logic                   mem_wb_store_fault,
    output logic [XLEN-1:0]        mem_wb_fault_addr,

    // Control to Hazard Unit / Stall Logic
    output logic                   mem_stall_req      // Stall pipeline if D-Cache doesn't respond
);

    logic [XLEN-1:0] load_data_aligned;
    logic [XLEN-1:0] store_data_aligned;
    logic [7:0]      write_strobe;
    logic            load_addr_misaligned;
    logic            store_addr_misaligned;

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

    assign load_addr_misaligned  = mem_is_load  && addr_misaligned(mem_mem_size, mem_alu_result);
    assign store_addr_misaligned = mem_is_store && addr_misaligned(mem_mem_size, mem_alu_result);

    // --------------------------
    // Memory Request Logic
    assign dmem_read_req  = mem_is_load  & ~load_addr_misaligned;
    assign dmem_write_req = mem_is_store & ~store_addr_misaligned;
    assign dmem_addr      = mem_alu_result; // ALU result is the computed address

    // --------------------------
    // Stall generation: Stall if memory request is not yet ready or valid
    // For a simple synchronous SRAM, this might be 0, but for cache it will depend on hits/misses
    // --------------------------
    always_comb begin
        if ((mem_is_load || mem_is_store) && (!dmem_req_ready || (mem_is_load && !dmem_rsp_valid))) begin
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

        // Mem address lower 3 bits determine the alignment within the 64-bit word
        align_offset = mem_alu_result[2:0];

        if (mem_is_store) begin
            case (mem_mem_size)
                // SB: Store Byte
                3'b000: begin
                    store_data_aligned = {56'b0, mem_rs2_rdata[7:0]} << (align_offset * 8);
                    write_strobe       = 8'b0000_0001 << align_offset;
                end
                // SH: Store Halfword
                3'b001: begin
                    store_data_aligned = {48'b0, mem_rs2_rdata[15:0]} << (align_offset * 8);
                    write_strobe       = 8'b0000_0011 << align_offset;
                end
                // SW: Store Word
                3'b010: begin
                    store_data_aligned = {32'b0, mem_rs2_rdata[31:0]} << (align_offset * 8);
                    write_strobe       = 8'b0000_1111 << align_offset;
                end
                // SD: Store Doubleword
                3'b011: begin
                    store_data_aligned = mem_rs2_rdata;
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

    // --------------------------
    // Pass Output Signals to WB Stage
    // --------------------------
    assign mem_wb_alu_result    = mem_alu_result;
    assign mem_wb_mem_rdata     = load_data_aligned;
    assign mem_wb_pc            = mem_pc;
    assign mem_wb_instr         = mem_instr;

    assign mem_wb_reg_write_en  = mem_reg_write_en;
    assign mem_wb_rd_addr       = mem_rd_addr;
    assign mem_wb_wb_sel        = mem_wb_sel;

    assign mem_wb_is_csr        = mem_is_csr;
    assign mem_wb_csr_op        = mem_csr_op;
    assign mem_wb_rs1_rdata     = mem_rs1_rdata;
    assign mem_wb_rs1_addr      = mem_rs1_addr;
    assign mem_wb_imm           = mem_imm;

    assign mem_wb_illegal_instr = mem_illegal_instr;
    assign mem_wb_is_ecall      = mem_is_ecall;
    assign mem_wb_is_ebreak     = mem_is_ebreak;
    assign mem_wb_is_mret       = mem_is_mret;
    assign mem_wb_is_sret       = mem_is_sret;
    assign mem_wb_load_fault    = load_addr_misaligned;
    assign mem_wb_store_fault   = store_addr_misaligned;
    assign mem_wb_fault_addr    = (load_addr_misaligned | store_addr_misaligned) ? mem_alu_result : '0;

endmodule
