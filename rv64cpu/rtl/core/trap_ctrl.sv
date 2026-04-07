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

// File path: rtl/core/trap_ctrl.sv
// Description: Privileged-level Trap Controller (Exception and Interrupt Handling)
//              Detects exceptions/interrupts, triggers context saving, and generates global pipeline flush and PC jump signals

module trap_ctrl 
    import rv64_pkg::*;
    import csr_pkg::*;
(
    // Submission signals from the WB stage (guarantee of precise exceptions)
    input  logic [XLEN-1:0]        wb_pc,
    input  logic                  trap_illegal_instr,
    input  logic                  trap_is_ecall,
    input  logic                  trap_is_ebreak,
    input  logic                  trap_is_mret,
    input  logic                  trap_is_sret,
    input  logic                  trap_load_fault,
    input  logic                  trap_store_fault,
    input  logic                  trap_instr_page_fault,
    input  logic                  trap_load_page_fault,
    input  logic                  trap_store_page_fault,
    input  logic [XLEN-1:0]       trap_bad_addr,
    input  logic [INST_WIDTH-1:0] trap_bad_instr,

    // Interrupt pin from external source (asynchronous signal)
    input  logic                  ext_timer_interrupt,       // M-Mode Machine Timer Interrupt (MTIP)
    input  logic                  ext_software_interrupt,    // M-Mode Software Interrupt (MSIP)
    input  logic                  ext_external_interrupt,    // M-Mode External Interrupt (MEIP)

    // Status read from CSR Unit
    input  logic                  csr_mstatus_mie,    // Global interrupt enable (MIE)
    input  logic [XLEN-1:0]       csr_mtvec,          // Machine trap vector
    input  logic [XLEN-1:0]       csr_stvec,          // Supervisor trap vector
    input  logic [XLEN-1:0]       csr_mepc,           // Machine exception PC (for MRET)
    input  logic [XLEN-1:0]       csr_sepc,           // Supervisor exception PC (for SRET)
    input  logic [XLEN-1:0]       csr_medeleg,        // Exception delegation mask
    input  logic [XLEN-1:0]       csr_mideleg,        // Interrupt delegation mask
    input  logic [1:0]            csr_priv_mode,      // Current privilege level

    // Status update signal sent to the CSR Unit
    output logic                  trap_trigger,       // Trigger exception/interrupt context save
    output logic [XLEN-1:0]       trap_mepc,          // Saved PC (MEPC/SEPC selected by trap_to_s_mode)
    output logic [XLEN-1:0]       trap_mcause,        // Cause code (bit63 indicates interrupt)
    output logic [XLEN-1:0]       trap_mtval,         // Additional information saved to mtval/stval
    output logic                  trap_to_s_mode,     // 1: delegated to Supervisor mode
    output logic                  mret_en,            // Trigger MRET context restore
    output logic                  sret_en,            // Trigger SRET context restore

    // Control flow interference sent to the Hazard Unit and IF stage
    output logic                  trap_flush_req,     // Global pipeline flush request
    output logic                  trap_pc_sel,        // Forcibly take over PC MUX (0: normal, 1: Trap/Mret/Sret)
    output logic [XLEN-1:0]       trap_next_pc        // Target address for forced jump
);

    typedef enum logic [1:0] {
        PRIV_MODE_U = 2'b00,
        PRIV_MODE_S = 2'b01,
        PRIV_MODE_M = 2'b11
    } priv_mode_t;

    localparam logic [XLEN-1:0] INTERRUPT_BIT = {1'b1, {(XLEN-1){1'b0}}};

    logic                 has_sync_exception;
    logic [XLEN-1:0]      sync_cause;
    logic [XLEN-1:0]      sync_mtval;
    logic [XLEN-1:0]      illegal_instr_value;

    assign illegal_instr_value = {{(XLEN-INST_WIDTH){1'b0}}, trap_bad_instr};

    always_comb begin
        has_sync_exception = 1'b0;
        sync_cause         = '0;
        sync_mtval         = '0;

        if (trap_illegal_instr) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_INST_ILLEGAL;
            sync_mtval         = illegal_instr_value;
        end else if (trap_is_ebreak) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_BREAKPOINT;
            sync_mtval         = wb_pc;
        end else if (trap_is_ecall) begin
            has_sync_exception = 1'b1;
            case (priv_mode_t'(csr_priv_mode))
                PRIV_MODE_U: sync_cause = EXC_ECALL_FROM_U;
                PRIV_MODE_S: sync_cause = EXC_ECALL_FROM_S;
                default:     sync_cause = EXC_ECALL_FROM_M;
            endcase
            sync_mtval = '0;
        end else if (trap_load_fault) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_LOAD_FAULT;
            sync_mtval         = trap_bad_addr;
        end else if (trap_store_fault) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_STORE_FAULT;
            sync_mtval         = trap_bad_addr;
        end else if (trap_instr_page_fault) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_INSTR_PAGE_FAULT;
            sync_mtval         = trap_bad_addr;
        end else if (trap_load_page_fault) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_LOAD_PAGE_FAULT;
            sync_mtval         = trap_bad_addr;
        end else if (trap_store_page_fault) begin
            has_sync_exception = 1'b1;
            sync_cause         = EXC_STORE_PAGE_FAULT;
            sync_mtval         = trap_bad_addr;
        end
    end

    // Interrupt Detection - Asynchronous, Protected by MIE
    logic               has_interrupt;
    logic [XLEN-1:0]    interrupt_cause;

    always_comb begin
        has_interrupt = 1'b0;
        interrupt_cause = '0;

        if (csr_mstatus_mie) begin
            if (ext_external_interrupt) begin
                has_interrupt = 1'b1;
                interrupt_cause = INTERRUPT_BIT | INTERRUPT_M_EXTERNAL;
            end else if (ext_software_interrupt) begin
                has_interrupt = 1'b1;
                interrupt_cause = INTERRUPT_BIT | INTERRUPT_M_SOFTWARE;
            end else if (ext_timer_interrupt) begin
                has_interrupt = 1'b1;
                interrupt_cause = INTERRUPT_BIT | INTERRUPT_M_TIMER;
            end
        end
    end 

    logic [XLEN-1:0] trap_cause_value;
    logic [XLEN-1:0] trap_mtval_value;
    logic            trap_is_interrupt;

    assign trap_is_interrupt = has_interrupt;
    assign trap_cause_value  = has_interrupt ? interrupt_cause : sync_cause;
    assign trap_mtval_value  = has_interrupt ? '0 : sync_mtval;

    assign trap_trigger = has_interrupt | has_sync_exception;

    logic [5:0]      trap_cause_index;
    logic            delegate_to_s;
    logic [XLEN-1:0] deleg_mask;

    assign trap_cause_index = trap_cause_value[5:0];
    assign deleg_mask       = trap_is_interrupt ? csr_mideleg : csr_medeleg;
    assign delegate_to_s    = trap_trigger && (priv_mode_t'(csr_priv_mode) != PRIV_MODE_M) && deleg_mask[trap_cause_index];
    assign trap_to_s_mode   = delegate_to_s;

    assign trap_mcause = trap_trigger ? trap_cause_value : '0;
    assign trap_mtval  = trap_trigger ? trap_mtval_value : '0;
    assign trap_mepc   = trap_trigger ? wb_pc : '0;

    assign mret_en = trap_is_mret & ~trap_trigger;
    assign sret_en = trap_is_sret & ~trap_trigger;

    assign trap_flush_req = trap_trigger | mret_en | sret_en;
    assign trap_pc_sel    = trap_flush_req;

    logic [XLEN-1:0] trap_vector_base;
    logic [1:0]      trap_vector_mode;
    logic [XLEN-1:0] trap_vector_aligned;

    assign trap_vector_base    = delegate_to_s ? csr_stvec : csr_mtvec;
    assign trap_vector_mode    = trap_vector_base[1:0];
    assign trap_vector_aligned = {trap_vector_base[XLEN-1:2], 2'b00};

    always_comb begin
        if (trap_trigger) begin
            if ((trap_vector_mode == 2'b01) && trap_is_interrupt) begin
                trap_next_pc = trap_vector_aligned + (({{(XLEN-6){1'b0}}, trap_cause_index}) << 2);
            end else begin
                trap_next_pc = trap_vector_aligned;
            end
        end else if (mret_en) begin
            trap_next_pc = csr_mepc;
        end else if (sret_en) begin
            trap_next_pc = csr_sepc;
        end else begin
            trap_next_pc = '0;
        end
    end

endmodule
