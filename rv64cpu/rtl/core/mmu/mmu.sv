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

// File path: rtl/core/mmu/mmu_top.sv
// Description: MMU top-level integrating iTLB + dTLB + shared PTW

module mmu
	import rv64_pkg::*;
#(
	parameter int ITLB_ENTRIES = 16,
	parameter int DTLB_ENTRIES = 32,
	parameter int ASID_WIDTH = 16
)(
	input  logic                  clk,
	input  logic                  rst,

	// CSR/context inputs
	input  logic [XLEN-1:0]       satp_value,
	input  logic [1:0]            priv_mode,
	input  logic                  sum,
	input  logic                  mxr,

	// i-side translation request
	input  logic                  i_req_valid,
	output logic                  i_req_ready,
	input  logic [XLEN-1:0]       i_req_vaddr,
	output logic                  i_resp_valid,
	output logic                  i_resp_hit,
	output logic                  i_resp_page_fault,
	output logic [XLEN-1:0]       i_resp_vaddr,
	output logic [XLEN-1:0]       i_resp_paddr,

	// d-side translation request
	input  logic                  d_req_valid,
	output logic                  d_req_ready,
	input  logic [XLEN-1:0]       d_req_vaddr,
	input  logic                  d_req_is_store,
	output logic                  d_resp_valid,
	output logic                  d_resp_hit,
	output logic                  d_resp_page_fault,
	output logic [XLEN-1:0]       d_resp_paddr,

	// sfence / invalidation
	input  logic                  flush_all,
	input  logic                  flush_asid_valid,
	input  logic [ASID_WIDTH-1:0] flush_asid,
	input  logic                  flush_vpn_valid,
	input  logic [44:0]           flush_vpn,

	// PTW memory interface
	output logic                  ptw_mem_req_valid,
	input  logic                  ptw_mem_req_ready,
	output logic [XLEN-1:0]       ptw_mem_req_addr,
	input  logic                  ptw_mem_resp_valid,
	input  logic [XLEN-1:0]       ptw_mem_resp_data
);

	localparam logic [3:0] SATP_MODE_BARE = 4'd0;

	typedef enum logic [0:0] {
		PTW_SRC_I = 1'b0,
		PTW_SRC_D = 1'b1
	} ptw_src_t;

	logic [ASID_WIDTH-1:0] req_asid;
	logic [3:0]            req_mode;

	logic                  i_tlb_resp_valid;
	logic                  i_tlb_resp_hit;
	logic [XLEN-1:0]       i_tlb_resp_paddr;
	logic                  i_tlb_resp_page_fault;
	logic                  i_tlb_resp_need_ptw;

	logic                  d_tlb_resp_valid;
	logic                  d_tlb_resp_hit;
	logic [XLEN-1:0]       d_tlb_resp_paddr;
	logic                  d_tlb_resp_page_fault;
	logic                  d_tlb_resp_need_ptw;

	logic                  i_pending_q;
	logic                  d_pending_q;
	logic [XLEN-1:0]       i_pending_vaddr_q;
	logic [XLEN-1:0]       d_pending_vaddr_q;
	logic [1:0]            i_pending_priv_q;
	logic [1:0]            d_pending_priv_q;
	logic                  i_pending_sum_q;
	logic                  d_pending_sum_q;
	logic                  i_pending_mxr_q;
	logic                  d_pending_mxr_q;
	logic [ASID_WIDTH-1:0] i_pending_asid_q;
	logic [ASID_WIDTH-1:0] d_pending_asid_q;
	logic [3:0]            i_pending_mode_q;
	logic [3:0]            d_pending_mode_q;
	logic                  d_pending_is_store_q;

	logic                  ptw_req_valid;
	logic                  ptw_req_ready;
	logic                  ptw_resp_valid;
	logic                  ptw_resp_fault;
	logic [XLEN-1:0]       ptw_resp_paddr;

	logic                  ptw_refill_valid;
	logic [44:0]           ptw_refill_vpn;
	logic [43:0]           ptw_refill_ppn;
	logic [2:0]            ptw_refill_level;
	logic [3:0]            ptw_refill_mode;
	logic [ASID_WIDTH-1:0] ptw_refill_asid;
	logic                  ptw_refill_g;
	logic                  ptw_refill_u;
	logic                  ptw_refill_r;
	logic                  ptw_refill_w;
	logic                  ptw_refill_x;
	logic                  ptw_refill_a;
	logic                  ptw_refill_d;

	logic                  ptw_busy_q;
	ptw_src_t              ptw_owner_q;
	ptw_src_t              ptw_owner_next;

	logic                  i_accept;
	logic                  d_accept;
	logic                  i_fast_resp_valid;
	logic                  d_fast_resp_valid;
	logic                  i_ptw_resp_sel;
	logic                  d_ptw_resp_sel;

	logic                  i_refill_valid;
	logic                  d_refill_valid;

	assign req_mode = satp_value[63:60];
	assign req_asid = satp_value[59 -: ASID_WIDTH];

	assign i_req_ready = !i_pending_q;
	assign d_req_ready = !d_pending_q;
	assign i_accept = i_req_valid && i_req_ready;
	assign d_accept = d_req_valid && d_req_ready;

	assign i_fast_resp_valid = i_accept && (i_tlb_resp_hit || i_tlb_resp_page_fault);
	assign d_fast_resp_valid = d_accept && (d_tlb_resp_hit || d_tlb_resp_page_fault);

	assign i_ptw_resp_sel = ptw_resp_valid && (ptw_owner_q == PTW_SRC_I);
	assign d_ptw_resp_sel = ptw_resp_valid && (ptw_owner_q == PTW_SRC_D);

	assign i_resp_valid = i_fast_resp_valid || i_ptw_resp_sel;
	assign i_resp_hit = i_fast_resp_valid ? i_tlb_resp_hit : (i_ptw_resp_sel && !ptw_resp_fault);
	assign i_resp_page_fault = i_fast_resp_valid ? i_tlb_resp_page_fault : (i_ptw_resp_sel && ptw_resp_fault);
	assign i_resp_vaddr = i_fast_resp_valid ? i_req_vaddr : i_pending_vaddr_q;
	assign i_resp_paddr = i_fast_resp_valid ? i_tlb_resp_paddr : ptw_resp_paddr;

	assign d_resp_valid = d_fast_resp_valid || d_ptw_resp_sel;
	assign d_resp_hit = d_fast_resp_valid ? d_tlb_resp_hit : (d_ptw_resp_sel && !ptw_resp_fault);
	assign d_resp_page_fault = d_fast_resp_valid ? d_tlb_resp_page_fault : (d_ptw_resp_sel && ptw_resp_fault);
	assign d_resp_paddr = d_fast_resp_valid ? d_tlb_resp_paddr : ptw_resp_paddr;

	always_comb begin
		if (d_pending_q) begin
			ptw_owner_next = PTW_SRC_D;
		end else begin
			ptw_owner_next = PTW_SRC_I;
		end
	end

	assign ptw_req_valid = !ptw_busy_q && (d_pending_q || i_pending_q);

	assign i_refill_valid = ptw_refill_valid && (ptw_owner_q == PTW_SRC_I);
	assign d_refill_valid = ptw_refill_valid && (ptw_owner_q == PTW_SRC_D);

	always_ff @(posedge clk) begin
		if (rst) begin
			i_pending_q <= 1'b0;
			d_pending_q <= 1'b0;
			ptw_busy_q <= 1'b0;
			ptw_owner_q <= PTW_SRC_I;
			i_pending_vaddr_q <= '0;
			d_pending_vaddr_q <= '0;
			i_pending_priv_q <= '0;
			d_pending_priv_q <= '0;
			i_pending_sum_q <= 1'b0;
			d_pending_sum_q <= 1'b0;
			i_pending_mxr_q <= 1'b0;
			d_pending_mxr_q <= 1'b0;
			i_pending_asid_q <= '0;
			d_pending_asid_q <= '0;
			i_pending_mode_q <= SATP_MODE_BARE;
			d_pending_mode_q <= SATP_MODE_BARE;
			d_pending_is_store_q <= 1'b0;
		end else begin
			if (i_accept && i_tlb_resp_need_ptw) begin
				i_pending_q <= 1'b1;
				i_pending_vaddr_q <= i_req_vaddr;
				i_pending_priv_q <= priv_mode;
				i_pending_sum_q <= sum;
				i_pending_mxr_q <= mxr;
				i_pending_asid_q <= req_asid;
				i_pending_mode_q <= req_mode;
			end

			if (d_accept && d_tlb_resp_need_ptw) begin
				d_pending_q <= 1'b1;
				d_pending_vaddr_q <= d_req_vaddr;
				d_pending_priv_q <= priv_mode;
				d_pending_sum_q <= sum;
				d_pending_mxr_q <= mxr;
				d_pending_asid_q <= req_asid;
				d_pending_mode_q <= req_mode;
				d_pending_is_store_q <= d_req_is_store;
			end

			if (ptw_req_valid && ptw_req_ready) begin
				ptw_busy_q <= 1'b1;
				ptw_owner_q <= ptw_owner_next;
			end

			if (ptw_resp_valid) begin
				ptw_busy_q <= 1'b0;
				if (ptw_owner_q == PTW_SRC_I) begin
					i_pending_q <= 1'b0;
				end else begin
					d_pending_q <= 1'b0;
				end
			end
		end
	end

	tlb #(
		.ENTRIES(ITLB_ENTRIES),
		.ASID_WIDTH(ASID_WIDTH)
	) u_itlb (
		.clk              (clk),
		.rst              (rst),
		.req_valid        (i_accept),
		.req_vaddr        (i_req_vaddr),
		.req_asid         (req_asid),
		.req_mode         (req_mode),
		.req_priv         (priv_mode),
		.req_is_fetch     (1'b1),
		.req_is_store     (1'b0),
		.req_sum          (sum),
		.req_mxr          (mxr),
		.resp_valid       (i_tlb_resp_valid),
		.resp_hit         (i_tlb_resp_hit),
		.resp_paddr       (i_tlb_resp_paddr),
		.resp_page_fault  (i_tlb_resp_page_fault),
		.resp_need_ptw    (i_tlb_resp_need_ptw),
		.refill_valid     (i_refill_valid),
		.refill_vpn       (ptw_refill_vpn),
		.refill_ppn       (ptw_refill_ppn),
		.refill_level     (ptw_refill_level),
		.refill_mode      (ptw_refill_mode),
		.refill_asid      (ptw_refill_asid),
		.refill_g         (ptw_refill_g),
		.refill_u         (ptw_refill_u),
		.refill_r         (ptw_refill_r),
		.refill_w         (ptw_refill_w),
		.refill_x         (ptw_refill_x),
		.refill_a         (ptw_refill_a),
		.refill_d         (ptw_refill_d),
		.flush_all        (flush_all),
		.flush_asid_valid (flush_asid_valid),
		.flush_asid       (flush_asid),
		.flush_vpn_valid  (flush_vpn_valid),
		.flush_vpn        (flush_vpn),
		.flush_mode       (req_mode)
	);

	tlb #(
		.ENTRIES(DTLB_ENTRIES),
		.ASID_WIDTH(ASID_WIDTH)
	) u_dtlb (
		.clk              (clk),
		.rst              (rst),
		.req_valid        (d_accept),
		.req_vaddr        (d_req_vaddr),
		.req_asid         (req_asid),
		.req_mode         (req_mode),
		.req_priv         (priv_mode),
		.req_is_fetch     (1'b0),
		.req_is_store     (d_req_is_store),
		.req_sum          (sum),
		.req_mxr          (mxr),
		.resp_valid       (d_tlb_resp_valid),
		.resp_hit         (d_tlb_resp_hit),
		.resp_paddr       (d_tlb_resp_paddr),
		.resp_page_fault  (d_tlb_resp_page_fault),
		.resp_need_ptw    (d_tlb_resp_need_ptw),
		.refill_valid     (d_refill_valid),
		.refill_vpn       (ptw_refill_vpn),
		.refill_ppn       (ptw_refill_ppn),
		.refill_level     (ptw_refill_level),
		.refill_mode      (ptw_refill_mode),
		.refill_asid      (ptw_refill_asid),
		.refill_g         (ptw_refill_g),
		.refill_u         (ptw_refill_u),
		.refill_r         (ptw_refill_r),
		.refill_w         (ptw_refill_w),
		.refill_x         (ptw_refill_x),
		.refill_a         (ptw_refill_a),
		.refill_d         (ptw_refill_d),
		.flush_all        (flush_all),
		.flush_asid_valid (flush_asid_valid),
		.flush_asid       (flush_asid),
		.flush_vpn_valid  (flush_vpn_valid),
		.flush_vpn        (flush_vpn),
		.flush_mode       (req_mode)
	);

	ptw #(
		.ASID_WIDTH(ASID_WIDTH)
	) u_ptw (
		.clk           (clk),
		.rst           (rst),
		.req_valid     (ptw_req_valid),
		.req_ready     (ptw_req_ready),
		.req_vaddr     ((ptw_owner_next == PTW_SRC_D) ? d_pending_vaddr_q : i_pending_vaddr_q),
		.req_asid      ((ptw_owner_next == PTW_SRC_D) ? d_pending_asid_q : i_pending_asid_q),
		.req_mode      ((ptw_owner_next == PTW_SRC_D) ? d_pending_mode_q : i_pending_mode_q),
		.req_priv      ((ptw_owner_next == PTW_SRC_D) ? d_pending_priv_q : i_pending_priv_q),
		.req_is_fetch  (ptw_owner_next == PTW_SRC_I),
		.req_is_store  ((ptw_owner_next == PTW_SRC_D) ? d_pending_is_store_q : 1'b0),
		.req_sum       ((ptw_owner_next == PTW_SRC_D) ? d_pending_sum_q : i_pending_sum_q),
		.req_mxr       ((ptw_owner_next == PTW_SRC_D) ? d_pending_mxr_q : i_pending_mxr_q),
		.satp_value    (satp_value),
		.mem_req_valid (ptw_mem_req_valid),
		.mem_req_ready (ptw_mem_req_ready),
		.mem_req_addr  (ptw_mem_req_addr),
		.mem_resp_valid(ptw_mem_resp_valid),
		.mem_resp_data (ptw_mem_resp_data),
		.resp_valid    (ptw_resp_valid),
		.resp_fault    (ptw_resp_fault),
		.resp_paddr    (ptw_resp_paddr),
		.refill_valid  (ptw_refill_valid),
		.refill_vpn    (ptw_refill_vpn),
		.refill_ppn    (ptw_refill_ppn),
		.refill_level  (ptw_refill_level),
		.refill_mode   (ptw_refill_mode),
		.refill_asid   (ptw_refill_asid),
		.refill_g      (ptw_refill_g),
		.refill_u      (ptw_refill_u),
		.refill_r      (ptw_refill_r),
		.refill_w      (ptw_refill_w),
		.refill_x      (ptw_refill_x),
		.refill_a      (ptw_refill_a),
		.refill_d      (ptw_refill_d)
	);

endmodule
