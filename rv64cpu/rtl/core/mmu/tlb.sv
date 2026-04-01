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

// File path: rtl/core/mmu/tlb.sv
// Description: Parameterized L1 TLB (Sv32/Sv48/Sv57)

module tlb
	import rv64_pkg::*;
#(
	parameter int ENTRIES = 32,
	parameter int ASID_WIDTH = 16
)(
	input  logic                   clk,
	input  logic                   rst,

	// Translation request
	input  logic                   req_valid,
	input  logic [XLEN-1:0]        req_vaddr,
	input  logic [ASID_WIDTH-1:0]  req_asid,
	input  logic [3:0]             req_mode,       // satp.MODE
	input  logic [1:0]             req_priv,       // 0=U,1=S,3=M
	input  logic                   req_is_fetch,
	input  logic                   req_is_store,
	input  logic                   req_sum,
	input  logic                   req_mxr,

	// Translation response
	output logic                   resp_valid,
	output logic                   resp_hit,
	output logic [XLEN-1:0]        resp_paddr,
	output logic                   resp_page_fault,
	output logic                   resp_need_ptw,

	// Refill from PTW
	input  logic                   refill_valid,
	input  logic [44:0]            refill_vpn,     // normalized VPN, max Sv57 width
	input  logic [43:0]            refill_ppn,
	input  logic [2:0]             refill_level,   // 0=4KB leaf, 1=next superpage ...
	input  logic [3:0]             refill_mode,
	input  logic [ASID_WIDTH-1:0]  refill_asid,
	input  logic                   refill_g,
	input  logic                   refill_u,
	input  logic                   refill_r,
	input  logic                   refill_w,
	input  logic                   refill_x,
	input  logic                   refill_a,
	input  logic                   refill_d,

	// sfence / invalidation hooks
	input  logic                   flush_all,
	input  logic                   flush_asid_valid,
	input  logic [ASID_WIDTH-1:0]  flush_asid,
	input  logic                   flush_vpn_valid,
	input  logic [44:0]            flush_vpn,
	input  logic [3:0]             flush_mode
);

	localparam logic [3:0] SATP_MODE_BARE = 4'd0;
	localparam logic [3:0] SATP_MODE_SV32 = 4'd1;
	localparam logic [3:0] SATP_MODE_SV48 = 4'd9;
	localparam logic [3:0] SATP_MODE_SV57 = 4'd10;

	typedef struct packed {
		logic                   valid;
		logic [3:0]             mode;
		logic [ASID_WIDTH-1:0]  asid;
		logic [44:0]            vpn;
		logic [43:0]            ppn;
		logic [2:0]             level;
		logic                   g;
		logic                   u;
		logic                   r;
		logic                   w;
		logic                   x;
		logic                   a;
		logic                   d;
	} tlb_entry_t;

	tlb_entry_t entries [0:ENTRIES-1];
	logic [$clog2(ENTRIES)-1:0] repl_ptr;

	logic [44:0] req_vpn;
	logic [43:0] hit_ppn;
	logic [2:0]  hit_level;
	logic [3:0]  hit_mode;
	logic        hit_u, hit_r, hit_w, hit_x, hit_a, hit_d;
	logic        hit_found;
	logic [5:0]  shift;
	logic [XLEN-1:0] low_mask;
	logic        req_priv_u;
	logic [XLEN-1:0] hit_ppn_xlen;
	int unsigned shift_int;

	function automatic logic is_supported_mode(input logic [3:0] mode);
		return (mode == SATP_MODE_BARE) || (mode == SATP_MODE_SV32) ||
			   (mode == SATP_MODE_SV48) || (mode == SATP_MODE_SV57);
	endfunction

	function automatic int vpn_width(input logic [3:0] mode);
		case (mode)
			SATP_MODE_SV32: vpn_width = 20;
			SATP_MODE_SV48: vpn_width = 36;
			SATP_MODE_SV57: vpn_width = 45;
			default:        vpn_width = 0;
		endcase
	endfunction

	function automatic int vpn_stride(input logic [3:0] mode);
		case (mode)
			SATP_MODE_SV32: vpn_stride = 10;
			SATP_MODE_SV48,
			SATP_MODE_SV57: vpn_stride = 9;
			default:        vpn_stride = 0;
		endcase
	endfunction

	function automatic logic [44:0] make_vpn_mask(
		input logic [3:0] mode,
		input logic [2:0] level
	);
		logic [44:0] mask;
		int width;
		int stride;
		int ignore;
		int i;
		begin
			mask = '0;
			width = vpn_width(mode);
			stride = vpn_stride(mode);
			ignore = level * stride;

			if (width > 0) begin
				for (i = ignore; i < width; i++) begin
					mask[i] = 1'b1;
				end
			end

			make_vpn_mask = mask;
		end
	endfunction

	function automatic logic [44:0] vaddr_to_vpn(
		input logic [XLEN-1:0] vaddr,
		input logic [3:0] mode
	);
		logic [44:0] vpn;
		begin
			vpn = '0;
			case (mode)
				SATP_MODE_SV32: vpn[19:0] = vaddr[31:12];
				SATP_MODE_SV48: vpn[35:0] = vaddr[47:12];
				SATP_MODE_SV57: vpn[44:0] = vaddr[56:12];
				default:         vpn = '0;
			endcase
			vaddr_to_vpn = vpn;
		end
	endfunction

	function automatic logic [5:0] page_shift(
		input logic [3:0] mode,
		input logic [2:0] level
	);
		int stride;
		begin
			stride = vpn_stride(mode);
			page_shift = 6'(12 + level * stride);
		end
	endfunction

	function automatic logic entry_match(
		input tlb_entry_t e,
		input logic [44:0] vpn,
		input logic [ASID_WIDTH-1:0] asid,
		input logic [3:0] mode
	);
		logic [44:0] mask;
		begin
			mask = make_vpn_mask(e.mode, e.level);
			entry_match = e.valid && (e.mode == mode) && (e.g || (e.asid == asid)) &&
						  ((((e.vpn ^ vpn) & mask) == '0));
		end
	endfunction

	function automatic logic allow_access(
		input logic is_fetch,
		input logic is_store,
		input logic priv_u,
		input logic sum,
		input logic mxr,
		input logic u,
		input logic r,
		input logic w,
		input logic x,
		input logic a,
		input logic d
	);
		logic is_load;
		logic readable;
		logic priv_ok;
		begin
			is_load = ~is_fetch && ~is_store;
			readable = r || (mxr && x);

			// U=1 page: only U-mode unless SUM permits S-mode data access
			// U=0 page: S/M-mode only
			if (u) begin
				if (priv_u) begin
					priv_ok = 1'b1;
				end else begin
					priv_ok = ~is_fetch && sum;
				end
			end else begin
				priv_ok = ~priv_u;
			end

			allow_access = a && priv_ok && (
				(is_fetch && x) ||
				(is_store && w && d) ||
				(is_load  && readable)
			);
		end
	endfunction

	integer k;
	always_comb begin
		req_vpn = vaddr_to_vpn(req_vaddr, req_mode);

		resp_valid = req_valid;
		resp_hit = 1'b0;
		resp_paddr = req_vaddr;
		resp_page_fault = 1'b0;
		resp_need_ptw = 1'b0;

		shift = '0;
		low_mask = '0;
		req_priv_u = 1'b0;
		hit_ppn_xlen = '0;
		shift_int = 0;

		hit_found = 1'b0;
		hit_ppn = '0;
		hit_level = '0;
		hit_mode = '0;
		hit_u = 1'b0;
		hit_r = 1'b0;
		hit_w = 1'b0;
		hit_x = 1'b0;
		hit_a = 1'b0;
		hit_d = 1'b0;

		if (req_valid) begin
			if (!is_supported_mode(req_mode)) begin
				resp_page_fault = 1'b1;
			end else if (req_mode == SATP_MODE_BARE || req_priv == 2'b11) begin
				// Bare mode or M-mode bypasses translation.
				resp_hit = 1'b1;
				resp_paddr = req_vaddr;
			end else begin
				for (k = 0; k < ENTRIES; k++) begin
					if (!hit_found && entry_match(entries[k], req_vpn, req_asid, req_mode)) begin
						hit_found = 1'b1;
						hit_ppn = entries[k].ppn;
						hit_level = entries[k].level;
						hit_mode = entries[k].mode;
						hit_u = entries[k].u;
						hit_r = entries[k].r;
						hit_w = entries[k].w;
						hit_x = entries[k].x;
						hit_a = entries[k].a;
						hit_d = entries[k].d;
					end
				end

				if (hit_found) begin
					shift = page_shift(hit_mode, hit_level);
					shift_int = int'(shift);
					if (shift_int >= XLEN) begin
						low_mask = {XLEN{1'b1}};
					end else begin
						low_mask = ({{(XLEN-1){1'b0}}, 1'b1} << shift_int) - 1;
					end

					hit_ppn_xlen = {{(XLEN-56){1'b0}}, hit_ppn, 12'b0};

					req_priv_u = (req_priv == 2'b00);
					if (!allow_access(req_is_fetch, req_is_store, req_priv_u, req_sum, req_mxr,
									  hit_u, hit_r, hit_w, hit_x, hit_a, hit_d)) begin
						resp_page_fault = 1'b1;
					end else begin
						resp_hit = 1'b1;
						resp_paddr = (hit_ppn_xlen & ~low_mask) | (req_vaddr & low_mask);
					end
				end else begin
					resp_need_ptw = 1'b1;
				end
			end
		end
	end

	integer i;
	always_ff @(posedge clk) begin
		if (rst) begin
			repl_ptr <= '0;
			for (i = 0; i < ENTRIES; i++) begin
				entries[i].valid <= 1'b0;
				entries[i].mode <= SATP_MODE_BARE;
				entries[i].asid <= '0;
				entries[i].vpn <= '0;
				entries[i].ppn <= '0;
				entries[i].level <= '0;
				entries[i].g <= 1'b0;
				entries[i].u <= 1'b0;
				entries[i].r <= 1'b0;
				entries[i].w <= 1'b0;
				entries[i].x <= 1'b0;
				entries[i].a <= 1'b0;
				entries[i].d <= 1'b0;
			end
		end else begin
			if (flush_all) begin
				for (i = 0; i < ENTRIES; i++) begin
					entries[i].valid <= 1'b0;
				end
			end else if (flush_asid_valid || flush_vpn_valid) begin
				for (i = 0; i < ENTRIES; i++) begin
					logic asid_match;
					logic vpn_match;
					logic mode_match;
					logic [44:0] mask;

					asid_match = !flush_asid_valid || entries[i].g || (entries[i].asid == flush_asid);
					mode_match = !flush_vpn_valid || (entries[i].mode == flush_mode);
					if (flush_vpn_valid) begin
						mask = make_vpn_mask(entries[i].mode, entries[i].level);
						vpn_match = (((entries[i].vpn ^ flush_vpn) & mask) == '0);
					end else begin
						vpn_match = 1'b1;
					end

					if (entries[i].valid && asid_match && mode_match && vpn_match) begin
						entries[i].valid <= 1'b0;
					end
				end
			end

			if (refill_valid && is_supported_mode(refill_mode) && (refill_mode != SATP_MODE_BARE)) begin
				entries[repl_ptr].valid <= 1'b1;
				entries[repl_ptr].mode <= refill_mode;
				entries[repl_ptr].asid <= refill_asid;
				entries[repl_ptr].vpn <= refill_vpn;
				entries[repl_ptr].ppn <= refill_ppn;
				entries[repl_ptr].level <= refill_level;
				entries[repl_ptr].g <= refill_g;
				entries[repl_ptr].u <= refill_u;
				entries[repl_ptr].r <= refill_r;
				entries[repl_ptr].w <= refill_w;
				entries[repl_ptr].x <= refill_x;
				entries[repl_ptr].a <= refill_a;
				entries[repl_ptr].d <= refill_d;
				repl_ptr <= repl_ptr + 1'b1;
			end
		end
	end

endmodule
