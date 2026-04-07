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

// File path: rtl/core/include/csr_pkg.sv
// Description: Global enumerations, address definitions, and exception reason constants related to CSR and privilege-level architecture

package csr_pkg;
    import rv64_pkg::*;

    typedef enum logic [2:0] { 
        CSR_OP_NONE  = 3'b000,
        CSR_OP_WRITE = 3'b001,
        CSR_OP_SET   = 3'b010,
        CSR_OP_CLEAR = 3'b011
    } csr_op_t;

    // ---------------------------------
    // Floating-Point CSRs
    // ---------------------------------
    localparam logic [11:0]     CSR_FFLAGS   = 12'h001;    // Floating-Point accrued exception flags
    localparam logic [11:0]     CSR_FRM      = 12'h002;    // Floating-Point dynamic rounding mode
    localparam logic [11:0]     CSR_FCSR     = 12'h003;    // Combined fflags/frm register

    // ---------------------------------
    // Machine Mode CSRs (M-Mode)
    // ---------------------------------
    localparam logic [11:0]     CSR_MVENDORID = 12'hF11;    // Vendor ID
    localparam logic [11:0]     CSR_MARCHID   = 12'hF12;    // Architecture ID
    localparam logic [11:0]     CSR_MIMPID    = 12'hF13;    // Implementation ID
    localparam logic [11:0]     CSR_MHARTID   = 12'hF14;    // Hart ID

    localparam logic [11:0]     CSR_MSTATUS   = 12'h300;    // Machine Status Register
    localparam logic [11:0]     CSR_MISA      = 12'h301;    // Machine ISA Register
    localparam logic [11:0]     CSR_MEDELEG   = 12'h302;    // Exception Delegation
    localparam logic [11:0]     CSR_MIDELEG   = 12'h303;    // Interrupt Delegation
    localparam logic [11:0]     CSR_MIE       = 12'h304;    // Machine Interrupt Enable Register
    localparam logic [11:0]     CSR_MTVEC     = 12'h305;    // Machine Trap Vector Register
    localparam logic [11:0]     CSR_MCOUNTEREN= 12'h306;    // Machine Counter Enable Register

    localparam logic [11:0]     CSR_MSCRATCH  = 12'h340;    // Machine Scratch Register
    localparam logic [11:0]     CSR_MEPC      = 12'h341;    // Machine Exception Program Counter
    localparam logic [11:0]     CSR_MCAUSE    = 12'h342;    // Machine Cause Register
    localparam logic [11:0]     CSR_MTVAL     = 12'h343;    // Machine Trap Value Register
    localparam logic [11:0]     CSR_MIP       = 12'h344;    // Machine Interrupt Pending Register

    localparam logic [11:0]     CSR_MCYCLE    = 12'hB00;    // Machine Cycle Counter
    localparam logic [11:0]     CSR_MINSTRET  = 12'hB02;    // Machine Instruction Retired Counter

    // ---------------------------------
    // Supervisor Mode CSRs (S-Mode)
    // ---------------------------------
    localparam logic [11:0]     CSR_SSTATUS   = 12'h100;    // Supervisor Status Register
    localparam logic [11:0]     CSR_SIE       = 12'h104;    // Supervisor Interrupt Enable Register
    localparam logic [11:0]     CSR_STVEC     = 12'h105;    // Supervisor Trap Vector Register
    localparam logic [11:0]     CSR_SCOUNTEREN= 12'h106;    // Supervisor Counter Enable Register
    localparam logic [11:0]     CSR_SSCRATCH  = 12'h140;    // Supervisor Scratch Register
    localparam logic [11:0]     CSR_SEPC      = 12'h141;    // Supervisor Exception Program Counter
    localparam logic [11:0]     CSR_SCAUSE    = 12'h142;    // Supervisor Cause Register
    localparam logic [11:0]     CSR_STVAL     = 12'h143;    // Supervisor Trap Value Register
    localparam logic [11:0]     CSR_SIP       = 12'h144;    // Supervisor Interrupt Pending Register
    localparam logic [11:0]     CSR_SATP      = 12'h180;    // Supervisor Address Translation and Protection Register

    // ---------------------------------
    // User Mode CSRs (U-Mode / URW)
    // ---------------------------------
    localparam logic [11:0]     CSR_CYCLE     = 12'hC00;    // Cycle Counter
    localparam logic [11:0]     CSR_TIME      = 12'hC01;    // Timer Counter
    localparam logic [11:0]     CSR_INSTRET   = 12'hC02;    // Instruction Retired Counter

    // ---------------------------------
    // Exception Cause Codes (mcause.Interrupt == 0)
    // ---------------------------------
    localparam logic [XLEN-1:0] EXC_INST_ADDR_MISALIGNED        = 64'd0;    // Instruction Address Misaligned
    localparam logic [XLEN-1:0] EXC_INST_FAULT                  = 64'd1;    // Instruction Access Fault
    localparam logic [XLEN-1:0] EXC_INST_ILLEGAL                = 64'd2;    // Illegal Instruction
    localparam logic [XLEN-1:0] EXC_BREAKPOINT                  = 64'd3;    // Breakpoint
    localparam logic [XLEN-1:0] EXC_LOAD_ADDR_MISALIGNED        = 64'd4;    // Load Address Misaligned
    localparam logic [XLEN-1:0] EXC_LOAD_FAULT                  = 64'd5;    // Load Access Fault
    localparam logic [XLEN-1:0] EXC_STORE_ADDR_MISALIGNED       = 64'd6;    // Store Address Misaligned
    localparam logic [XLEN-1:0] EXC_STORE_FAULT                 = 64'd7;    // Store Access Fault
    localparam logic [XLEN-1:0] EXC_ECALL_FROM_U                = 64'd8;    // Environment Call from U-Mode
    localparam logic [XLEN-1:0] EXC_ECALL_FROM_S                = 64'd9;    // Environment Call from S-Mode
    localparam logic [XLEN-1:0] EXC_ECALL_FROM_M                = 64'd11;   // Environment Call from M-Mode
    localparam logic [XLEN-1:0] EXC_INSTR_PAGE_FAULT            = 64'd12;   // Instruction Page Fault
    localparam logic [XLEN-1:0] EXC_LOAD_PAGE_FAULT             = 64'd13;   // Load Page Fault
    localparam logic [XLEN-1:0] EXC_STORE_PAGE_FAULT            = 64'd15;   // Store Page Fault

    // ---------------------------------
    // Interrupt Cause Codes (mcause.Interrupt == 1)
    // ---------------------------------
    // User Mode Interrupts
    localparam logic [XLEN-1:0] INTERRUPT_U_SOFTWARE            = 64'd0;    // User Mode Software Interrupt
    localparam logic [XLEN-1:0] INTERRUPT_U_TIMER               = 64'd4;    // User Mode Timer Interrupt
    localparam logic [XLEN-1:0] INTERRUPT_U_EXTERNAL            = 64'd8;    // User Mode External Interrupt
    // Supervisor Mode Interrupts
    localparam logic [XLEN-1:0] INTERRUPT_S_SOFTWARE            = 64'd1;    // Supervisor Mode Software Interrupt
    localparam logic [XLEN-1:0] INTERRUPT_S_TIMER               = 64'd5;    // Supervisor Mode Timer Interrupt
    localparam logic [XLEN-1:0] INTERRUPT_S_EXTERNAL            = 64'd9;    // Supervisor Mode External Interrupt
    // Machine Mode Interrupts
    localparam logic [XLEN-1:0] INTERRUPT_M_SOFTWARE            = 64'd3;    // Machine Mode Software Interrupt
    localparam logic [XLEN-1:0] INTERRUPT_M_TIMER               = 64'd7;    // Machine Mode Timer Interrupt
    localparam logic [XLEN-1:0] INTERRUPT_M_EXTERNAL            = 64'd11;   // Machine Mode External Interrupt

endpackage
