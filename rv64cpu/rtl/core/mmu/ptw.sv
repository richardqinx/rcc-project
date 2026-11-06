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

// File path: rtl/core/mmu/ptw.sv
// Description: Page-table walker for Sv39. Sv48/Sv57/Sv64 are explicit stubs.

module ptw
	import rv64_pkg::*;
#(
	parameter int ASID_WIDTH = 16
)(
	input  logic                   clk,
	input  logic                   rst,

	input  logic                   req_valid,
	output logic                   req_ready,
	input  logic [XLEN-1:0]        req_vaddr,
	input  logic [ASID_WIDTH-1:0]  req_asid,
	input  logic [3:0]             req_mode,
	input  logic [1:0]             req_priv,
	input  logic                   req_is_fetch,
	input  logic                   req_is_store,
	input  logic                   req_sum,
	input  logic                   req_mxr,
	input  logic [XLEN-1:0]        satp_value,

	output logic                   mem_req_valid,
	input  logic                   mem_req_ready,
	output logic [XLEN-1:0]        mem_req_addr,
	input  logic                   mem_resp_valid,
	input  logic [XLEN-1:0]        mem_resp_data,

	output logic                   resp_valid,
	output logic                   resp_fault,
	output logic [XLEN-1:0]        resp_paddr,

	output logic                   refill_valid,
	output logic [44:0]            refill_vpn,
	output logic [43:0]            refill_ppn,
	output logic [2:0]             refill_level,
	output logic [3:0]             refill_mode,
	output logic [ASID_WIDTH-1:0]  refill_asid,
	output logic                   refill_g,
	output logic                   refill_u,
	output logic                   refill_r,
	output logic                   refill_w,
	output logic                   refill_x,
	output logic                   refill_a,
	output logic                   refill_d
);

	localparam logic [3:0] SATP_MODE_BARE = 4'd0;
	localparam logic [3:0] SATP_MODE_SV39 = 4'd8;
	localparam logic [3:0] SATP_MODE_SV48 = 4'd9;
	localparam logic [3:0] SATP_MODE_SV57 = 4'd10;
	localparam logic [3:0] SATP_MODE_SV64 = 4'd11; // Reserved/future stub.

	typedef enum logic [2:0] {
		PTW_IDLE,
		PTW_ISSUE,
		PTW_WAIT,
		PTW_DONE
	} ptw_state_t;

	ptw_state_t state_q;

	logic [XLEN-1:0]        req_vaddr_q;
	logic [ASID_WIDTH-1:0]  req_asid_q;
	logic [3:0]             req_mode_q;
	logic [1:0]             req_priv_q;
	logic                   req_is_fetch_q;
	logic                   req_is_store_q;
	logic                   req_sum_q;
	logic                   req_mxr_q;

	logic [43:0]            table_ppn_q;
	logic [2:0]             level_q;

	logic                   pte_v;
	logic                   pte_r;
	logic                   pte_w;
	logic                   pte_x;
	logic                   pte_u;
	logic                   pte_g;
	logic                   pte_a;
	logic                   pte_d;
	logic [43:0]            pte_ppn;

	logic                   leaf_pte;
	logic                   bad_pte;
	logic                   superpage_misaligned;

	logic [44:0]            req_vpn_q;
	logic [XLEN-1:0]        next_paddr;

	logic                   resp_fault_q;
	logic [XLEN-1:0]        resp_paddr_q;
	logic                   refill_valid_q;

	logic [44:0]            refill_vpn_q;
	logic [43:0]            refill_ppn_q;
	logic [2:0]             refill_level_q;
	logic [3:0]             refill_mode_q;
	logic [ASID_WIDTH-1:0]  refill_asid_q;
	logic                   refill_g_q;
	logic                   refill_u_q;
	logic                   refill_r_q;
	logic                   refill_w_q;
	logic                   refill_x_q;
	logic                   refill_a_q;
	logic                   refill_d_q;

	function automatic logic is_supported_mode(input logic [3:0] mode);
		return (mode == SATP_MODE_SV39);
	endfunction

	function automatic int vpn_stride(input logic [3:0] mode);
		case (mode)
			SATP_MODE_SV39: vpn_stride = 9;
			default:        vpn_stride = 0;
		endcase
	endfunction

	function automatic logic [44:0] vaddr_to_vpn(
		input logic [XLEN-1:0] vaddr,
		input logic [3:0] mode
	);
		logic [44:0] vpn;
		begin
			vpn = '0;
			case (mode)
				SATP_MODE_SV39: vpn[26:0] = vaddr[38:12];
				default:         vpn = '0;
			endcase
			return vpn;
		end
	endfunction

	function automatic logic [9:0] vpn_index(
		input logic [XLEN-1:0] vaddr,
		input logic [3:0] mode,
		input logic [2:0] level
	);
		logic [9:0] idx;
		begin
			idx = '0;
			case (mode)
				SATP_MODE_SV39: idx = {1'b0, vaddr[12 + level * 9 +: 9]};
				default: idx = '0;
			endcase
			return idx;
		end
	endfunction

	function automatic logic vaddr_is_canonical(
		input logic [XLEN-1:0] vaddr,
		input logic [3:0] mode
	);
		case (mode)
			SATP_MODE_SV39: vaddr_is_canonical = (vaddr[63:39] == {25{vaddr[38]}});
			default:        vaddr_is_canonical = 1'b1;
		endcase
	endfunction

	function automatic logic [5:0] page_shift(
		input logic [3:0] mode,
		input logic [2:0] level
	);
		int stride;
		begin
			stride = vpn_stride(mode);
			return 6'(12 + level * stride);
		end
	endfunction

	function automatic logic allow_access(
		input logic is_fetch,
		input logic is_store,
		input logic [1:0] priv,
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
		logic priv_u;
		logic priv_ok;
		begin
			is_load = ~is_fetch && ~is_store;
			readable = r || (mxr && x);
			priv_u = (priv == 2'b00);

			if (u) begin
				if (priv_u) begin
					priv_ok = 1'b1;
				end else begin
					priv_ok = ~is_fetch && sum;
				end
			end else begin
				priv_ok = ~priv_u;
			end

			return a && priv_ok && (
				(is_fetch && x) ||
				(is_store && w && d) ||
				(is_load && readable)
			);
		end
	endfunction

	assign pte_v = mem_resp_data[0];
	assign pte_r = mem_resp_data[1];
	assign pte_w = mem_resp_data[2];
	assign pte_x = mem_resp_data[3];
	assign pte_u = mem_resp_data[4];
	assign pte_g = mem_resp_data[5];
	assign pte_a = mem_resp_data[6];
	assign pte_d = mem_resp_data[7];
	assign pte_ppn = mem_resp_data[53:10];

	assign leaf_pte = pte_r || pte_x;
	assign bad_pte = (!pte_v) || (pte_w && !pte_r);

	always_comb begin
		int stride;
		int lower_bits;
		logic [63:0] ppn_mask;
		stride = vpn_stride(req_mode_q);
		lower_bits = level_q * stride;
		ppn_mask = '0;
		superpage_misaligned = 1'b0;
		if (leaf_pte && (lower_bits > 0)) begin
			if (lower_bits >= 44) begin
				ppn_mask = 64'h0000_0FFF_FFFF_FFFF;
			end else begin
				ppn_mask = (64'd1 << lower_bits) - 1;
			end
			superpage_misaligned = |({20'b0, pte_ppn} & ppn_mask);
		end
	end

	always_comb begin
		logic [5:0] shift;
		logic [XLEN-1:0] low_mask;
		int shift_index;
		shift = page_shift(req_mode_q, level_q);
		shift_index = int'(shift);
		if (shift_index >= XLEN) begin
			low_mask = {XLEN{1'b1}};
		end else begin
			low_mask = ({{(XLEN-1){1'b0}}, 1'b1} << shift_index) - 1;
		end
		next_paddr = (({{(XLEN-56){1'b0}}, pte_ppn, 12'b0}) & ~low_mask) | (req_vaddr_q & low_mask);
	end

	assign req_ready = (state_q == PTW_IDLE);
	assign mem_req_valid = (state_q == PTW_ISSUE);
	assign mem_req_addr = {{(XLEN-56){1'b0}}, table_ppn_q, 12'b0} +
						 ({{(XLEN-10){1'b0}}, vpn_index(req_vaddr_q, req_mode_q, level_q)} << 3);

	assign resp_valid = (state_q == PTW_DONE);
	assign resp_fault = resp_fault_q;
	assign resp_paddr = resp_paddr_q;

	assign refill_valid = refill_valid_q && (state_q == PTW_DONE);
	assign refill_vpn = refill_vpn_q;
	assign refill_ppn = refill_ppn_q;
	assign refill_level = refill_level_q;
	assign refill_mode = refill_mode_q;
	assign refill_asid = refill_asid_q;
	assign refill_g = refill_g_q;
	assign refill_u = refill_u_q;
	assign refill_r = refill_r_q;
	assign refill_w = refill_w_q;
	assign refill_x = refill_x_q;
	assign refill_a = refill_a_q;
	assign refill_d = refill_d_q;

	always_ff @(posedge clk) begin
		if (rst) begin
			state_q <= PTW_IDLE;
			req_vaddr_q <= '0;
			req_asid_q <= '0;
			req_mode_q <= SATP_MODE_BARE;
			req_priv_q <= '0;
			req_is_fetch_q <= 1'b0;
			req_is_store_q <= 1'b0;
			req_sum_q <= 1'b0;
			req_mxr_q <= 1'b0;
			table_ppn_q <= '0;
			level_q <= '0;
			req_vpn_q <= '0;
			resp_fault_q <= 1'b0;
			resp_paddr_q <= '0;
			refill_valid_q <= 1'b0;
			refill_vpn_q <= '0;
			refill_ppn_q <= '0;
			refill_level_q <= '0;
			refill_mode_q <= SATP_MODE_BARE;
			refill_asid_q <= '0;
			refill_g_q <= 1'b0;
			refill_u_q <= 1'b0;
			refill_r_q <= 1'b0;
			refill_w_q <= 1'b0;
			refill_x_q <= 1'b0;
			refill_a_q <= 1'b0;
			refill_d_q <= 1'b0;
		end else begin
			case (state_q)
				PTW_IDLE: begin
					refill_valid_q <= 1'b0;
					if (req_valid) begin
						req_vaddr_q <= req_vaddr;
						req_asid_q <= req_asid;
						req_mode_q <= req_mode;
						req_priv_q <= req_priv;
						req_is_fetch_q <= req_is_fetch;
						req_is_store_q <= req_is_store;
						req_sum_q <= req_sum;
						req_mxr_q <= req_mxr;
						req_vpn_q <= vaddr_to_vpn(req_vaddr, req_mode);

						if (!is_supported_mode(req_mode) || !vaddr_is_canonical(req_vaddr, req_mode)) begin
							resp_fault_q <= 1'b1;
							resp_paddr_q <= '0;
							state_q <= PTW_DONE;
						end else begin
							table_ppn_q <= satp_value[43:0];
							case (req_mode)
								SATP_MODE_SV39: level_q <= 3'd2;
								default:        level_q <= '0;
							endcase
							state_q <= PTW_ISSUE;
						end
					end
				end

				PTW_ISSUE: begin
					if (mem_req_ready) begin
						state_q <= PTW_WAIT;
					end
				end

				PTW_WAIT: begin
					if (mem_resp_valid) begin
						if (bad_pte) begin
							resp_fault_q <= 1'b1;
							resp_paddr_q <= '0;
							refill_valid_q <= 1'b0;
							state_q <= PTW_DONE;
						end else if (leaf_pte) begin
							if (superpage_misaligned ||
								!allow_access(req_is_fetch_q, req_is_store_q, req_priv_q, req_sum_q, req_mxr_q,
												  pte_u, pte_r, pte_w, pte_x, pte_a, pte_d)) begin
								resp_fault_q <= 1'b1;
								resp_paddr_q <= '0;
								refill_valid_q <= 1'b0;
							end else begin
								resp_fault_q <= 1'b0;
								resp_paddr_q <= next_paddr;
								refill_valid_q <= 1'b1;
								refill_vpn_q <= req_vpn_q;
								refill_ppn_q <= pte_ppn;
								refill_level_q <= level_q;
								refill_mode_q <= req_mode_q;
								refill_asid_q <= req_asid_q;
								refill_g_q <= pte_g;
								refill_u_q <= pte_u;
								refill_r_q <= pte_r;
								refill_w_q <= pte_w;
								refill_x_q <= pte_x;
								refill_a_q <= pte_a;
								refill_d_q <= pte_d;
							end
							state_q <= PTW_DONE;
						end else if (level_q == 0) begin
							resp_fault_q <= 1'b1;
							resp_paddr_q <= '0;
							refill_valid_q <= 1'b0;
							state_q <= PTW_DONE;
						end else begin
							table_ppn_q <= pte_ppn;
							level_q <= level_q - 1'b1;
							state_q <= PTW_ISSUE;
						end
					end
				end

				PTW_DONE: begin
					state_q <= PTW_IDLE;
				end

				default: state_q <= PTW_IDLE;
			endcase
		end
	end

endmodule
