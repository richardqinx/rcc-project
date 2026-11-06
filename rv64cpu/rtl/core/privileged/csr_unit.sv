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
	parameter logic [XLEN-1:0] HART_ID         = '0,
	parameter logic [XLEN-1:0] VENDOR_ID       = '0,
	parameter logic [XLEN-1:0] ARCH_ID         = '0,
	parameter logic [XLEN-1:0] IMPL_ID         = '0,
	parameter logic [XLEN-1:0] MISA_VALUE      = 64'h8000_0000_0014_11ad,
	parameter int              PHYS_ADDR_WIDTH = 56,
	parameter int              PMP_GRANULARITY = 0
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
	input  logic              m_timer_interrupt,
	input  logic              m_software_interrupt,
	input  logic              m_external_interrupt,
	input  logic              s_timer_interrupt,
	input  logic              s_software_interrupt,
	input  logic              s_external_interrupt,
	input  logic              fp_state_dirty,
	input  logic              fp_fflags_valid,
	input  logic [4:0]        fp_fflags,
	output logic [XLEN-1:0]   csr_rdata,
	output logic [XLEN-1:0]   csr_mstatus,
	output logic [XLEN-1:0]   csr_sstatus,
	output logic [XLEN-1:0]   csr_medeleg,
	output logic [XLEN-1:0]   csr_mideleg,
	output logic [XLEN-1:0]   csr_mie,
	output logic [XLEN-1:0]   csr_sie,
	output logic [XLEN-1:0]   csr_mip,
	output logic [XLEN-1:0]   csr_sip,
	output logic [XLEN-1:0]   csr_mtvec,
	output logic [XLEN-1:0]   csr_stvec,
	output logic [XLEN-1:0]   csr_mepc,
	output logic [XLEN-1:0]   csr_sepc,
	output logic [XLEN-1:0]   csr_satp,
	output logic [2:0]        csr_frm,
	output logic [1:0]        csr_priv_mode,
	output logic [XLEN-1:0]   csr_trap_vector,
	output logic              csr_trap_to_s_mode,
	output logic [XLEN-1:0]   csr_trap_vector_next,
	output logic              csr_trap_to_s_mode_next,
	output pmpcfg_entry_t     csr_pmpcfg_entries [PMP_ENTRY_COUNT],
	output pmpaddr_entry_t    csr_pmpaddr_entries [PMP_ENTRY_COUNT],
	output logic              csr_illegal_access
);

	typedef enum logic [1:0] {
		PRIV_MODE_U = 2'b00,
		PRIV_MODE_S = 2'b01,
		PRIV_MODE_M = 2'b11
	} priv_mode_t;

	localparam int MSTATUS_MIE_BIT  = 3;
	localparam int MSTATUS_MPIE_BIT = 7;
	localparam int MSTATUS_VS_LO    = 9;
	localparam int MSTATUS_VS_HI    = 10;
	localparam int MSTATUS_MPP_LO   = 11;
	localparam int MSTATUS_MPP_HI   = 12;
	localparam int MSTATUS_FS_LO    = 13;
	localparam int MSTATUS_FS_HI    = 14;
	localparam int MSTATUS_XS_LO    = 15;
	localparam int MSTATUS_XS_HI    = 16;
	localparam int MSTATUS_MPRV_BIT = 17;
	localparam int MSTATUS_SUM_BIT  = 18;
	localparam int MSTATUS_MXR_BIT  = 19;
	localparam int MSTATUS_TVM_BIT  = 20;
	localparam int MSTATUS_TW_BIT   = 21;
	localparam int MSTATUS_TSR_BIT  = 22;
	localparam int MSTATUS_UXL_LO   = 32;
	localparam int MSTATUS_UXL_HI   = 33;
	localparam int MSTATUS_SXL_LO   = 34;
	localparam int MSTATUS_SXL_HI   = 35;
	localparam int MSTATUS_SD_BIT   = 63;
	localparam logic [1:0] STATUS_XLEN_64 = 2'b10;
	localparam int SSTATUS_SIE_BIT  = 1;
	localparam int SSTATUS_SPIE_BIT = 5;
	localparam int SSTATUS_SPP_BIT  = 8;
	localparam int SSTATUS_VS_LO    = MSTATUS_VS_LO;
	localparam int SSTATUS_VS_HI    = MSTATUS_VS_HI;
	localparam int SSTATUS_FS_LO    = MSTATUS_FS_LO;
	localparam int SSTATUS_FS_HI    = MSTATUS_FS_HI;
	localparam int SSTATUS_XS_LO    = MSTATUS_XS_LO;
	localparam int SSTATUS_XS_HI    = MSTATUS_XS_HI;
	localparam int SSTATUS_SUM_BIT  = MSTATUS_SUM_BIT;
	localparam int SSTATUS_MXR_BIT  = MSTATUS_MXR_BIT;
	localparam int SSTATUS_UXL_LO   = MSTATUS_UXL_LO;
	localparam int SSTATUS_UXL_HI   = MSTATUS_UXL_HI;
	localparam int SSTATUS_SD_BIT   = MSTATUS_SD_BIT;
	localparam int SATP_MODE_LO     = 60;
	localparam int SATP_MODE_HI     = 63;
	localparam int SATP_ASID_LO     = 44;
	localparam int SATP_ASID_HI     = 59;
	localparam int SATP_PPN_LO      = 0;
	localparam int SATP_PPN_HI      = 43;
	localparam logic [3:0] SATP_MODE_BARE = 4'd0;
	localparam logic [3:0] SATP_MODE_SV39 = 4'd8;
	localparam logic [3:0] SATP_MODE_SV48 = 4'd9;  // Stub: unsupported until Sv48 is implemented.
	localparam logic [3:0] SATP_MODE_SV57 = 4'd10; // Stub: unsupported until Sv57 is implemented.
	localparam logic [3:0] SATP_MODE_SV64 = 4'd11; // Reserved/future stub.
	localparam int MCOUNTINHIBIT_CY_BIT = 0;
	localparam int MCOUNTINHIBIT_IR_BIT = 2;
	localparam int COUNTEREN_CY_BIT = 0;
	localparam int COUNTEREN_TM_BIT = 1;
	localparam int COUNTEREN_IR_BIT = 2;
	localparam int HPM_COUNTER_COUNT = 29;
	localparam int INTERRUPT_S_SOFTWARE_BIT = int'(INTERRUPT_S_SOFTWARE);
	localparam int INTERRUPT_S_TIMER_BIT    = int'(INTERRUPT_S_TIMER);
	localparam int INTERRUPT_S_EXTERNAL_BIT = int'(INTERRUPT_S_EXTERNAL);
	localparam int INTERRUPT_M_SOFTWARE_BIT = int'(INTERRUPT_M_SOFTWARE);
	localparam int INTERRUPT_M_TIMER_BIT    = int'(INTERRUPT_M_TIMER);
	localparam int INTERRUPT_M_EXTERNAL_BIT = int'(INTERRUPT_M_EXTERNAL);
	localparam logic [XLEN-1:0] S_INTERRUPT_MASK =
		(64'h1 << INTERRUPT_S_SOFTWARE) |
		(64'h1 << INTERRUPT_S_TIMER) |
		(64'h1 << INTERRUPT_S_EXTERNAL);
	localparam logic [XLEN-1:0] M_INTERRUPT_MASK =
		(64'h1 << INTERRUPT_M_SOFTWARE) |
		(64'h1 << INTERRUPT_M_TIMER) |
		(64'h1 << INTERRUPT_M_EXTERNAL);
	localparam logic [XLEN-1:0] INTERRUPT_MASK = S_INTERRUPT_MASK | M_INTERRUPT_MASK;

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
	logic [XLEN-1:0] mtinst_reg;
	logic [XLEN-1:0] mtval2_reg;
	logic [XLEN-1:0] menvcfg_reg;
	logic [XLEN-1:0] mstateen_reg [4];
	pmpcfg_entry_t  pmpcfg_entries [PMP_ENTRY_COUNT];
	pmpaddr_entry_t pmpaddr_entries [PMP_ENTRY_COUNT];

	// Supervisor-mode CSRs
	logic [XLEN-1:0] sstatus_reg;
	logic [XLEN-1:0] stvec_reg;
	logic [XLEN-1:0] scounteren_reg;
	logic [XLEN-1:0] senvcfg_reg;
	logic [XLEN-1:0] sstateen_reg [4];
	logic [XLEN-1:0] sscratch_reg;
	logic [XLEN-1:0] sepc_reg;
	logic [XLEN-1:0] scause_reg;
	logic [XLEN-1:0] stval_reg;
	logic [XLEN-1:0] satp_reg;
	logic [XLEN-1:0] hstatus_reg;
	logic [XLEN-1:0] hedeleg_reg;
	logic [XLEN-1:0] hideleg_reg;
	logic [XLEN-1:0] hie_reg;
	logic [XLEN-1:0] htimedelta_reg;
	logic [XLEN-1:0] hcounteren_reg;
	logic [XLEN-1:0] hgeie_reg;
	logic [XLEN-1:0] henvcfg_reg;
	logic [XLEN-1:0] hstateen_reg [4];
	logic [XLEN-1:0] htval_reg;
	logic [XLEN-1:0] hip_reg;
	logic [XLEN-1:0] hvip_reg;
	logic [XLEN-1:0] htinst_reg;
	logic [XLEN-1:0] hgatp_reg;
	logic [XLEN-1:0] vsstatus_reg;
	logic [XLEN-1:0] vsie_reg;
	logic [XLEN-1:0] vstvec_reg;
	logic [XLEN-1:0] vsscratch_reg;
	logic [XLEN-1:0] vsepc_reg;
	logic [XLEN-1:0] vscause_reg;
	logic [XLEN-1:0] vstval_reg;
	logic [XLEN-1:0] vsip_reg;
	logic [XLEN-1:0] vsatp_reg;

	// Counters
	logic [XLEN-1:0] mcycle_reg;
	logic [XLEN-1:0] minstret_reg;
	logic [XLEN-1:0] time_reg;
	logic [XLEN-1:0] mcountinhibit_reg;
	logic [XLEN-1:0] mhpmcounter_reg [HPM_COUNTER_COUNT];

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
	logic            csr_privilege_illegal;
	logic            csr_write_illegal;
	logic            csr_stateen_illegal;
	logic [XLEN-1:0] csr_read_data;
	logic [XLEN-1:0] mip_value;
	logic [XLEN-1:0] sip_value;
	logic [XLEN-1:0] sie_value;
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
	assign csr_write_en       = csr_req & csr_addr_supported & !csr_privilege_illegal &
	                            !csr_write_illegal & !csr_stateen_illegal &
	                            (is_write_cmd | ((is_set_cmd | is_clear_cmd) & has_non_zero_data));
	assign trap_taken               = trap_ctrl_trigger;
	assign trap_taken_to_s_mode     = trap_ctrl_to_s_mode;
	assign trap_taken_mepc          = trap_ctrl_mepc;
	assign trap_taken_mcause        = trap_ctrl_mcause;
	assign trap_taken_mtval         = trap_ctrl_mtval;

	assign trap_vector_target      = trap_taken_to_s_mode ? stvec_reg : mtvec_reg;
	assign csr_read_data           = read_csr(csr_addr_canonical);
	assign csr_rdata               = csr_read_data;
	assign mip_value               = compose_mip();
	assign sip_value               = mip_value & S_INTERRUPT_MASK;
	assign sie_value               = mie_reg & S_INTERRUPT_MASK;
	assign csr_mstatus             = mstatus_reg;
	assign csr_sstatus             = sstatus_reg;
	assign csr_medeleg             = medeleg_reg;
	assign csr_mideleg             = mideleg_reg;
	assign csr_mie                 = mie_reg;
	assign csr_sie                 = sie_value;
	assign csr_mip                 = mip_value;
	assign csr_sip                 = sip_value;
	assign csr_mtvec               = mtvec_reg;
	assign csr_stvec               = stvec_reg;
	assign csr_mepc                = mepc_reg;
	assign csr_sepc                = sepc_reg;
	assign csr_satp                = satp_reg;
	assign csr_frm                 = frm_reg;
	assign csr_priv_mode           = priv_mode_q;
	assign csr_trap_vector         = trap_vector_q;
	assign csr_trap_to_s_mode      = trap_to_s_q;
	assign csr_trap_vector_next    = trap_vector_target;
	assign csr_trap_to_s_mode_next = trap_taken_to_s_mode;
	assign csr_write_illegal       = csr_req && csr_is_read_only_by_encoding(csr_addr) &&
	                                 (is_write_cmd || ((is_set_cmd || is_clear_cmd) && has_non_zero_data));
	assign csr_privilege_illegal   = csr_req && csr_addr_supported &&
	                                 !csr_privilege_allows_access(csr_addr, csr_addr_canonical);
	assign csr_stateen_illegal     = csr_req && csr_access_blocked_by_stateen(csr_addr, csr_addr_canonical);
	assign csr_illegal_access      = csr_req && (!csr_addr_supported || csr_privilege_illegal ||
	                                             csr_write_illegal || csr_stateen_illegal);

	for (genvar pmp_entry_idx = 0; pmp_entry_idx < PMP_ENTRY_COUNT; pmp_entry_idx++) begin : gen_pmp_outputs
		assign csr_pmpcfg_entries[pmp_entry_idx] = pmpcfg_entries[pmp_entry_idx];
		assign csr_pmpaddr_entries[pmp_entry_idx] = pmpaddr_entries[pmp_entry_idx];
	end

	// Main CSR state machine: counts cycles, processes CSR writes, and reacts to traps/returns.
	always_ff @(posedge clk) begin
		if (rst) begin
			mstatus_reg      <= reset_mstatus_value();
			medeleg_reg      <= legalize_medeleg('0);
			mideleg_reg      <= '0;
			mie_reg          <= '0;
			mtvec_reg        <= '0;
			mcounteren_reg   <= '0;
			mscratch_reg     <= '0;
			mepc_reg         <= '0;
			mcause_reg       <= '0;
			mtval_reg        <= '0;
			mip_reg          <= '0;
			mtinst_reg       <= '0;
			mtval2_reg       <= '0;
			menvcfg_reg      <= '0;
			for (int unsigned stateen_idx = 0; stateen_idx < 4; stateen_idx++) begin
				mstateen_reg[stateen_idx] <= '0;
				sstateen_reg[stateen_idx] <= '0;
				hstateen_reg[stateen_idx] <= '0;
			end
			for (int unsigned pmp_entry_idx = 0; pmp_entry_idx < PMP_ENTRY_COUNT; pmp_entry_idx++) begin
				pmpcfg_entries[pmp_entry_idx] <= '0;
				pmpaddr_entries[pmp_entry_idx] <= '0;
			end
			sstatus_reg      <= reset_sstatus_value();
			stvec_reg        <= '0;
			scounteren_reg   <= '0;
			senvcfg_reg      <= '0;
			sscratch_reg     <= '0;
			sepc_reg         <= '0;
			scause_reg       <= '0;
			stval_reg        <= '0;
			satp_reg         <= '0;
			hstatus_reg      <= '0;
			hedeleg_reg      <= '0;
			hideleg_reg      <= '0;
			hie_reg          <= '0;
			htimedelta_reg   <= '0;
			hcounteren_reg   <= '0;
			hgeie_reg        <= '0;
			henvcfg_reg      <= '0;
			htval_reg        <= '0;
			hip_reg          <= '0;
			hvip_reg         <= '0;
			htinst_reg       <= '0;
			hgatp_reg        <= '0;
			vsstatus_reg     <= reset_sstatus_value();
			vsie_reg         <= '0;
			vstvec_reg       <= '0;
			vsscratch_reg    <= '0;
			vsepc_reg        <= '0;
			vscause_reg      <= '0;
			vstval_reg       <= '0;
			vsip_reg         <= '0;
			vsatp_reg        <= '0;
			mcycle_reg         <= '0;
			minstret_reg       <= '0;
			time_reg           <= '0;
			mcountinhibit_reg  <= '0;
			for (int unsigned hpm_idx = 0; hpm_idx < HPM_COUNTER_COUNT; hpm_idx++) begin
				mhpmcounter_reg[hpm_idx] <= '0;
			end
			fflags_reg       <= '0;
			frm_reg          <= '0;
			priv_mode_q      <= PRIV_MODE_M;
			trap_vector_q    <= '0;
			trap_to_s_q      <= 1'b0;
		end else begin
			if (!mcountinhibit_reg[MCOUNTINHIBIT_CY_BIT]) begin
				mcycle_reg <= mcycle_reg + 64'd1;
			end
			time_reg <= time_reg + 64'd1;
			if (!trap_taken && !mcountinhibit_reg[MCOUNTINHIBIT_IR_BIT]) begin
				minstret_reg <= minstret_reg + 64'd1;
			end

			if (csr_write_en && csr_is_writable(csr_addr_canonical)) begin
				if (csr_is_pmpcfg(csr_addr_canonical)) begin
					write_pmpcfg_csr(csr_addr_canonical, csr_write_next(read_csr(csr_addr_canonical), csr_rs1_data, csr_op));
				end else if (csr_is_pmpaddr(csr_addr_canonical)) begin
					write_pmpaddr_csr(csr_addr_canonical, csr_write_next(read_csr(csr_addr_canonical), csr_rs1_data, csr_op));
				end else if (csr_is_mhpmcounter(csr_addr_canonical)) begin
					mhpmcounter_reg[hpmcounter_index(csr_addr_canonical)] <=
						csr_write_next(read_csr(csr_addr_canonical), csr_rs1_data, csr_op);
				end else begin
					unique case (csr_addr_canonical)
						CSR_MSTATUS: begin
							logic [XLEN-1:0] next_data;
							logic [XLEN-1:0] legal_data;
							next_data    = csr_write_next(mstatus_reg, csr_rs1_data, csr_op);
							legal_data   = legalize_mstatus(next_data);
							mstatus_reg  <= legal_data;
							sstatus_reg  <= mirror_mstatus_to_sstatus(sstatus_reg, legal_data);
						end
						CSR_MEDELEG:    medeleg_reg    <= legalize_medeleg(csr_write_next(medeleg_reg, csr_rs1_data, csr_op));
						CSR_MIDELEG:    mideleg_reg    <= legalize_mideleg(csr_write_next(mideleg_reg, csr_rs1_data, csr_op));
						CSR_MIE:        mie_reg        <= legalize_interrupt_csr(csr_write_next(mie_reg, csr_rs1_data, csr_op));
						CSR_MTVEC:      mtvec_reg      <= csr_write_next(mtvec_reg, csr_rs1_data, csr_op);
						CSR_MSCRATCH:   mscratch_reg   <= csr_write_next(mscratch_reg, csr_rs1_data, csr_op);
						CSR_MEPC:       mepc_reg       <= csr_write_next(mepc_reg, csr_rs1_data, csr_op);
						CSR_MCAUSE:     mcause_reg     <= csr_write_next(mcause_reg, csr_rs1_data, csr_op);
						CSR_MTVAL:      mtval_reg      <= csr_write_next(mtval_reg, csr_rs1_data, csr_op);
						CSR_MIP:        mip_reg        <= legalize_interrupt_csr(csr_write_next(mip_reg, csr_rs1_data, csr_op));
						CSR_MTINST:     mtinst_reg     <= '0;
						CSR_MTVAL2:     mtval2_reg     <= csr_write_next(mtval2_reg, csr_rs1_data, csr_op);
						CSR_MCOUNTEREN: mcounteren_reg <= legalize_counteren(csr_write_next(mcounteren_reg, csr_rs1_data, csr_op));
						CSR_MCOUNTINHIBIT: mcountinhibit_reg <= legalize_mcountinhibit(csr_write_next(mcountinhibit_reg, csr_rs1_data, csr_op));
						CSR_MENVCFG:    menvcfg_reg    <= legalize_menvcfg(csr_write_next(menvcfg_reg, csr_rs1_data, csr_op));
						CSR_MSTATEEN0:  mstateen_reg[0] <= legalize_mstateen(0, csr_write_next(mstateen_reg[0], csr_rs1_data, csr_op));
						CSR_MSTATEEN1:  mstateen_reg[1] <= legalize_mstateen(1, csr_write_next(mstateen_reg[1], csr_rs1_data, csr_op));
						CSR_MSTATEEN2:  mstateen_reg[2] <= legalize_mstateen(2, csr_write_next(mstateen_reg[2], csr_rs1_data, csr_op));
						CSR_MSTATEEN3:  mstateen_reg[3] <= legalize_mstateen(3, csr_write_next(mstateen_reg[3], csr_rs1_data, csr_op));
						CSR_SSTATUS: begin
							logic [XLEN-1:0] next_sstatus;
							next_sstatus = csr_write_next(sstatus_reg, csr_rs1_data, csr_op);
							sstatus_reg  <= legalize_sstatus(next_sstatus);
							mstatus_reg  <= apply_sstatus_to_mstatus(mstatus_reg, next_sstatus);
						end
						CSR_SIE:        mie_reg        <= (mie_reg & ~S_INTERRUPT_MASK) |
						                                  (legalize_interrupt_csr(csr_write_next(sie_value, csr_rs1_data, csr_op)) &
						                                   S_INTERRUPT_MASK);
						CSR_STVEC:      stvec_reg      <= csr_write_next(stvec_reg, csr_rs1_data, csr_op);
						CSR_SCOUNTEREN: scounteren_reg <= legalize_counteren(csr_write_next(scounteren_reg, csr_rs1_data, csr_op));
						CSR_SENVCFG:    senvcfg_reg    <= legalize_senvcfg(csr_write_next(senvcfg_reg, csr_rs1_data, csr_op));
						CSR_SSTATEEN0:  sstateen_reg[0] <= legalize_sstateen(0, csr_write_next(sstateen_reg[0], csr_rs1_data, csr_op));
						CSR_SSTATEEN1:  sstateen_reg[1] <= legalize_sstateen(1, csr_write_next(sstateen_reg[1], csr_rs1_data, csr_op));
						CSR_SSTATEEN2:  sstateen_reg[2] <= legalize_sstateen(2, csr_write_next(sstateen_reg[2], csr_rs1_data, csr_op));
						CSR_SSTATEEN3:  sstateen_reg[3] <= legalize_sstateen(3, csr_write_next(sstateen_reg[3], csr_rs1_data, csr_op));
						CSR_SSCRATCH:   sscratch_reg   <= csr_write_next(sscratch_reg, csr_rs1_data, csr_op);
						CSR_SEPC:       sepc_reg       <= csr_write_next(sepc_reg, csr_rs1_data, csr_op);
						CSR_SCAUSE:     scause_reg     <= csr_write_next(scause_reg, csr_rs1_data, csr_op);
						CSR_STVAL:      stval_reg      <= csr_write_next(stval_reg, csr_rs1_data, csr_op);
						CSR_SIP:        mip_reg        <= (mip_reg & ~S_INTERRUPT_MASK) |
						                                  (legalize_interrupt_csr(csr_write_next(sip_value, csr_rs1_data, csr_op)) &
						                                   S_INTERRUPT_MASK);
						CSR_SATP: begin
							logic [XLEN-1:0] next_satp;
							next_satp = csr_write_next(satp_reg, csr_rs1_data, csr_op);
							satp_reg  <= legalize_satp(next_satp, satp_reg);
						end
						CSR_HSTATUS:    hstatus_reg    <= legalize_hstatus(csr_write_next(hstatus_reg, csr_rs1_data, csr_op));
						CSR_HEDELEG:    hedeleg_reg    <= legalize_medeleg(csr_write_next(hedeleg_reg, csr_rs1_data, csr_op));
						CSR_HIDELEG:    hideleg_reg    <= legalize_mideleg(csr_write_next(hideleg_reg, csr_rs1_data, csr_op));
						CSR_HIE:        hie_reg        <= '0;
						CSR_HTIMEDELTA: htimedelta_reg <= csr_write_next(htimedelta_reg, csr_rs1_data, csr_op);
						CSR_HCOUNTEREN: hcounteren_reg <= legalize_counteren(csr_write_next(hcounteren_reg, csr_rs1_data, csr_op));
						CSR_HGEIE:      hgeie_reg      <= '0;
						CSR_HENVCFG:    henvcfg_reg    <= legalize_menvcfg(csr_write_next(henvcfg_reg, csr_rs1_data, csr_op));
						CSR_HSTATEEN0:  hstateen_reg[0] <= legalize_mstateen(0, csr_write_next(hstateen_reg[0], csr_rs1_data, csr_op));
						CSR_HSTATEEN1:  hstateen_reg[1] <= legalize_mstateen(1, csr_write_next(hstateen_reg[1], csr_rs1_data, csr_op));
						CSR_HSTATEEN2:  hstateen_reg[2] <= legalize_mstateen(2, csr_write_next(hstateen_reg[2], csr_rs1_data, csr_op));
						CSR_HSTATEEN3:  hstateen_reg[3] <= legalize_mstateen(3, csr_write_next(hstateen_reg[3], csr_rs1_data, csr_op));
						CSR_HTVAL:      htval_reg      <= csr_write_next(htval_reg, csr_rs1_data, csr_op);
						CSR_HIP:        hip_reg        <= '0;
						CSR_HVIP:       hvip_reg       <= '0;
						CSR_HTINST:     htinst_reg     <= '0;
						CSR_HGATP:      hgatp_reg      <= legalize_hgatp(csr_write_next(hgatp_reg, csr_rs1_data, csr_op), hgatp_reg);
						CSR_VSSTATUS:   vsstatus_reg   <= legalize_sstatus(csr_write_next(vsstatus_reg, csr_rs1_data, csr_op));
						CSR_VSIE:       vsie_reg       <= '0;
						CSR_VSTVEC:     vstvec_reg     <= csr_write_next(vstvec_reg, csr_rs1_data, csr_op);
						CSR_VSSCRATCH:  vsscratch_reg  <= csr_write_next(vsscratch_reg, csr_rs1_data, csr_op);
						CSR_VSEPC:      vsepc_reg      <= csr_write_next(vsepc_reg, csr_rs1_data, csr_op);
						CSR_VSCAUSE:    vscause_reg    <= csr_write_next(vscause_reg, csr_rs1_data, csr_op);
						CSR_VSTVAL:     vstval_reg     <= csr_write_next(vstval_reg, csr_rs1_data, csr_op);
						CSR_VSIP:       vsip_reg       <= '0;
						CSR_VSATP: begin
							logic [XLEN-1:0] next_vsatp;
							next_vsatp = csr_write_next(vsatp_reg, csr_rs1_data, csr_op);
							vsatp_reg  <= legalize_satp(next_vsatp, vsatp_reg);
						end
						CSR_MCYCLE:     mcycle_reg     <= csr_write_next(mcycle_reg, csr_rs1_data, csr_op);
						CSR_MINSTRET:   minstret_reg   <= csr_write_next(minstret_reg, csr_rs1_data, csr_op);
						CSR_FFLAGS: begin
							logic [XLEN-1:0] next_flags;
							next_flags  = csr_write_next({{(XLEN-5){1'b0}}, fflags_reg}, csr_rs1_data, csr_op);
							fflags_reg  <= next_flags[4:0];
							mstatus_reg <= set_mstatus_fs_dirty(mstatus_reg);
							sstatus_reg <= set_sstatus_fs_dirty(sstatus_reg);
						end
						CSR_FRM: begin
							logic [XLEN-1:0] next_frm;
							next_frm    = csr_write_next({{(XLEN-3){1'b0}}, frm_reg}, csr_rs1_data, csr_op);
							frm_reg     <= next_frm[2:0];
							mstatus_reg <= set_mstatus_fs_dirty(mstatus_reg);
							sstatus_reg <= set_sstatus_fs_dirty(sstatus_reg);
						end
						CSR_FCSR: begin
							logic [XLEN-1:0] next_fcsr;
							next_fcsr   = csr_write_next(compose_fcsr(), csr_rs1_data, csr_op);
							fflags_reg  <= next_fcsr[4:0];
							frm_reg     <= next_fcsr[7:5];
							mstatus_reg <= set_mstatus_fs_dirty(mstatus_reg);
							sstatus_reg <= set_sstatus_fs_dirty(sstatus_reg);
						end
						default: ;
					endcase
				end
			end

			if (fp_state_dirty) begin
				mstatus_reg <= set_mstatus_fs_dirty(mstatus_reg);
				sstatus_reg <= set_sstatus_fs_dirty(sstatus_reg);
			end

			if (fp_fflags_valid) begin
				fflags_reg <= fflags_reg | fp_fflags;
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
					mstatus_reg <= legalize_mstatus(next_mstatus);
					sstatus_reg <= mirror_mstatus_to_sstatus(sstatus_reg, legalize_mstatus(next_mstatus));
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
					if (return_mode != PRIV_MODE_M) begin
						next_mstatus[MSTATUS_MPRV_BIT] = 1'b0;
					end
					mstatus_reg <= legalize_mstatus(next_mstatus);
					sstatus_reg <= mirror_mstatus_to_sstatus(sstatus_reg, legalize_mstatus(next_mstatus));
					priv_mode_q <= return_mode;
				end

				if (trap_ctrl_sret_en) begin
					logic [XLEN-1:0] next_sstatus;
					logic [XLEN-1:0] next_mstatus;
					next_sstatus = sstatus_reg;
					next_sstatus[SSTATUS_SIE_BIT]  = sstatus_reg[SSTATUS_SPIE_BIT];
					next_sstatus[SSTATUS_SPIE_BIT] = 1'b1;
					next_sstatus[SSTATUS_SPP_BIT]  = 1'b0;
					next_mstatus = apply_sstatus_to_mstatus(mstatus_reg, next_sstatus);
					next_mstatus[MSTATUS_MPRV_BIT] = 1'b0;
					sstatus_reg <= legalize_sstatus(next_sstatus);
					mstatus_reg <= legalize_mstatus(next_mstatus);
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

	function automatic bit csr_is_pmpcfg(input logic [11:0] addr);
		csr_is_pmpcfg = (addr >= CSR_PMPCFG0) && (addr <= CSR_PMPCFG15) && (addr[0] == 1'b0);
	endfunction

	function automatic bit csr_is_pmpaddr(input logic [11:0] addr);
		csr_is_pmpaddr = (addr >= CSR_PMPADDR0) && (addr <= CSR_PMPADDR63);
	endfunction

	function automatic bit csr_is_mhpmcounter(input logic [11:0] addr);
		csr_is_mhpmcounter = (addr >= CSR_MHPMCOUNTER3) && (addr <= CSR_MHPMCOUNTER31);
	endfunction

	function automatic bit csr_is_hpmcounter(input logic [11:0] addr);
		csr_is_hpmcounter = (addr >= CSR_HPMCOUNTER3) && (addr <= CSR_HPMCOUNTER31);
	endfunction

	function automatic bit csr_is_mhpmevent(input logic [11:0] addr);
		csr_is_mhpmevent = (addr >= CSR_MHPMEVENT3) && (addr <= CSR_MHPMEVENT31);
	endfunction

	function automatic bit csr_is_counter(input logic [11:0] addr);
		return (addr == CSR_CYCLE) || (addr == CSR_TIME) || (addr == CSR_INSTRET) || csr_is_hpmcounter(addr);
	endfunction

	function automatic int unsigned counter_index(input logic [11:0] addr);
		if (csr_is_hpmcounter(addr)) begin
			return int'(addr) - int'(CSR_CYCLE);
		end
		case (addr)
			CSR_CYCLE:   return COUNTEREN_CY_BIT;
			CSR_TIME:    return COUNTEREN_TM_BIT;
			CSR_INSTRET: return COUNTEREN_IR_BIT;
			default:     return COUNTEREN_CY_BIT;
		endcase
	endfunction

	function automatic int unsigned hpmcounter_index(input logic [11:0] addr);
		if (csr_is_hpmcounter(addr)) begin
			return int'(addr) - int'(CSR_HPMCOUNTER3);
		end
		return int'(addr) - int'(CSR_MHPMCOUNTER3);
	endfunction

	function automatic int unsigned pmpcfg_csr_base_entry(input logic [11:0] addr);
		return (int'(addr) - int'(CSR_PMPCFG0)) * 4;
	endfunction

	function automatic int unsigned pmpaddr_csr_entry(input logic [11:0] addr);
		return int'(addr) - int'(CSR_PMPADDR0);
	endfunction

	function automatic logic [XLEN-1:0] pmpaddr_writable_mask();
		logic [XLEN-1:0] mask;
		mask = '0;
		for (int bit_idx = 0; bit_idx < (PHYS_ADDR_WIDTH - 2); bit_idx++) begin
			mask[bit_idx] = 1'b1;
		end
		return mask;
	endfunction

	function automatic bit pmpcfg_byte_is_legal(input pmpcfg_entry_t cfg);
		logic [1:0] address_matching;
		address_matching = cfg[PMPCFG_A_HI:PMPCFG_A_LO];
		if (!cfg[PMPCFG_R_BIT] && cfg[PMPCFG_W_BIT]) begin
			return 1'b0;
		end
		if ((PMP_GRANULARITY >= 2) && (address_matching == PMP_MODE_NA4)) begin
			return 1'b0;
		end
		return 1'b1;
	endfunction

	function automatic pmpcfg_entry_t legalize_pmpcfg_byte(input pmpcfg_entry_t cfg);
		pmpcfg_entry_t legal_cfg;
		legal_cfg = cfg;
		legal_cfg[6:5] = 2'b00;
		return legal_cfg;
	endfunction

	function automatic bit pmpaddr_is_locked(input int unsigned entry_idx);
		bit locked;
		locked = pmpcfg_entries[entry_idx][PMPCFG_L_BIT];
		if (entry_idx < (PMP_ENTRY_COUNT - 1)) begin
			locked |= pmpcfg_entries[entry_idx + 1][PMPCFG_L_BIT] &&
			          (pmpcfg_entries[entry_idx + 1][PMPCFG_A_HI:PMPCFG_A_LO] == PMP_MODE_TOR);
		end
		return locked;
	endfunction

	function automatic logic [XLEN-1:0] read_pmpcfg_csr(input logic [11:0] addr);
		logic [XLEN-1:0] data;
		int unsigned base_entry;
		data = '0;
		base_entry = pmpcfg_csr_base_entry(addr);
		for (int byte_idx = 0; byte_idx < PMP_CFG_ENTRIES_PER_RV64_CSR; byte_idx++) begin
			if ((base_entry + byte_idx) < PMP_ENTRY_COUNT) begin
				data[(byte_idx * 8) +: 8] = pmpcfg_entries[base_entry + byte_idx];
			end
		end
		return data;
	endfunction

	function automatic logic [XLEN-1:0] read_pmpaddr_csr(input logic [11:0] addr);
		return pmpaddr_entries[pmpaddr_csr_entry(addr)] & pmpaddr_writable_mask();
	endfunction

	task automatic write_pmpcfg_csr(input logic [11:0] addr, input logic [XLEN-1:0] write_data);
		int unsigned base_entry;
		pmpcfg_entry_t next_cfg;
		base_entry = pmpcfg_csr_base_entry(addr);
		for (int byte_idx = 0; byte_idx < PMP_CFG_ENTRIES_PER_RV64_CSR; byte_idx++) begin
			if ((base_entry + byte_idx) < PMP_ENTRY_COUNT) begin
				next_cfg = legalize_pmpcfg_byte(write_data[(byte_idx * 8) +: 8]);
				if (!pmpcfg_entries[base_entry + byte_idx][PMPCFG_L_BIT] && pmpcfg_byte_is_legal(next_cfg)) begin
					pmpcfg_entries[base_entry + byte_idx] <= next_cfg;
				end
			end
		end
	endtask

	task automatic write_pmpaddr_csr(input logic [11:0] addr, input logic [XLEN-1:0] write_data);
		int unsigned entry_idx;
		entry_idx = pmpaddr_csr_entry(addr);
		if (!pmpaddr_is_locked(entry_idx)) begin
			pmpaddr_entries[entry_idx] <= write_data & pmpaddr_writable_mask();
		end
	endtask

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
			CSR_MVENDORID,
			CSR_MARCHID,
			CSR_MIMPID,
			CSR_MHARTID,
			CSR_MCONFIGPTR,
			CSR_TIME: csr_is_writable = 1'b0;
			default:  csr_is_writable = 1'b1;
		endcase
	endfunction

	function automatic bit csr_is_read_only_by_encoding(input logic [11:0] addr);
		return addr[11:10] == 2'b11;
	endfunction

	function automatic bit csr_is_supported(input logic [11:0] addr);
		bit supported;
		if (csr_is_pmpcfg(addr) || csr_is_pmpaddr(addr) ||
		    csr_is_mhpmcounter(addr) || csr_is_hpmcounter(addr) || csr_is_mhpmevent(addr)) begin
			supported = 1'b1;
		end else begin
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
				CSR_MTINST,
				CSR_MTVAL2,
				CSR_MCOUNTEREN,
				CSR_MCOUNTINHIBIT,
				CSR_MENVCFG,
				CSR_MSTATEEN0,
				CSR_MSTATEEN1,
				CSR_MSTATEEN2,
				CSR_MSTATEEN3,
				CSR_MVENDORID,
				CSR_MARCHID,
				CSR_MIMPID,
				CSR_MHARTID,
				CSR_MCONFIGPTR,
				CSR_SSTATUS,
				CSR_SIE,
				CSR_STVEC,
				CSR_SCOUNTEREN,
				CSR_SENVCFG,
				CSR_SSTATEEN0,
				CSR_SSTATEEN1,
				CSR_SSTATEEN2,
				CSR_SSTATEEN3,
				CSR_SSCRATCH,
				CSR_SEPC,
				CSR_SCAUSE,
				CSR_STVAL,
				CSR_SIP,
				CSR_SATP,
				CSR_HSTATUS,
				CSR_HEDELEG,
				CSR_HIDELEG,
				CSR_HIE,
				CSR_HTIMEDELTA,
				CSR_HCOUNTEREN,
				CSR_HGEIE,
				CSR_HENVCFG,
				CSR_HSTATEEN0,
				CSR_HSTATEEN1,
				CSR_HSTATEEN2,
				CSR_HSTATEEN3,
				CSR_HTVAL,
				CSR_HIP,
				CSR_HVIP,
				CSR_HTINST,
				CSR_HGATP,
				CSR_HGEIP,
				CSR_VSSTATUS,
				CSR_VSIE,
				CSR_VSTVEC,
				CSR_VSSCRATCH,
				CSR_VSEPC,
				CSR_VSCAUSE,
				CSR_VSTVAL,
				CSR_VSIP,
				CSR_VSATP,
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
		end
		return supported;
	endfunction

	function automatic bit csr_privilege_allows_access(
		input logic [11:0] requested_addr,
		input logic [11:0] canonicalized_addr
	);
		logic [1:0] minimum_privilege;
		minimum_privilege = requested_addr[9:8];
		if (!csr_is_supported(canonicalized_addr)) begin
			return 1'b0;
		end
		if (minimum_privilege == 2'b10) begin
			return 1'b0;
		end
		return encode_priv_mode(priv_mode_q) >= minimum_privilege;
	endfunction

	function automatic bit csr_access_blocked_by_stateen(
		input logic [11:0] requested_addr,
		input logic [11:0] canonicalized_addr
	);
		int unsigned current_counter_index;
		if (csr_is_counter(requested_addr) && (priv_mode_q != PRIV_MODE_M)) begin
			current_counter_index = counter_index(requested_addr);
			if (!mcounteren_reg[current_counter_index]) begin
				return 1'b1;
			end
			if ((priv_mode_q == PRIV_MODE_U) && !scounteren_reg[current_counter_index]) begin
				return 1'b1;
			end
		end
		if ((canonicalized_addr == CSR_SENVCFG) && (priv_mode_q == PRIV_MODE_S)) begin
			return !mstateen_reg[0][STATEEN0_ENVCFG_BIT];
		end
		return 1'b0;
	endfunction

	function automatic logic [XLEN-1:0] legalize_mstatus(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = data;
		if (legal_data[MSTATUS_MPP_HI:MSTATUS_MPP_LO] == 2'b10) begin
			legal_data[MSTATUS_MPP_HI:MSTATUS_MPP_LO] = encode_priv_mode(PRIV_MODE_U);
		end
		legal_data[MSTATUS_XS_HI:MSTATUS_XS_LO] = 2'b00;
		legal_data[MSTATUS_UXL_HI:MSTATUS_UXL_LO] = STATUS_XLEN_64;
		legal_data[MSTATUS_SXL_HI:MSTATUS_SXL_LO] = STATUS_XLEN_64;
		legal_data[MSTATUS_SD_BIT] = (legal_data[MSTATUS_FS_HI:MSTATUS_FS_LO] == 2'b11) ||
		                             (legal_data[MSTATUS_XS_HI:MSTATUS_XS_LO] == 2'b11) ||
		                             (legal_data[MSTATUS_VS_HI:MSTATUS_VS_LO] == 2'b11);
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_sstatus(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		legal_data[SSTATUS_SIE_BIT]                  = data[SSTATUS_SIE_BIT];
		legal_data[SSTATUS_SPIE_BIT]                 = data[SSTATUS_SPIE_BIT];
		legal_data[SSTATUS_SPP_BIT]                  = data[SSTATUS_SPP_BIT];
		legal_data[SSTATUS_VS_HI:SSTATUS_VS_LO]      = data[SSTATUS_VS_HI:SSTATUS_VS_LO];
		legal_data[SSTATUS_FS_HI:SSTATUS_FS_LO]      = data[SSTATUS_FS_HI:SSTATUS_FS_LO];
		legal_data[SSTATUS_SUM_BIT]                  = data[SSTATUS_SUM_BIT];
		legal_data[SSTATUS_MXR_BIT]                  = data[SSTATUS_MXR_BIT];
		legal_data[SSTATUS_UXL_HI:SSTATUS_UXL_LO]    = STATUS_XLEN_64;
		legal_data[SSTATUS_SD_BIT]                   = (legal_data[SSTATUS_FS_HI:SSTATUS_FS_LO] == 2'b11) ||
		                                               (legal_data[SSTATUS_XS_HI:SSTATUS_XS_LO] == 2'b11) ||
		                                               (legal_data[SSTATUS_VS_HI:SSTATUS_VS_LO] == 2'b11);
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] reset_mstatus_value();
		return legalize_mstatus('0);
	endfunction

	function automatic logic [XLEN-1:0] reset_sstatus_value();
		return legalize_sstatus('0);
	endfunction

	function automatic logic [XLEN-1:0] set_mstatus_fs_dirty(input logic [XLEN-1:0] current);
		logic [XLEN-1:0] next_data;
		next_data = current;
		next_data[MSTATUS_FS_HI:MSTATUS_FS_LO] = 2'b11;
		return legalize_mstatus(next_data);
	endfunction

	function automatic logic [XLEN-1:0] set_sstatus_fs_dirty(input logic [XLEN-1:0] current);
		logic [XLEN-1:0] next_data;
		next_data = current;
		next_data[SSTATUS_FS_HI:SSTATUS_FS_LO] = 2'b11;
		return legalize_sstatus(next_data);
	endfunction

	function automatic logic [XLEN-1:0] legalize_medeleg(input logic [XLEN-1:0] data);
		return (data & MEDELEG_WRITABLE_MASK) | MEDELEG_READ_ONLY_ONE_MASK;
	endfunction

	function automatic logic [XLEN-1:0] legalize_mideleg(input logic [XLEN-1:0] data);
		return data & MIDELEG_WRITABLE_MASK;
	endfunction

	function automatic logic [XLEN-1:0] legalize_interrupt_csr(input logic [XLEN-1:0] data);
		return data & INTERRUPT_MASK;
	endfunction

	function automatic logic [XLEN-1:0] compose_mip();
		logic [XLEN-1:0] pending;
		pending = legalize_interrupt_csr(mip_reg);
		pending[INTERRUPT_M_SOFTWARE_BIT] |= m_software_interrupt;
		pending[INTERRUPT_M_TIMER_BIT]    |= m_timer_interrupt;
		pending[INTERRUPT_M_EXTERNAL_BIT] |= m_external_interrupt;
		pending[INTERRUPT_S_SOFTWARE_BIT] |= s_software_interrupt;
		pending[INTERRUPT_S_TIMER_BIT]    |= s_timer_interrupt;
		pending[INTERRUPT_S_EXTERNAL_BIT] |= s_external_interrupt;
		return pending;
	endfunction

	function automatic logic [XLEN-1:0] legalize_mcountinhibit(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		legal_data[MCOUNTINHIBIT_CY_BIT] = data[MCOUNTINHIBIT_CY_BIT];
		legal_data[MCOUNTINHIBIT_IR_BIT] = data[MCOUNTINHIBIT_IR_BIT];
		legal_data[31:3] = data[31:3];
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_counteren(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		legal_data[COUNTEREN_CY_BIT] = data[COUNTEREN_CY_BIT];
		legal_data[COUNTEREN_TM_BIT] = data[COUNTEREN_TM_BIT];
		legal_data[COUNTEREN_IR_BIT] = data[COUNTEREN_IR_BIT];
		legal_data[31:3] = data[31:3];
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_senvcfg(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		legal_data[ENVCFG_FIOM_BIT] = data[ENVCFG_FIOM_BIT];
		legal_data[ENVCFG_CBIE_HI:ENVCFG_CBIE_LO] =
			menvcfg_reg[ENVCFG_CBIE_HI:ENVCFG_CBIE_LO] & data[ENVCFG_CBIE_HI:ENVCFG_CBIE_LO];
		legal_data[ENVCFG_CBCFE_BIT] = menvcfg_reg[ENVCFG_CBCFE_BIT] & data[ENVCFG_CBCFE_BIT];
		legal_data[ENVCFG_CBZE_BIT] = menvcfg_reg[ENVCFG_CBZE_BIT] & data[ENVCFG_CBZE_BIT];
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_menvcfg(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		legal_data[ENVCFG_FIOM_BIT] = data[ENVCFG_FIOM_BIT];
		legal_data[ENVCFG_CBIE_HI:ENVCFG_CBIE_LO] = data[ENVCFG_CBIE_HI:ENVCFG_CBIE_LO];
		legal_data[ENVCFG_CBCFE_BIT] = data[ENVCFG_CBCFE_BIT];
		legal_data[ENVCFG_CBZE_BIT] = data[ENVCFG_CBZE_BIT];
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_hstatus(input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		legal_data[6]  = data[6];   // VTSR
		legal_data[7]  = data[7];   // VTW
		legal_data[8]  = data[8];   // VTVM
		legal_data[12] = data[12];  // SPVP
		legal_data[14] = data[14];  // SPV
		legal_data[16:15] = data[16:15]; // GVA and VSBE are WARL here.
		legal_data[33:32] = STATUS_XLEN_64; // VSXL
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_mstateen(input int unsigned index, input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		if (index == 0) begin
			legal_data[STATEEN0_ENVCFG_BIT] = data[STATEEN0_ENVCFG_BIT];
		end
		legal_data[STATEEN_SE0_BIT] = data[STATEEN_SE0_BIT];
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_sstateen(input int unsigned index, input logic [XLEN-1:0] data);
		logic [XLEN-1:0] legal_data;
		legal_data = '0;
		if (index == 0) begin
			legal_data[STATEEN0_ENVCFG_BIT] = data[STATEEN0_ENVCFG_BIT] & mstateen_reg[0][STATEEN0_ENVCFG_BIT];
		end
		legal_data[STATEEN_SE0_BIT] = data[STATEEN_SE0_BIT] & mstateen_reg[index][STATEEN_SE0_BIT];
		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_satp(
		input logic [XLEN-1:0] data,
		input logic [XLEN-1:0] current
	);
		logic [XLEN-1:0] legal_data;
		logic [3:0] mode;
		mode = data[SATP_MODE_HI:SATP_MODE_LO];
		legal_data = current;

		unique case (mode)
			SATP_MODE_BARE: begin
				legal_data = '0;
			end
			SATP_MODE_SV39: begin
				legal_data = '0;
				legal_data[SATP_MODE_HI:SATP_MODE_LO] = SATP_MODE_SV39;
				legal_data[SATP_ASID_HI:SATP_ASID_LO] = data[SATP_ASID_HI:SATP_ASID_LO];
				legal_data[SATP_PPN_HI:SATP_PPN_LO]   = data[SATP_PPN_HI:SATP_PPN_LO];
			end
			SATP_MODE_SV48,
			SATP_MODE_SV57,
			SATP_MODE_SV64: begin
				legal_data = current;
			end
			default: begin
				legal_data = current;
			end
		endcase

		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] legalize_hgatp(
		input logic [XLEN-1:0] data,
		input logic [XLEN-1:0] current
	);
		logic [XLEN-1:0] legal_data;
		logic [3:0] mode;
		mode = data[SATP_MODE_HI:SATP_MODE_LO];
		legal_data = current;

		unique case (mode)
			SATP_MODE_BARE: legal_data = '0;
			SATP_MODE_SV39: legal_data = data;
			SATP_MODE_SV48,
			SATP_MODE_SV57,
			SATP_MODE_SV64: legal_data = current;
			default: legal_data = current;
		endcase

		return legal_data;
	endfunction

	function automatic logic [XLEN-1:0] apply_sstatus_to_mstatus(
		input logic [XLEN-1:0] mstatus_cur,
		input logic [XLEN-1:0] sstatus_data
	);
		logic [XLEN-1:0] next_data;
		next_data = mstatus_cur;
		next_data[SSTATUS_SIE_BIT]                  = sstatus_data[SSTATUS_SIE_BIT];
		next_data[SSTATUS_SPIE_BIT]                 = sstatus_data[SSTATUS_SPIE_BIT];
		next_data[SSTATUS_SPP_BIT]                  = sstatus_data[SSTATUS_SPP_BIT];
		next_data[SSTATUS_VS_HI:SSTATUS_VS_LO]      = sstatus_data[SSTATUS_VS_HI:SSTATUS_VS_LO];
		next_data[SSTATUS_FS_HI:SSTATUS_FS_LO]      = sstatus_data[SSTATUS_FS_HI:SSTATUS_FS_LO];
		next_data[SSTATUS_SUM_BIT]                  = sstatus_data[SSTATUS_SUM_BIT];
		next_data[SSTATUS_MXR_BIT]                  = sstatus_data[SSTATUS_MXR_BIT];
		next_data[MSTATUS_UXL_HI:MSTATUS_UXL_LO]    = STATUS_XLEN_64;
		return legalize_mstatus(next_data);
	endfunction

	function automatic logic [XLEN-1:0] mirror_mstatus_to_sstatus(
		input logic [XLEN-1:0] sstatus_cur,
		input logic [XLEN-1:0] mstatus_data
	);
		logic [XLEN-1:0] next_data;
		next_data = legalize_sstatus(sstatus_cur);
		next_data[SSTATUS_SIE_BIT]                  = mstatus_data[SSTATUS_SIE_BIT];
		next_data[SSTATUS_SPIE_BIT]                 = mstatus_data[SSTATUS_SPIE_BIT];
		next_data[SSTATUS_SPP_BIT]                  = mstatus_data[SSTATUS_SPP_BIT];
		next_data[SSTATUS_VS_HI:SSTATUS_VS_LO]      = mstatus_data[MSTATUS_VS_HI:MSTATUS_VS_LO];
		next_data[SSTATUS_FS_HI:SSTATUS_FS_LO]      = mstatus_data[SSTATUS_FS_HI:SSTATUS_FS_LO];
		next_data[SSTATUS_SUM_BIT]                  = mstatus_data[SSTATUS_SUM_BIT];
		next_data[SSTATUS_MXR_BIT]                  = mstatus_data[MSTATUS_MXR_BIT];
		next_data[SSTATUS_UXL_HI:SSTATUS_UXL_LO]    = STATUS_XLEN_64;
		next_data[SSTATUS_SD_BIT]                   = mstatus_data[MSTATUS_SD_BIT];
		return legalize_sstatus(next_data);
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
		if (csr_is_pmpcfg(addr)) begin
			data_out = read_pmpcfg_csr(addr);
		end else if (csr_is_pmpaddr(addr)) begin
			data_out = read_pmpaddr_csr(addr);
		end else if (csr_is_mhpmcounter(addr) || csr_is_hpmcounter(addr)) begin
			data_out = mhpmcounter_reg[hpmcounter_index(addr)];
		end else if (csr_is_mhpmevent(addr)) begin
			data_out = '0;
		end else begin
			unique case (addr)
				CSR_FFLAGS:    data_out = {{(XLEN-5){1'b0}}, fflags_reg};
				CSR_FRM:       data_out = {{(XLEN-3){1'b0}}, frm_reg};
				CSR_FCSR:      data_out = compose_fcsr();
				CSR_MSTATUS:   data_out = mstatus_reg;
				CSR_MISA:      data_out = MISA_VALUE;
				CSR_MEDELEG:   data_out = medeleg_reg;
				CSR_MIDELEG:   data_out = mideleg_reg;
				CSR_MIE:       data_out = mie_reg;
				CSR_MTVEC:     data_out = mtvec_reg;
				CSR_MSCRATCH:  data_out = mscratch_reg;
				CSR_MEPC:      data_out = mepc_reg;
				CSR_MCAUSE:    data_out = mcause_reg;
				CSR_MTVAL:     data_out = mtval_reg;
				CSR_MIP:       data_out = mip_value;
				CSR_MTINST:    data_out = mtinst_reg;
				CSR_MTVAL2:    data_out = mtval2_reg;
				CSR_MCOUNTEREN: data_out = mcounteren_reg;
				CSR_MCOUNTINHIBIT: data_out = mcountinhibit_reg;
				CSR_MENVCFG:   data_out = menvcfg_reg;
				CSR_MSTATEEN0: data_out = mstateen_reg[0];
				CSR_MSTATEEN1: data_out = mstateen_reg[1];
				CSR_MSTATEEN2: data_out = mstateen_reg[2];
				CSR_MSTATEEN3: data_out = mstateen_reg[3];
				CSR_MVENDORID: data_out = VENDOR_ID;
				CSR_MARCHID:   data_out = ARCH_ID;
				CSR_MIMPID:    data_out = IMPL_ID;
				CSR_MHARTID:   data_out = HART_ID;
				CSR_MCONFIGPTR: data_out = '0;
				CSR_SSTATUS:   data_out = sstatus_reg;
				CSR_SIE:       data_out = sie_value;
				CSR_STVEC:     data_out = stvec_reg;
				CSR_SCOUNTEREN: data_out = scounteren_reg;
				CSR_SENVCFG:   data_out = legalize_senvcfg(senvcfg_reg);
				CSR_SSTATEEN0: data_out = sstateen_reg[0];
				CSR_SSTATEEN1: data_out = sstateen_reg[1];
				CSR_SSTATEEN2: data_out = sstateen_reg[2];
				CSR_SSTATEEN3: data_out = sstateen_reg[3];
				CSR_SSCRATCH:  data_out = sscratch_reg;
				CSR_SEPC:      data_out = sepc_reg;
				CSR_SCAUSE:    data_out = scause_reg;
				CSR_STVAL:     data_out = stval_reg;
				CSR_SIP:       data_out = sip_value;
				CSR_SATP:      data_out = satp_reg;
				CSR_HSTATUS:   data_out = hstatus_reg;
				CSR_HEDELEG:   data_out = hedeleg_reg;
				CSR_HIDELEG:   data_out = hideleg_reg;
				CSR_HIE:       data_out = hie_reg;
				CSR_HTIMEDELTA: data_out = htimedelta_reg;
				CSR_HCOUNTEREN: data_out = hcounteren_reg;
				CSR_HGEIE:     data_out = hgeie_reg;
				CSR_HENVCFG:   data_out = henvcfg_reg;
				CSR_HSTATEEN0: data_out = hstateen_reg[0];
				CSR_HSTATEEN1: data_out = hstateen_reg[1];
				CSR_HSTATEEN2: data_out = hstateen_reg[2];
				CSR_HSTATEEN3: data_out = hstateen_reg[3];
				CSR_HTVAL:     data_out = htval_reg;
				CSR_HIP:       data_out = hip_reg;
				CSR_HVIP:      data_out = hvip_reg;
				CSR_HTINST:    data_out = htinst_reg;
				CSR_HGATP:     data_out = hgatp_reg;
				CSR_HGEIP:     data_out = '0;
				CSR_VSSTATUS:  data_out = vsstatus_reg;
				CSR_VSIE:      data_out = vsie_reg;
				CSR_VSTVEC:    data_out = vstvec_reg;
				CSR_VSSCRATCH: data_out = vsscratch_reg;
				CSR_VSEPC:     data_out = vsepc_reg;
				CSR_VSCAUSE:   data_out = vscause_reg;
				CSR_VSTVAL:    data_out = vstval_reg;
				CSR_VSIP:      data_out = vsip_reg;
				CSR_VSATP:     data_out = vsatp_reg;
				CSR_MCYCLE:    data_out = mcycle_reg;
				CSR_MINSTRET:  data_out = minstret_reg;
				CSR_CYCLE:     data_out = mcycle_reg;
				CSR_INSTRET:   data_out = minstret_reg;
				CSR_TIME:       data_out = time_reg;
				default:        data_out = '0;
			endcase
		end
		return data_out;
	endfunction

endmodule
