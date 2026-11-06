//     Richard CPU
// Copyright (C) 2026  Richard Qin
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// File path: tb/unit/tb_trap_ctrl.cc
// Description: Standalone unit test for trap_ctrl exception/interrupt control logic

#include <cassert>
#include <cstdint>
#include <iostream>
#include <verilated.h>

#include "Vtrap_ctrl.h"

namespace {
constexpr uint64_t EXC_INST_ILLEGAL   = 2ULL;
constexpr uint64_t EXC_ECALL_FROM_S   = 9ULL;
constexpr uint64_t EXC_LOAD_PAGE_FAULT= 13ULL;
constexpr uint64_t EXC_STORE_PAGE_FAULT= 15ULL;
constexpr uint64_t EXC_LOAD_FAULT     = 5ULL;
constexpr uint64_t EXC_STORE_FAULT    = 7ULL;

constexpr uint64_t INTERRUPT_M_SOFTWARE = 3ULL;
constexpr uint64_t INTERRUPT_M_TIMER    = 7ULL;
constexpr uint64_t INTERRUPT_M_EXTERNAL = 11ULL;
constexpr uint64_t INTERRUPT_S_SOFTWARE = 1ULL;
constexpr uint64_t INTERRUPT_S_TIMER    = 5ULL;
constexpr uint64_t INTERRUPT_S_EXTERNAL = 9ULL;

constexpr uint64_t INTERRUPT_BIT      = 1ULL << 63;
constexpr uint64_t PRIV_MODE_U        = 0ULL;
constexpr uint64_t PRIV_MODE_S        = 1ULL;
constexpr uint64_t PRIV_MODE_M        = 3ULL;

uint64_t mie_bit(uint64_t interrupt_cause) {
    return 1ULL << interrupt_cause;
}

void drive_defaults(Vtrap_ctrl* dut) {
    dut->wb_pc = 0;
    dut->trap_illegal_instr = 0;
    dut->trap_is_ecall = 0;
    dut->trap_is_ebreak = 0;
    dut->trap_is_mret = 0;
    dut->trap_is_sret = 0;
    dut->trap_load_fault = 0;
    dut->trap_store_fault = 0;
    dut->trap_instr_page_fault = 0;
    dut->trap_load_page_fault = 0;
    dut->trap_store_page_fault = 0;
    dut->trap_bad_addr = 0;
    dut->trap_bad_instr = 0;

    dut->csr_mstatus_mie = 0;
    dut->csr_mie = 0;
    dut->csr_sie = 0;
    dut->csr_mip = 0;
    dut->csr_sip = 0;
    dut->csr_mtvec = 0x800ULL;
    dut->csr_stvec = 0x400ULL;
    dut->csr_mepc = 0x1000ULL;
    dut->csr_sepc = 0x2000ULL;
    dut->csr_medeleg = 0ULL;
    dut->csr_mideleg = 0ULL;
    dut->csr_priv_mode = PRIV_MODE_M;

    dut->eval();
}

int pass_count = 0;

void check(bool cond, const char* name) {
    if (cond) {
        std::cout << "\033[32m[PASS] " << name << "\033[0m" << std::endl;
        pass_count++;
    } else {
        std::cout << "\033[31m[FAIL] " << name << "\033[0m" << std::endl;
        assert(false);
    }
}
}  // namespace

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vtrap_ctrl* dut = new Vtrap_ctrl;

    std::cout << "----------------------------------\n";
    std::cout << "        trap_ctrl Testbench\n";
    std::cout << "----------------------------------\n";

    // ---------------------------------------------------------------------
    // 1) Synchronous exceptions (illegal instruction, ecall, page fault)
    // ---------------------------------------------------------------------
    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_M;
    dut->csr_mtvec = 0x880ULL;
    dut->wb_pc = 0x1234ULL;
    dut->trap_bad_instr = 0xDEADBEEFu;
    dut->trap_illegal_instr = 1;
    dut->eval();
    check(dut->trap_trigger, "IllegalInst_TriggersTrap");
    check(dut->trap_mcause == EXC_INST_ILLEGAL, "IllegalInst_Mcause");
    check(dut->trap_mtval == 0xDEADBEEFULL, "IllegalInst_Mtval");
    check(dut->trap_mepc == 0x1234ULL, "IllegalInst_Mepc");
    check(!dut->trap_to_s_mode, "IllegalInst_NoDelegation");
    check(dut->trap_next_pc == 0x880ULL, "IllegalInst_JumpsToMtvec");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_S;
    dut->csr_mtvec = 0x440ULL;
    dut->wb_pc = 0x5678ULL;
    dut->trap_is_ecall = 1;
    dut->eval();
    check(dut->trap_trigger, "EcallS_TriggersTrap");
    check(dut->trap_mcause == EXC_ECALL_FROM_S, "EcallS_Mcause");
    check(dut->trap_mtval == 0ULL, "EcallS_MtvalZero");
    check(dut->trap_next_pc == 0x440ULL, "EcallS_JumpsToMtvec");

    drive_defaults(dut);
    dut->trap_load_page_fault = 1;
    dut->trap_bad_addr = 0xBADC0FFEE0ULL;
    dut->eval();
    check(dut->trap_mcause == EXC_LOAD_PAGE_FAULT, "LoadPageFault_Mcause");
    check(dut->trap_mtval == 0xBADC0FFEE0ULL, "LoadPageFault_Mtval");

    drive_defaults(dut);
    dut->trap_store_page_fault = 1;
    dut->trap_bad_addr = 0xDEADBEEF00ULL;
    dut->eval();
    check(dut->trap_mcause == EXC_STORE_PAGE_FAULT, "StorePageFault_Mcause");
    check(dut->trap_mtval == 0xDEADBEEF00ULL, "StorePageFault_Mtval");

    drive_defaults(dut);
    dut->trap_load_fault = 1;
    dut->trap_bad_addr = 0xABCDEF0010ULL;
    dut->eval();
    check(dut->trap_mcause == EXC_LOAD_FAULT, "LoadFault_Mcause");
    check(dut->trap_mtval == 0xABCDEF0010ULL, "LoadFault_Mtval");

    drive_defaults(dut);
    dut->trap_store_fault = 1;
    dut->trap_bad_addr = 0xABCDEF0020ULL;
    dut->eval();
    check(dut->trap_mcause == EXC_STORE_FAULT, "StoreFault_Mcause");
    check(dut->trap_mtval == 0xABCDEF0020ULL, "StoreFault_Mtval");

    // ---------------------------------------------------------------------
    // 2) Interrupt priority + enable masking
    // ---------------------------------------------------------------------
    drive_defaults(dut);
    dut->csr_mstatus_mie = 1;
    dut->csr_mie = mie_bit(INTERRUPT_M_EXTERNAL) |
                   mie_bit(INTERRUPT_M_SOFTWARE) |
                   mie_bit(INTERRUPT_M_TIMER);
    dut->csr_priv_mode = PRIV_MODE_M;
    dut->csr_mtvec = 0x300ULL | 0x1ULL;  // vectored mode
    dut->wb_pc = 0x4000ULL;
    dut->csr_mip = mie_bit(INTERRUPT_M_EXTERNAL) |
                   mie_bit(INTERRUPT_M_SOFTWARE) |
                   mie_bit(INTERRUPT_M_TIMER);
    dut->eval();
    const uint64_t expected_vectored_pc = 0x300ULL + (INTERRUPT_M_EXTERNAL << 2);
    check(dut->trap_trigger, "InterruptPriority_Triggers");
    check(dut->trap_mcause == (INTERRUPT_BIT | INTERRUPT_M_EXTERNAL), "InterruptPriority_Mcause");
    check(dut->trap_mtval == 0ULL, "InterruptPriority_MtvalZero");
    check(dut->trap_next_pc == expected_vectored_pc, "InterruptPriority_VectoredJump");

    drive_defaults(dut);
    dut->csr_mstatus_mie = 0;
    dut->csr_mie = mie_bit(INTERRUPT_M_EXTERNAL);
    dut->csr_mip = mie_bit(INTERRUPT_M_EXTERNAL);
    dut->eval();
    check(!dut->trap_trigger, "InterruptMaskedByGlobalMie_NoTrigger");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_M;
    dut->csr_mstatus_mie = 1;
    dut->csr_mie = 0;
    dut->csr_mip = mie_bit(INTERRUPT_M_EXTERNAL);
    dut->eval();
    check(!dut->trap_trigger, "InterruptMaskedByMie_NoTrigger");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_S;
    dut->csr_mstatus_mie = 0;
    dut->csr_mie = mie_bit(INTERRUPT_M_EXTERNAL);
    dut->csr_mtvec = 0x440ULL;
    dut->wb_pc = 0x6780ULL;
    dut->csr_mip = mie_bit(INTERRUPT_M_EXTERNAL);
    dut->eval();
    check(dut->trap_trigger, "MachineInterruptBelowM_IgnoresMstatusMie");
    check(!dut->trap_to_s_mode, "MachineInterruptBelowM_NoDelegationWithoutMideleg");
    check(dut->trap_mcause == (INTERRUPT_BIT | INTERRUPT_M_EXTERNAL), "MachineInterruptBelowM_Mcause");
    check(dut->trap_next_pc == 0x440ULL, "MachineInterruptBelowM_JumpsToMtvec");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_S;
    dut->csr_mstatus_mie = 1;
    dut->csr_mie = mie_bit(INTERRUPT_M_TIMER);
    dut->csr_mtvec = 0x880ULL | 0x1ULL;  // vectored machine mode
    dut->csr_stvec = 0x400ULL | 0x1ULL;
    dut->csr_mideleg = (1ULL << INTERRUPT_M_TIMER);
    dut->wb_pc = 0xABCDULL;
    dut->csr_mip = mie_bit(INTERRUPT_M_TIMER);
    dut->eval();
    const uint64_t expected_sv_pc = 0x880ULL + (INTERRUPT_M_TIMER << 2);
    check(!dut->trap_to_s_mode, "MachineInterrupt_NoDelegation");
    check(dut->trap_next_pc == expected_sv_pc, "MachineInterrupt_UsesMtvec");
    check(dut->trap_mcause == (INTERRUPT_BIT | INTERRUPT_M_TIMER), "MachineInterrupt_Mcause");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_S;
    dut->csr_mstatus = (1ULL << 1);
    dut->csr_sie = mie_bit(INTERRUPT_S_TIMER);
    dut->csr_sip = mie_bit(INTERRUPT_S_TIMER);
    dut->csr_mideleg = mie_bit(INTERRUPT_S_TIMER);
    dut->csr_stvec = 0x900ULL | 0x1ULL;
    dut->wb_pc = 0xABCEULL;
    dut->eval();
    const uint64_t expected_s_timer_pc = 0x900ULL + (INTERRUPT_S_TIMER << 2);
    check(dut->trap_to_s_mode, "SupervisorInterruptDelegated_ToSupervisor");
    check(dut->trap_next_pc == expected_s_timer_pc, "SupervisorInterruptDelegated_UsesStvec");
    check(dut->trap_mcause == (INTERRUPT_BIT | INTERRUPT_S_TIMER), "SupervisorInterruptDelegated_Mcause");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_S;
    dut->csr_mstatus = 0;
    dut->csr_sie = mie_bit(INTERRUPT_S_TIMER);
    dut->csr_sip = mie_bit(INTERRUPT_S_TIMER);
    dut->csr_mideleg = mie_bit(INTERRUPT_S_TIMER);
    dut->eval();
    check(!dut->trap_trigger, "SupervisorInterruptMaskedBySstatusSie_NoTrigger");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_U;
    dut->csr_mstatus = 0;
    dut->csr_sie = mie_bit(INTERRUPT_S_EXTERNAL);
    dut->csr_sip = mie_bit(INTERRUPT_S_EXTERNAL);
    dut->csr_mideleg = mie_bit(INTERRUPT_S_EXTERNAL);
    dut->csr_stvec = 0x940ULL;
    dut->eval();
    check(dut->trap_to_s_mode, "SupervisorInterruptBelowS_IgnoresSstatusSie");
    check(dut->trap_next_pc == 0x940ULL, "SupervisorInterruptBelowS_UsesStvec");

    // ---------------------------------------------------------------------
    // 3) MRET/SRET redirect without new traps
    // ---------------------------------------------------------------------
    drive_defaults(dut);
    dut->csr_mepc = 0xCAFEBABEULL;
    dut->trap_is_mret = 1;
    dut->eval();
    check(dut->mret_en, "Mret_PathEnabled");
    check(dut->trap_next_pc == 0xCAFEBABEULL, "Mret_RestoresMepc");
    check(dut->trap_flush_req && dut->trap_pc_sel, "Mret_ForcesFlush");
    check(!dut->trap_trigger, "Mret_NoTrapOverlap");

    drive_defaults(dut);
    dut->csr_priv_mode = PRIV_MODE_S;
    dut->csr_sepc = 0x13579BDFULL;
    dut->trap_is_sret = 1;
    dut->eval();
    check(dut->sret_en, "Sret_PathEnabled");
    check(dut->trap_next_pc == 0x13579BDFULL, "Sret_RestoresSepc");
    check(dut->trap_flush_req && dut->trap_pc_sel, "Sret_ForcesFlush");

    std::cout << "----------------------------------\n";
    std::cout << "All " << pass_count << " trap_ctrl tests passed!\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
