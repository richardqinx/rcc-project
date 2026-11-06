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
    localparam logic [11:0]     CSR_MCONFIGPTR= 12'hF15;    // Machine Configuration Pointer

    localparam logic [11:0]     CSR_MSTATUS   = 12'h300;    // Machine Status Register
    localparam logic [11:0]     CSR_MISA      = 12'h301;    // Machine ISA Register
    localparam logic [11:0]     CSR_MEDELEG   = 12'h302;    // Exception Delegation
    localparam logic [11:0]     CSR_MIDELEG   = 12'h303;    // Interrupt Delegation
    localparam logic [11:0]     CSR_MIE       = 12'h304;    // Machine Interrupt Enable Register
    localparam logic [11:0]     CSR_MTVEC     = 12'h305;    // Machine Trap Vector Register
    localparam logic [11:0]     CSR_MCOUNTEREN= 12'h306;    // Machine Counter Enable Register

    localparam logic [11:0]     CSR_MENVCFG   = 12'h30A;    // Machine Environment Configuration
    localparam logic [11:0]     CSR_MSTATEEN0 = 12'h30C;    // Machine State Enable 0
    localparam logic [11:0]     CSR_MSTATEEN1 = 12'h30D;    // Machine State Enable 1
    localparam logic [11:0]     CSR_MSTATEEN2 = 12'h30E;    // Machine State Enable 2
    localparam logic [11:0]     CSR_MSTATEEN3 = 12'h30F;    // Machine State Enable 3

    localparam logic [11:0]     CSR_PMPCFG0   = 12'h3A0;    // PMP Configuration Register 0
    localparam logic [11:0]     CSR_PMPCFG15  = 12'h3AF;    // PMP Configuration Register 15
    localparam logic [11:0]     CSR_PMPADDR0  = 12'h3B0;    // PMP Address Register 0
    localparam logic [11:0]     CSR_PMPADDR63 = 12'h3EF;    // PMP Address Register 63

    localparam logic [11:0]     CSR_MSCRATCH  = 12'h340;    // Machine Scratch Register
    localparam logic [11:0]     CSR_MEPC      = 12'h341;    // Machine Exception Program Counter
    localparam logic [11:0]     CSR_MCAUSE    = 12'h342;    // Machine Cause Register
    localparam logic [11:0]     CSR_MTVAL     = 12'h343;    // Machine Trap Value Register
    localparam logic [11:0]     CSR_MIP       = 12'h344;    // Machine Interrupt Pending Register
    localparam logic [11:0]     CSR_MTINST    = 12'h34A;    // Machine Trap Instruction
    localparam logic [11:0]     CSR_MTVAL2    = 12'h34B;    // Machine Second Trap Value

    localparam logic [11:0]     CSR_MCOUNTINHIBIT = 12'h320; // Machine Counter-Inhibit Register
    localparam logic [11:0]     CSR_MCYCLE    = 12'hB00;    // Machine Cycle Counter
    localparam logic [11:0]     CSR_MINSTRET  = 12'hB02;    // Machine Instruction Retired Counter
    localparam logic [11:0]     CSR_MHPMCOUNTER3  = 12'hB03; // Machine HPM Counter 3
    localparam logic [11:0]     CSR_MHPMCOUNTER31 = 12'hB1F; // Machine HPM Counter 31
    localparam logic [11:0]     CSR_MHPMEVENT3    = 12'h323; // Machine HPM Event Selector 3
    localparam logic [11:0]     CSR_MHPMEVENT31   = 12'h33F; // Machine HPM Event Selector 31

    // ---------------------------------
    // Supervisor Mode CSRs (S-Mode)
    // ---------------------------------
    localparam logic [11:0]     CSR_SSTATUS   = 12'h100;    // Supervisor Status Register
    localparam logic [11:0]     CSR_SIE       = 12'h104;    // Supervisor Interrupt Enable Register
    localparam logic [11:0]     CSR_STVEC     = 12'h105;    // Supervisor Trap Vector Register
    localparam logic [11:0]     CSR_SCOUNTEREN= 12'h106;    // Supervisor Counter Enable Register
    localparam logic [11:0]     CSR_SENVCFG   = 12'h10A;    // Supervisor Environment Configuration
    localparam logic [11:0]     CSR_SSTATEEN0 = 12'h10C;    // Supervisor State Enable 0
    localparam logic [11:0]     CSR_SSTATEEN1 = 12'h10D;    // Supervisor State Enable 1
    localparam logic [11:0]     CSR_SSTATEEN2 = 12'h10E;    // Supervisor State Enable 2
    localparam logic [11:0]     CSR_SSTATEEN3 = 12'h10F;    // Supervisor State Enable 3
    localparam logic [11:0]     CSR_SSCRATCH  = 12'h140;    // Supervisor Scratch Register
    localparam logic [11:0]     CSR_SEPC      = 12'h141;    // Supervisor Exception Program Counter
    localparam logic [11:0]     CSR_SCAUSE    = 12'h142;    // Supervisor Cause Register
    localparam logic [11:0]     CSR_STVAL     = 12'h143;    // Supervisor Trap Value Register
    localparam logic [11:0]     CSR_SIP       = 12'h144;    // Supervisor Interrupt Pending Register
    localparam logic [11:0]     CSR_SATP      = 12'h180;    // Supervisor Address Translation and Protection Register

    // ---------------------------------
    // Hypervisor CSRs (H-Extension)
    // ---------------------------------
    localparam logic [11:0]     CSR_HSTATUS     = 12'h600;  // Hypervisor Status
    localparam logic [11:0]     CSR_HEDELEG     = 12'h602;  // Hypervisor Exception Delegation
    localparam logic [11:0]     CSR_HIDELEG     = 12'h603;  // Hypervisor Interrupt Delegation
    localparam logic [11:0]     CSR_HIE         = 12'h604;  // Hypervisor Interrupt Enable
    localparam logic [11:0]     CSR_HTIMEDELTA  = 12'h605;  // Hypervisor Timer Delta
    localparam logic [11:0]     CSR_HCOUNTEREN  = 12'h606;  // Hypervisor Counter Enable
    localparam logic [11:0]     CSR_HGEIE       = 12'h607;  // Hypervisor Guest External Interrupt Enable
    localparam logic [11:0]     CSR_HENVCFG     = 12'h60A;  // Hypervisor Environment Configuration
    localparam logic [11:0]     CSR_HSTATEEN0   = 12'h60C;  // Hypervisor State Enable 0
    localparam logic [11:0]     CSR_HSTATEEN1   = 12'h60D;  // Hypervisor State Enable 1
    localparam logic [11:0]     CSR_HSTATEEN2   = 12'h60E;  // Hypervisor State Enable 2
    localparam logic [11:0]     CSR_HSTATEEN3   = 12'h60F;  // Hypervisor State Enable 3
    localparam logic [11:0]     CSR_HTVAL       = 12'h643;  // Hypervisor Trap Value
    localparam logic [11:0]     CSR_HIP         = 12'h644;  // Hypervisor Interrupt Pending
    localparam logic [11:0]     CSR_HVIP        = 12'h645;  // Hypervisor Virtual Interrupt Pending
    localparam logic [11:0]     CSR_HTINST      = 12'h64A;  // Hypervisor Trap Instruction
    localparam logic [11:0]     CSR_HGATP       = 12'h680;  // Hypervisor Guest Address Translation and Protection
    localparam logic [11:0]     CSR_HGEIP       = 12'hE12;  // Hypervisor Guest External Interrupt Pending

    localparam logic [11:0]     CSR_VSSTATUS    = 12'h200;  // Virtual Supervisor Status
    localparam logic [11:0]     CSR_VSIE        = 12'h204;  // Virtual Supervisor Interrupt Enable
    localparam logic [11:0]     CSR_VSTVEC      = 12'h205;  // Virtual Supervisor Trap Vector
    localparam logic [11:0]     CSR_VSSCRATCH   = 12'h240;  // Virtual Supervisor Scratch
    localparam logic [11:0]     CSR_VSEPC       = 12'h241;  // Virtual Supervisor Exception PC
    localparam logic [11:0]     CSR_VSCAUSE     = 12'h242;  // Virtual Supervisor Cause
    localparam logic [11:0]     CSR_VSTVAL      = 12'h243;  // Virtual Supervisor Trap Value
    localparam logic [11:0]     CSR_VSIP        = 12'h244;  // Virtual Supervisor Interrupt Pending
    localparam logic [11:0]     CSR_VSATP       = 12'h280;  // Virtual Supervisor Address Translation and Protection

    // ---------------------------------
    // User Mode CSRs (U-Mode / URW)
    // ---------------------------------
    localparam logic [11:0]     CSR_CYCLE     = 12'hC00;    // Cycle Counter
    localparam logic [11:0]     CSR_TIME      = 12'hC01;    // Timer Counter
    localparam logic [11:0]     CSR_INSTRET   = 12'hC02;    // Instruction Retired Counter
    localparam logic [11:0]     CSR_HPMCOUNTER3  = 12'hC03; // HPM Counter 3
    localparam logic [11:0]     CSR_HPMCOUNTER31 = 12'hC1F; // HPM Counter 31

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

    localparam logic [XLEN-1:0] MEDELEG_WRITABLE_MASK =
        (64'h1 << EXC_INST_ADDR_MISALIGNED) |
        (64'h1 << EXC_INST_FAULT) |
        (64'h1 << EXC_INST_ILLEGAL) |
        (64'h1 << EXC_BREAKPOINT) |
        (64'h1 << EXC_LOAD_ADDR_MISALIGNED) |
        (64'h1 << EXC_LOAD_FAULT) |
        (64'h1 << EXC_STORE_ADDR_MISALIGNED) |
        (64'h1 << EXC_STORE_FAULT) |
        (64'h1 << EXC_ECALL_FROM_U) |
        (64'h1 << EXC_ECALL_FROM_S) |
        (64'h1 << EXC_INSTR_PAGE_FAULT) |
        (64'h1 << EXC_LOAD_PAGE_FAULT) |
        (64'h1 << EXC_STORE_PAGE_FAULT);

    localparam logic [XLEN-1:0] MEDELEG_READ_ONLY_ONE_MASK = '0;

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

    localparam logic [XLEN-1:0] MIDELEG_WRITABLE_MASK =
        (64'h1 << INTERRUPT_S_SOFTWARE) |
        (64'h1 << INTERRUPT_S_TIMER) |
        (64'h1 << INTERRUPT_S_EXTERNAL);

    // ---------------------------------
    // Physical Memory Protection (PMP)
    // ---------------------------------
    localparam int PMP_ENTRY_COUNT = 64;
    localparam int PMP_CFG_CSR_COUNT = 16;
    localparam int PMP_CFG_ENTRIES_PER_RV64_CSR = 8;

    typedef logic [7:0]      pmpcfg_entry_t;
    typedef logic [XLEN-1:0] pmpaddr_entry_t;

    localparam int PMPCFG_R_BIT = 0;
    localparam int PMPCFG_W_BIT = 1;
    localparam int PMPCFG_X_BIT = 2;
    localparam int PMPCFG_A_LO  = 3;
    localparam int PMPCFG_A_HI  = 4;
    localparam int PMPCFG_L_BIT = 7;

    localparam logic [1:0] PMP_MODE_OFF   = 2'b00;
    localparam logic [1:0] PMP_MODE_TOR   = 2'b01;
    localparam logic [1:0] PMP_MODE_NA4   = 2'b10;
    localparam logic [1:0] PMP_MODE_NAPOT = 2'b11;

    // ---------------------------------
    // Environment Configuration / State Enable
    // ---------------------------------
    localparam int ENVCFG_FIOM_BIT     = 0;
    localparam int ENVCFG_CBIE_LO      = 4;
    localparam int ENVCFG_CBIE_HI      = 5;
    localparam int ENVCFG_CBCFE_BIT    = 6;
    localparam int ENVCFG_CBZE_BIT     = 7;
    localparam int ENVCFG_PMM_LO       = 32;
    localparam int ENVCFG_PMM_HI       = 33;
    localparam int STATEEN0_ENVCFG_BIT = 62;
    localparam int STATEEN_SE0_BIT     = 63;

endpackage
