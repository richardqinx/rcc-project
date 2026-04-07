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

// File path: rtl/core/csr_unit.sv

module csr_unit
	import rv64_pkg::*;
	import csr_pkg::*;
#(
	parameter logic [XLEN-1:0] HART_ID    = '0,
	parameter logic [XLEN-1:0] VENDOR_ID  = '0,
	parameter logic [XLEN-1:0] ARCH_ID    = '0,
	parameter logic [XLEN-1:0] IMPL_ID    = '0,
	parameter logic [XLEN-1:0] MISA_VALUE = 64'h8000_0000_0050_112D
)(
	input  logic              clk,
	input  logic              rst,
	input  logic              csr_req,
	input  logic [2:0]        csr_op,
	input  logic [11:0]       csr_addr,
	input  logic [XLEN-1:0]   csr_rs1_data,
	input  logic              trap_ctrl_trigger,
	input  logic              trap_ctrl_to_s_mode,
	input  logic [XLEN-1:0]   trap_ctrl_mepc,
	input  logic [XLEN-1:0]   trap_ctrl_mcause,
	input  logic [XLEN-1:0]   trap_ctrl_mtval,
	input  logic              trap_ctrl_mret_en,
	input  logic              trap_ctrl_sret_en,
	output logic [XLEN-1:0]   csr_rdata,
	output logic [XLEN-1:0]   csr_mstatus,
	output logic [XLEN-1:0]   csr_sstatus,
	output logic [XLEN-1:0]   csr_medeleg,
	output logic [XLEN-1:0]   csr_mideleg,
	output logic [XLEN-1:0]   csr_mtvec,
	output logic [XLEN-1:0]   csr_stvec,
	output logic [XLEN-1:0]   csr_mepc,
	output logic [XLEN-1:0]   csr_sepc,
	output logic [XLEN-1:0]   csr_satp,
	output logic [1:0]        csr_priv_mode,
	output logic [XLEN-1:0]   csr_trap_vector,
	output logic              csr_trap_to_s_mode,
	output logic [XLEN-1:0]   csr_trap_vector_next,
	output logic              csr_trap_to_s_mode_next,
	output logic              csr_illegal_access
);

	typedef enum logic [1:0] {
		PRIV_MODE_U = 2'b00,
		PRIV_MODE_S = 2'b01,
		PRIV_MODE_M = 2'b11
	} priv_mode_t;

	localparam int MSTATUS_MIE_BIT  = 3;
	localparam int MSTATUS_MPIE_BIT = 7;
	localparam int MSTATUS_MPP_LO   = 11;
	localparam int MSTATUS_MPP_HI   = 12;
	localparam int SSTATUS_SIE_BIT  = 1;
	localparam int SSTATUS_SPIE_BIT = 5;
	localparam int SSTATUS_SPP_BIT  = 8;

	// Machine-mode CSRs
	logic [XLEN-1:0] mstatus_reg;
	logic [XLEN-1:0] medeleg_reg;
	logic [XLEN-1:0] mideleg_reg;
	logic [XLEN-1:0] mie_reg;
	logic [XLEN-1:0] mtvec_reg;
	logic [XLEN-1:0] mcounteren_reg;
	logic [XLEN-1:0] mscratch_reg;
	logic [XLEN-1:0] mepc_reg;
	logic [XLEN-1:0] mcause_reg;
	logic [XLEN-1:0] mtval_reg;
	logic [XLEN-1:0] mip_reg;

	// Supervisor-mode CSRs
	logic [XLEN-1:0] sstatus_reg;
	logic [XLEN-1:0] sie_reg;
	logic [XLEN-1:0] stvec_reg;
	logic [XLEN-1:0] scounteren_reg;
	logic [XLEN-1:0] sscratch_reg;
	logic [XLEN-1:0] sepc_reg;
	logic [XLEN-1:0] scause_reg;
	logic [XLEN-1:0] stval_reg;
	logic [XLEN-1:0] sip_reg;
	logic [XLEN-1:0] satp_reg;

	// Counters
	logic [XLEN-1:0] mcycle_reg;
	logic [XLEN-1:0] minstret_reg;
	logic [XLEN-1:0] time_reg;

	// Floating-point CSRs
	logic [4:0]       fflags_reg;
	logic [2:0]       frm_reg;

	priv_mode_t      priv_mode_q;

	logic [11:0]     csr_addr_canonical;
	logic            is_write_cmd;
	logic            is_set_cmd;
	logic            is_clear_cmd;
	logic            has_non_zero_data;
	logic            csr_write_en;
	logic [XLEN-1:0] trap_vector_target;
	logic            csr_addr_supported;
	logic            csr_write_illegal;
	logic [XLEN-1:0] csr_read_data;
	logic [XLEN-1:0] trap_vector_q;
	logic            trap_to_s_q;
	logic            trap_taken;
	logic            trap_taken_to_s_mode;
	logic [XLEN-1:0] trap_taken_mepc;
	logic [XLEN-1:0] trap_taken_mcause;
	logic [XLEN-1:0] trap_taken_mtval;

	// Canonicalize addresses, decode operations, and prepare CSR/trap metadata.
	assign csr_addr_canonical = canonical_addr(csr_addr);
	assign is_write_cmd       = (csr_op == 3'b001) || (csr_op == 3'b101);
	assign is_set_cmd         = (csr_op == 3'b010) || (csr_op == 3'b110);
	assign is_clear_cmd       = (csr_op == 3'b011) || (csr_op == 3'b111);
	assign has_non_zero_data  = |csr_rs1_data;
	assign csr_addr_supported = csr_is_supported(csr_addr_canonical);
	assign csr_write_en       = csr_req & csr_addr_supported & (is_write_cmd | ((is_set_cmd | is_clear_cmd) & has_non_zero_data));
	assign trap_taken               = trap_ctrl_trigger;
	assign trap_taken_to_s_mode     = trap_ctrl_to_s_mode;
	assign trap_taken_mepc          = trap_ctrl_mepc;
	assign trap_taken_mcause        = trap_ctrl_mcause;
	assign trap_taken_mtval         = trap_ctrl_mtval;

	assign trap_vector_target      = trap_taken_to_s_mode ? stvec_reg : mtvec_reg;
	assign csr_read_data           = read_csr(csr_addr_canonical);
	assign csr_rdata               = csr_read_data;
	assign csr_mstatus             = mstatus_reg;
	assign csr_sstatus             = sstatus_reg;
	assign csr_medeleg             = medeleg_reg;
	assign csr_mideleg             = mideleg_reg;
	assign csr_mtvec               = mtvec_reg;
	assign csr_stvec               = stvec_reg;
	assign csr_mepc                = mepc_reg;
	assign csr_sepc                = sepc_reg;
	assign csr_satp                = satp_reg;
	assign csr_priv_mode           = priv_mode_q;
	assign csr_trap_vector         = trap_vector_q;
	assign csr_trap_to_s_mode      = trap_to_s_q;
	assign csr_trap_vector_next    = trap_vector_target;
	assign csr_trap_to_s_mode_next = trap_taken_to_s_mode;
	assign csr_write_illegal       = (csr_req && (is_write_cmd || is_set_cmd || is_clear_cmd) && !csr_is_writable(csr_addr_canonical));
	assign csr_illegal_access      = csr_req && (!csr_addr_supported || csr_write_illegal);

	// Main CSR state machine: counts cycles, processes CSR writes, and reacts to traps/returns.
	always_ff @(posedge clk) begin
		if (rst) begin
			mstatus_reg      <= '0;
			medeleg_reg      <= '0;
			mideleg_reg      <= '0;
			mie_reg          <= '0;
			mtvec_reg        <= '0;
			mcounteren_reg   <= '0;
			mscratch_reg     <= '0;
			mepc_reg         <= '0;
			mcause_reg       <= '0;
			mtval_reg        <= '0;
			mip_reg          <= '0;
			sstatus_reg      <= '0;
			sie_reg          <= '0;
			stvec_reg        <= '0;
			scounteren_reg   <= '0;
			sscratch_reg     <= '0;
			sepc_reg         <= '0;
			scause_reg       <= '0;
			stval_reg        <= '0;
			sip_reg          <= '0;
			satp_reg         <= '0;
			mcycle_reg       <= '0;
			minstret_reg     <= '0;
			time_reg         <= '0;
			fflags_reg       <= '0;
			frm_reg          <= '0;
			priv_mode_q      <= PRIV_MODE_M;
			trap_vector_q    <= '0;
			trap_to_s_q      <= 1'b0;
		end else begin
			mcycle_reg <= mcycle_reg + 64'd1;
			time_reg   <= time_reg + 64'd1;
			if (!trap_taken) begin
				minstret_reg <= minstret_reg + 64'd1;
			end

			if (csr_write_en && csr_is_writable(csr_addr_canonical)) begin
				unique case (csr_addr_canonical)
					CSR_MSTATUS: begin
						logic [XLEN-1:0] next_data;
						next_data    = csr_write_next(mstatus_reg, csr_rs1_data, csr_op);
						mstatus_reg  <= next_data;
						sstatus_reg  <= mirror_mstatus_to_sstatus(sstatus_reg, next_data);
					end
					CSR_MEDELEG:    medeleg_reg    <= csr_write_next(medeleg_reg, csr_rs1_data, csr_op);
					CSR_MIDELEG:    mideleg_reg    <= csr_write_next(mideleg_reg, csr_rs1_data, csr_op);
					CSR_MIE:        mie_reg        <= csr_write_next(mie_reg, csr_rs1_data, csr_op);
					CSR_MTVEC:      mtvec_reg      <= csr_write_next(mtvec_reg, csr_rs1_data, csr_op);
					CSR_MSCRATCH:   mscratch_reg   <= csr_write_next(mscratch_reg, csr_rs1_data, csr_op);
					CSR_MEPC:       mepc_reg       <= csr_write_next(mepc_reg, csr_rs1_data, csr_op);
					CSR_MCAUSE:     mcause_reg     <= csr_write_next(mcause_reg, csr_rs1_data, csr_op);
					CSR_MTVAL:      mtval_reg      <= csr_write_next(mtval_reg, csr_rs1_data, csr_op);
					CSR_MIP:        mip_reg        <= csr_write_next(mip_reg, csr_rs1_data, csr_op);
					CSR_MCOUNTEREN: mcounteren_reg <= csr_write_next(mcounteren_reg, csr_rs1_data, csr_op);
					CSR_SSTATUS: begin
						logic [XLEN-1:0] next_sstatus;
						next_sstatus = csr_write_next(sstatus_reg, csr_rs1_data, csr_op);
						sstatus_reg  <= next_sstatus;
						mstatus_reg  <= apply_sstatus_to_mstatus(mstatus_reg, next_sstatus);
					end
					CSR_SIE:        sie_reg        <= csr_write_next(sie_reg, csr_rs1_data, csr_op);
					CSR_STVEC:      stvec_reg      <= csr_write_next(stvec_reg, csr_rs1_data, csr_op);
					CSR_SCOUNTEREN: scounteren_reg <= csr_write_next(scounteren_reg, csr_rs1_data, csr_op);
					CSR_SSCRATCH:   sscratch_reg   <= csr_write_next(sscratch_reg, csr_rs1_data, csr_op);
					CSR_SEPC:       sepc_reg       <= csr_write_next(sepc_reg, csr_rs1_data, csr_op);
					CSR_SCAUSE:     scause_reg     <= csr_write_next(scause_reg, csr_rs1_data, csr_op);
					CSR_STVAL:      stval_reg      <= csr_write_next(stval_reg, csr_rs1_data, csr_op);
					CSR_SIP:        sip_reg        <= csr_write_next(sip_reg, csr_rs1_data, csr_op);
					CSR_SATP:       satp_reg       <= csr_write_next(satp_reg, csr_rs1_data, csr_op);
					CSR_MCYCLE:     mcycle_reg     <= csr_write_next(mcycle_reg, csr_rs1_data, csr_op);
					CSR_MINSTRET:   minstret_reg   <= csr_write_next(minstret_reg, csr_rs1_data, csr_op);
					CSR_FFLAGS: begin
						logic [XLEN-1:0] next_flags;
						next_flags  = csr_write_next({{(XLEN-5){1'b0}}, fflags_reg}, csr_rs1_data, csr_op);
						fflags_reg  <= next_flags[4:0];
					end
					CSR_FRM: begin
						logic [XLEN-1:0] next_frm;
						next_frm    = csr_write_next({{(XLEN-3){1'b0}}, frm_reg}, csr_rs1_data, csr_op);
						frm_reg     <= next_frm[2:0];
					end
					CSR_FCSR: begin
						logic [XLEN-1:0] next_fcsr;
						next_fcsr   = csr_write_next(compose_fcsr(), csr_rs1_data, csr_op);
						fflags_reg  <= next_fcsr[4:0];
						frm_reg     <= next_fcsr[7:5];
					end
					default: ;
				endcase
			end

			if (trap_taken) begin
				trap_vector_q <= trap_vector_target;
				trap_to_s_q   <= trap_taken_to_s_mode;
				// Update privilege-specific CSRs based on whether the trap was delegated.
				if (trap_taken_to_s_mode) begin
					logic [XLEN-1:0] next_sstatus;
					next_sstatus = sstatus_reg;
					next_sstatus[SSTATUS_SPIE_BIT] = sstatus_reg[SSTATUS_SIE_BIT];
					next_sstatus[SSTATUS_SIE_BIT]  = 1'b0;
					next_sstatus[SSTATUS_SPP_BIT]  = (priv_mode_q == PRIV_MODE_S);
					sstatus_reg <= next_sstatus;
					mstatus_reg <= apply_sstatus_to_mstatus(mstatus_reg, next_sstatus);
					sepc_reg    <= trap_taken_mepc;
					scause_reg  <= trap_taken_mcause;
					stval_reg   <= trap_taken_mtval;
					priv_mode_q <= PRIV_MODE_S;
				end else begin
					logic [XLEN-1:0] next_mstatus;
					next_mstatus = mstatus_reg;
					next_mstatus[MSTATUS_MPIE_BIT]              = mstatus_reg[MSTATUS_MIE_BIT];
					next_mstatus[MSTATUS_MIE_BIT]               = 1'b0;
					next_mstatus[MSTATUS_MPP_HI:MSTATUS_MPP_LO] = encode_priv_mode(priv_mode_q);
					mstatus_reg <= next_mstatus;
					sstatus_reg <= mirror_mstatus_to_sstatus(sstatus_reg, next_mstatus);
					mepc_reg    <= trap_taken_mepc;
					mcause_reg  <= trap_taken_mcause;
					mtval_reg   <= trap_taken_mtval;
					priv_mode_q <= PRIV_MODE_M;
				end
			end else begin
				if (trap_ctrl_mret_en) begin
					priv_mode_t return_mode;
					logic [XLEN-1:0] next_mstatus;
					return_mode  = decode_priv_mode(mstatus_reg[MSTATUS_MPP_HI:MSTATUS_MPP_LO]);
					next_mstatus = mstatus_reg;
					next_mstatus[MSTATUS_MIE_BIT]               = mstatus_reg[MSTATUS_MPIE_BIT];
					next_mstatus[MSTATUS_MPIE_BIT]              = 1'b1;
					next_mstatus[MSTATUS_MPP_HI:MSTATUS_MPP_LO] = encode_priv_mode(PRIV_MODE_U);
					mstatus_reg <= next_mstatus;
					sstatus_reg <= mirror_mstatus_to_sstatus(sstatus_reg, next_mstatus);
					priv_mode_q <= return_mode;
				end

				if (trap_ctrl_sret_en) begin
					logic [XLEN-1:0] next_sstatus;
					next_sstatus = sstatus_reg;
					next_sstatus[SSTATUS_SIE_BIT]  = sstatus_reg[SSTATUS_SPIE_BIT];
					next_sstatus[SSTATUS_SPIE_BIT] = 1'b1;
					next_sstatus[SSTATUS_SPP_BIT]  = 1'b0;
					sstatus_reg <= next_sstatus;
					mstatus_reg <= apply_sstatus_to_mstatus(mstatus_reg, next_sstatus);
					priv_mode_q <= (sstatus_reg[SSTATUS_SPP_BIT]) ? PRIV_MODE_S : PRIV_MODE_U;
				end
			end
		end
	end

	// Map CSR aliases (cycle/instret) to their canonical machine addresses.
	function automatic logic [11:0] canonical_addr(input logic [11:0] addr);
		case (addr)
			CSR_CYCLE:   canonical_addr = CSR_MCYCLE;
			CSR_INSTRET: canonical_addr = CSR_MINSTRET;
			default:     canonical_addr = addr;
		endcase
	endfunction

	// Apply the CSR op semantics (write/set/clear) for a single register.
	function automatic logic [XLEN-1:0] csr_write_next(
		input logic [XLEN-1:0] current_data,
		input logic [XLEN-1:0] write_data,
		input logic [2:0]      op_code
	);
		logic [XLEN-1:0] next_data;
		unique case (op_code)
			3'b001, 3'b101: next_data = write_data;                     // CSRRW / CSRRWI
			3'b010, 3'b110: next_data = current_data | write_data;      // CSRRS / CSRRSI
			3'b011, 3'b111: next_data = current_data & ~write_data;     // CSRRC / CSRRCI
			default:         next_data = current_data;
		endcase
		return next_data;
	endfunction

	function automatic bit csr_is_writable(input logic [11:0] addr);
		case (addr)
			CSR_MISA,
			CSR_MVENDORID,
			CSR_MARCHID,
			CSR_MIMPID,
			CSR_MHARTID,
			CSR_TIME: csr_is_writable = 1'b0;
			default:  csr_is_writable = 1'b1;
		endcase
	endfunction

	function automatic bit csr_is_supported(input logic [11:0] addr);
		bit supported;
		unique case (addr)
			CSR_MSTATUS,
			CSR_MISA,
			CSR_MEDELEG,
			CSR_MIDELEG,
			CSR_MIE,
			CSR_MTVEC,
			CSR_MSCRATCH,
			CSR_MEPC,
			CSR_MCAUSE,
			CSR_MTVAL,
			CSR_MIP,
			CSR_MCOUNTEREN,
			CSR_MVENDORID,
			CSR_MARCHID,
			CSR_MIMPID,
			CSR_MHARTID,
			CSR_SSTATUS,
			CSR_SIE,
			CSR_STVEC,
			CSR_SCOUNTEREN,
			CSR_SSCRATCH,
			CSR_SEPC,
			CSR_SCAUSE,
			CSR_STVAL,
			CSR_SIP,
			CSR_SATP,
			CSR_MCYCLE,
			CSR_MINSTRET,
			CSR_CYCLE,
			CSR_INSTRET,
			CSR_TIME,
			CSR_FFLAGS,
			CSR_FRM,
			CSR_FCSR:
				supported = 1'b1;
			default:
				supported = 1'b0;
		endcase
		return supported;
	endfunction

	function automatic logic [XLEN-1:0] apply_sstatus_to_mstatus(
		input logic [XLEN-1:0] mstatus_cur,
		input logic [XLEN-1:0] sstatus_data
	);
		logic [XLEN-1:0] next_data;
		next_data = mstatus_cur;
		next_data[SSTATUS_SIE_BIT]  = sstatus_data[SSTATUS_SIE_BIT];
		next_data[SSTATUS_SPIE_BIT] = sstatus_data[SSTATUS_SPIE_BIT];
		next_data[SSTATUS_SPP_BIT]  = sstatus_data[SSTATUS_SPP_BIT];
		return next_data;
	endfunction

	function automatic logic [XLEN-1:0] mirror_mstatus_to_sstatus(
		input logic [XLEN-1:0] sstatus_cur,
		input logic [XLEN-1:0] mstatus_data
	);
		logic [XLEN-1:0] next_data;
		next_data = sstatus_cur;
		next_data[SSTATUS_SIE_BIT]  = mstatus_data[SSTATUS_SIE_BIT];
		next_data[SSTATUS_SPIE_BIT] = mstatus_data[SSTATUS_SPIE_BIT];
		next_data[SSTATUS_SPP_BIT]  = mstatus_data[SSTATUS_SPP_BIT];
		return next_data;
	endfunction

	// Pack the discrete FP status bits into the architectural FCSR layout.
	function automatic logic [XLEN-1:0] compose_fcsr();
		logic [XLEN-1:0] value;
		value = '0;
		value[4:0] = fflags_reg;
		value[7:5] = frm_reg;
		return value;
	endfunction

	function automatic priv_mode_t decode_priv_mode(input logic [1:0] encoded);
		case (encoded)
			2'b00: decode_priv_mode = PRIV_MODE_U;
			2'b01: decode_priv_mode = PRIV_MODE_S;
			default: decode_priv_mode = PRIV_MODE_M;
		endcase
	endfunction

	function automatic logic [1:0] encode_priv_mode(input priv_mode_t mode);
		case (mode)
			PRIV_MODE_U: encode_priv_mode = 2'b00;
			PRIV_MODE_S: encode_priv_mode = 2'b01;
			default:     encode_priv_mode = 2'b11;
		endcase
	endfunction

	function automatic logic [XLEN-1:0] read_csr(input logic [11:0] addr);
		logic [XLEN-1:0] data_out;
		unique case (addr)
			CSR_FFLAGS:    data_out = {{(XLEN-5){1'b0}}, fflags_reg};
			CSR_FRM:       data_out = {{(XLEN-3){1'b0}}, frm_reg};
			CSR_FCSR:      data_out = compose_fcsr();
			CSR_MSTATUS:    data_out = mstatus_reg;
			CSR_MISA:       data_out = MISA_VALUE;
			CSR_MEDELEG:    data_out = medeleg_reg;
			CSR_MIDELEG:    data_out = mideleg_reg;
			CSR_MIE:        data_out = mie_reg;
			CSR_MTVEC:      data_out = mtvec_reg;
			CSR_MSCRATCH:   data_out = mscratch_reg;
			CSR_MEPC:       data_out = mepc_reg;
			CSR_MCAUSE:     data_out = mcause_reg;
			CSR_MTVAL:      data_out = mtval_reg;
			CSR_MIP:        data_out = mip_reg;
			CSR_MCOUNTEREN: data_out = mcounteren_reg;
			CSR_MVENDORID:  data_out = VENDOR_ID;
			CSR_MARCHID:    data_out = ARCH_ID;
			CSR_MIMPID:     data_out = IMPL_ID;
			CSR_MHARTID:    data_out = HART_ID;
			CSR_SSTATUS:    data_out = sstatus_reg;
			CSR_SIE:        data_out = sie_reg;
			CSR_STVEC:      data_out = stvec_reg;
			CSR_SCOUNTEREN: data_out = scounteren_reg;
			CSR_SSCRATCH:   data_out = sscratch_reg;
			CSR_SEPC:       data_out = sepc_reg;
			CSR_SCAUSE:     data_out = scause_reg;
			CSR_STVAL:      data_out = stval_reg;
			CSR_SIP:        data_out = sip_reg;
			CSR_SATP:       data_out = satp_reg;
			CSR_MCYCLE:     data_out = mcycle_reg;
			CSR_MINSTRET:   data_out = minstret_reg;
			CSR_CYCLE:      data_out = mcycle_reg;
			CSR_INSTRET:    data_out = minstret_reg;
			CSR_TIME:       data_out = time_reg;
			default:        data_out = '0;
		endcase
		return data_out;
	endfunction

endmodule
