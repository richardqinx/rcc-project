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

// File path: rtl/core/hazard_unit.sv
// Description: Hazard Control Unit
// Handles data hazards (forwarding, Load-Use stalls), control hazards
// (branch misprediction flushes), and memory stalls (D-Cache misses).

module hazard_unit
    import rv64_pkg::*;
(
    // -----------------------
    // Inputs from ID Stage (for Load-Use detection)
    // -----------------------
    input  logic [4:0]  id_rs1_addr,
    input  logic [4:0]  id_rs2_addr,

    // -----------------------
    // Inputs from EX Stage (ID/EX register outputs)
    // -----------------------
    input  logic [4:0]  ex_rs1_addr,
    input  logic [4:0]  ex_rs2_addr,
    input  logic [4:0]  ex_rd_addr,
    input  logic        ex_reg_write_en,
    input  logic        ex_is_load,
    input  logic        ex_branch_taken,

    // -----------------------
    // Inputs from MEM Stage (EX/MEM register outputs)
    // -----------------------
    input  logic [4:0]  mem_rd_addr,
    input  logic        mem_reg_write_en,
    input  logic        mem_stall_req,       // D-Cache miss or memory busy

    // -----------------------
    // Inputs from WB Stage (MEM/WB register outputs)
    // -----------------------
    input  logic [4:0]  wb_rd_addr,
    input  logic        wb_reg_write_en,

    // -----------------------
    // Outputs: EX-stage operand forwarding mux selects
    // -----------------------
    // 00: no forward (use regfile), 01: forward from MEM, 10: forward from WB
    output logic [1:0]  forward_a_sel,
    output logic [1:0]  forward_b_sel,

    // -----------------------
    // Outputs: Pipeline stall and flush controls
    // -----------------------
    output logic        if_stall_en,
    output logic        if_id_stall_en,
    output logic        id_ex_stall_en,
    output logic        ex_mem_stall_en,
    output logic        if_id_flush_en,
    output logic        id_ex_flush_en
);

    // -----------------------
    // Data Forwarding (EX-stage operand bypass)
    // -----------------------
    // Forwarding resolves RAW hazards by routing results from
    // MEM or WB back to the EX ALU inputs. We compare EX-stage
    // source register addresses (the instruction currently in EX)
    // against the destination registers of instructions in MEM/WB.
    //
    // Priority: MEM > WB (MEM result is more recent).

    // Operand A (ex_rs1_addr)
    always_comb begin
        if (mem_reg_write_en && (mem_rd_addr != 5'b0) && (mem_rd_addr == ex_rs1_addr)) begin
            forward_a_sel = 2'b01; // Forward from MEM stage
        end else if (wb_reg_write_en && (wb_rd_addr != 5'b0) && (wb_rd_addr == ex_rs1_addr)) begin
            forward_a_sel = 2'b10; // Forward from WB stage
        end else begin
            forward_a_sel = 2'b00; // No forwarding
        end
    end

    // Operand B (ex_rs2_addr)
    always_comb begin
        if (mem_reg_write_en && (mem_rd_addr != 5'b0) && (mem_rd_addr == ex_rs2_addr)) begin
            forward_b_sel = 2'b01; // Forward from MEM stage
        end else if (wb_reg_write_en && (wb_rd_addr != 5'b0) && (wb_rd_addr == ex_rs2_addr)) begin
            forward_b_sel = 2'b10; // Forward from WB stage
        end else begin
            forward_b_sel = 2'b00; // No forwarding
        end
    end

    // -----------------------
    // Load-Use Hazard Detection
    // -----------------------
    // If the instruction in EX is a load, and the instruction in
    // ID reads from the load's destination, we must stall for 1
    // cycle because the load data is not available until MEM.
    logic load_use_stall;
    assign load_use_stall = ex_is_load && (ex_rd_addr != 5'b0) &&
                            ((ex_rd_addr == id_rs1_addr) || (ex_rd_addr == id_rs2_addr));

    // -----------------------
    // Stall and Flush Signal Generation
    // -----------------------
    always_comb begin
        // Defaults: no stalls, no flushes
        if_stall_en    = 1'b0;
        if_id_stall_en = 1'b0;
        id_ex_stall_en = 1'b0;
        ex_mem_stall_en = 1'b0;
        if_id_flush_en = 1'b0;
        id_ex_flush_en = 1'b0;

        // (1) Memory stall (D-Cache miss): freeze the entire pipeline
        if (mem_stall_req) begin
            if_stall_en    = 1'b1;
            if_id_stall_en = 1'b1;
            id_ex_stall_en = 1'b1;
            ex_mem_stall_en = 1'b1;
        end

        // (2) Load-Use stall: freeze IF and IF/ID, insert bubble into ID/EX
        if (load_use_stall && !mem_stall_req) begin
            if_stall_en    = 1'b1;
            if_id_stall_en = 1'b1;
            id_ex_flush_en = 1'b1;
        end

        // (3) Branch/jump misprediction: flush IF/ID and ID/EX
        // Branch flush overrides load-use stall (the stalled instruction is invalid anyway)
        if (ex_branch_taken) begin
            if_id_flush_en = 1'b1;
            id_ex_flush_en = 1'b1;
            // Cancel load-use stall if branch is also taken
            if_stall_en    = 1'b0;
            if_id_stall_en = 1'b0;
        end
    end

endmodule
