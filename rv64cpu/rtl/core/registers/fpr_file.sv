//     Richard CPU
//     Copyright (C) 2026  Richard Qin

// File path: rtl/core/fp_regfile.sv
// Description: 32-entry 64-bit floating-point register file for RV64F/RV64D.

module fp_regfile
    import rv64_pkg::*;
(
    input  logic            clk,
    input  logic            rst,

    input  logic [4:0]      fs1_addr,
    input  logic [4:0]      fs2_addr,
    input  logic [4:0]      fs3_addr,
    output logic [XLEN-1:0] fs1_rdata,
    output logic [XLEN-1:0] fs2_rdata,
    output logic [XLEN-1:0] fs3_rdata,

    input  logic            fp_write_en,
    input  logic [4:0]      fd_addr,
    input  logic [XLEN-1:0] fd_wdata
);

    logic [XLEN-1:0] regs [0:31];

    assign fs1_rdata = (fp_write_en && (fs1_addr == fd_addr)) ? fd_wdata : regs[fs1_addr];
    assign fs2_rdata = (fp_write_en && (fs2_addr == fd_addr)) ? fd_wdata : regs[fs2_addr];
    assign fs3_rdata = (fp_write_en && (fs3_addr == fd_addr)) ? fd_wdata : regs[fs3_addr];

    always_ff @(posedge clk) begin
        if (rst) begin
            for (int unsigned reg_idx = 0; reg_idx < 32; reg_idx++) begin
                regs[reg_idx] <= '0;
            end
        end else if (fp_write_en) begin
            regs[fd_addr] <= fd_wdata;
        end
    end

endmodule
