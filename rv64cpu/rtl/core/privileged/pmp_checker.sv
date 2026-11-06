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

// File path: rtl/core/pmp_checker.sv
// Description: RISC-V Physical Memory Protection access checker.

module pmp_checker
    import rv64_pkg::*;
    import csr_pkg::*;
#(
    parameter int PHYS_ADDR_WIDTH = 56
)(
    input  logic              req_valid,
    input  logic [XLEN-1:0]   req_paddr,
    input  logic [3:0]        req_size,
    input  logic              req_is_fetch,
    input  logic              req_is_store,
    input  logic [1:0]        priv_mode,
    input  pmpcfg_entry_t     pmpcfg_entries [PMP_ENTRY_COUNT],
    input  pmpaddr_entry_t    pmpaddr_entries [PMP_ENTRY_COUNT],
    output logic              access_fault
);

    localparam logic [1:0] PRIV_MODE_M = 2'b11;

    typedef struct packed {
        logic valid;
        logic [XLEN-1:0] lower_bound;
        logic [XLEN-1:0] upper_bound;
    } pmp_region_t;

    function automatic logic [XLEN-1:0] pmpaddr_to_paddr(input pmpaddr_entry_t pmpaddr);
        logic [XLEN-1:0] paddr;
        paddr = pmpaddr << 2;
        if (PHYS_ADDR_WIDTH < XLEN) begin
            paddr[XLEN-1:PHYS_ADDR_WIDTH] = '0;
        end
        return paddr;
    endfunction

    function automatic int unsigned napot_trailing_ones(input pmpaddr_entry_t pmpaddr);
        int unsigned ones;
        ones = 0;
        for (int bit_idx = 0; bit_idx < (PHYS_ADDR_WIDTH - 2); bit_idx++) begin
            if (pmpaddr[bit_idx]) begin
                ones++;
            end else begin
                break;
            end
        end
        return ones;
    endfunction

    function automatic pmp_region_t pmp_region(input int unsigned entry_idx);
        pmp_region_t region;
        logic [1:0] address_matching;
        int unsigned ones;
        logic [XLEN-1:0] napot_mask;
        logic [XLEN-1:0] napot_base;

        region.valid = 1'b0;
        region.lower_bound = '0;
        region.upper_bound = '0;
        address_matching = pmpcfg_entries[entry_idx][PMPCFG_A_HI:PMPCFG_A_LO];

        unique case (address_matching)
            PMP_MODE_OFF: begin
                region.valid = 1'b0;
            end
            PMP_MODE_TOR: begin
                region.valid = 1'b1;
                region.lower_bound = (entry_idx == 0) ? '0 : pmpaddr_to_paddr(pmpaddr_entries[entry_idx - 1]);
                region.upper_bound = pmpaddr_to_paddr(pmpaddr_entries[entry_idx]);
                if (region.upper_bound <= region.lower_bound) begin
                    region.valid = 1'b0;
                end
            end
            PMP_MODE_NA4: begin
                region.valid = 1'b1;
                region.lower_bound = pmpaddr_to_paddr(pmpaddr_entries[entry_idx]);
                region.upper_bound = region.lower_bound + 64'd4;
            end
            PMP_MODE_NAPOT: begin
                region.valid = 1'b1;
                ones = napot_trailing_ones(pmpaddr_entries[entry_idx]);
                napot_mask = (64'd1 << (ones + 1)) - 64'd1;
                napot_base = (pmpaddr_entries[entry_idx] & ~napot_mask) << 2;
                if (PHYS_ADDR_WIDTH < XLEN) begin
                    napot_base[XLEN-1:PHYS_ADDR_WIDTH] = '0;
                end
                region.lower_bound = napot_base;
                region.upper_bound = napot_base + (64'd1 << (ones + 3));
            end
            default: begin
                region.valid = 1'b0;
            end
        endcase

        return region;
    endfunction

    function automatic logic ranges_overlap(
        input logic [XLEN-1:0] access_lower,
        input logic [XLEN-1:0] access_upper,
        input logic [XLEN-1:0] region_lower,
        input logic [XLEN-1:0] region_upper
    );
        return (access_lower < region_upper) && (access_upper > region_lower);
    endfunction

    function automatic logic range_contains(
        input logic [XLEN-1:0] access_lower,
        input logic [XLEN-1:0] access_upper,
        input logic [XLEN-1:0] region_lower,
        input logic [XLEN-1:0] region_upper
    );
        return (access_lower >= region_lower) && (access_upper <= region_upper);
    endfunction

    function automatic logic permissions_allow(
        input pmpcfg_entry_t cfg,
        input logic is_fetch,
        input logic is_store
    );
        if (is_fetch) begin
            return cfg[PMPCFG_X_BIT];
        end
        if (is_store) begin
            return cfg[PMPCFG_W_BIT];
        end
        return cfg[PMPCFG_R_BIT];
    endfunction

    always_comb begin
        logic [XLEN-1:0] access_lower;
        logic [XLEN-1:0] access_upper;
        logic [3:0] normalized_size;
        logic matched;
        pmp_region_t region;

        normalized_size = (req_size == 4'd0) ? 4'd1 : req_size;
        access_lower = req_paddr;
        access_upper = req_paddr + {{(XLEN-4){1'b0}}, normalized_size};
        matched = 1'b0;
        region = '0;
        access_fault = 1'b0;

        if (req_valid) begin
            for (int unsigned entry_idx = 0; entry_idx < PMP_ENTRY_COUNT; entry_idx++) begin
                region = pmp_region(entry_idx);
                if (!matched && region.valid &&
                    ranges_overlap(access_lower, access_upper, region.lower_bound, region.upper_bound)) begin
                    matched = 1'b1;
                    if (!range_contains(access_lower, access_upper, region.lower_bound, region.upper_bound)) begin
                        access_fault = 1'b1;
                    end else if ((priv_mode != PRIV_MODE_M) || pmpcfg_entries[entry_idx][PMPCFG_L_BIT]) begin
                        access_fault = !permissions_allow(pmpcfg_entries[entry_idx], req_is_fetch, req_is_store);
                    end
                end
            end

            if (!matched && (priv_mode != PRIV_MODE_M)) begin
                access_fault = 1'b1;
            end
        end
    end

endmodule
