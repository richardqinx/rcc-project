//     Richard CPU
//     Copyright (C) 2026  Richard Qin
//
//     This program is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.
//
//     This program is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.
//
//     You should have received a copy of the GNU General Public License
//     along with this program.  If not, see <https://www.gnu.org/licenses/>.

// File path: tb/unit/tb_csr.cc
// Description: CSR unit testbench

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <verilated.h>

#include "Vcsr_unit.h"

namespace {
constexpr uint32_t CSR_OP_WRITE = 0b001;
constexpr uint32_t CSR_OP_SET   = 0b010;
constexpr uint32_t CSR_OP_CLEAR = 0b011;

constexpr uint16_t CSR_MSTATUS   = 0x300;
constexpr uint16_t CSR_MISA      = 0x301;
constexpr uint16_t CSR_MEDELEG   = 0x302;
constexpr uint16_t CSR_MIDELEG   = 0x303;
constexpr uint16_t CSR_MIE       = 0x304;
constexpr uint16_t CSR_MTVEC     = 0x305;
constexpr uint16_t CSR_MCOUNTEREN= 0x306;
constexpr uint16_t CSR_MSCRATCH  = 0x340;
constexpr uint16_t CSR_MEPC      = 0x341;
constexpr uint16_t CSR_MCAUSE    = 0x342;
constexpr uint16_t CSR_MTVAL     = 0x343;
constexpr uint16_t CSR_MCYCLE    = 0xB00;
constexpr uint16_t CSR_MINSTRET  = 0xB02;
constexpr uint16_t CSR_SSTATUS   = 0x100;
constexpr uint16_t CSR_SIE       = 0x104;
constexpr uint16_t CSR_STVEC     = 0x105;
constexpr uint16_t CSR_SCOUNTEREN= 0x106;
constexpr uint16_t CSR_SSCRATCH  = 0x140;
constexpr uint16_t CSR_SEPC      = 0x141;
constexpr uint16_t CSR_SCAUSE    = 0x142;
constexpr uint16_t CSR_STVAL     = 0x143;
constexpr uint16_t CSR_SIP       = 0x144;
constexpr uint16_t CSR_SATP      = 0x180;
constexpr uint16_t CSR_CYCLE     = 0xC00;
constexpr uint16_t CSR_INSTRET   = 0xC02;
constexpr uint16_t CSR_FFLAGS    = 0x001;
constexpr uint16_t CSR_FRM       = 0x002;
constexpr uint16_t CSR_FCSR      = 0x003;

constexpr uint64_t EXC_INST_ILLEGAL = 2ULL;
constexpr uint64_t EXC_ECALL_FROM_U = 8ULL;
constexpr uint64_t EXC_ECALL_FROM_S = 9ULL;
constexpr uint64_t EXC_LOAD_FAULT   = 5ULL;
constexpr uint64_t EXC_STORE_FAULT  = 7ULL;

constexpr uint64_t INTERRUPT_BIT    = 1ULL << 63;
constexpr uint64_t INT_M_SOFTWARE   = 3ULL;
constexpr uint64_t INT_M_TIMER      = 7ULL;
constexpr uint64_t INT_M_EXTERNAL   = 11ULL;

constexpr int MSTATUS_MIE_BIT  = 3;
constexpr int MSTATUS_MPIE_BIT = 7;
constexpr int MSTATUS_MPP_LO   = 11;
constexpr int MSTATUS_MPP_HI   = 12;
constexpr int SSTATUS_SIE_BIT  = 1;
constexpr int SSTATUS_SPIE_BIT = 5;
constexpr int SSTATUS_SPP_BIT  = 8;

constexpr uint64_t PRIV_ENCODE_U = 0ULL;
constexpr uint64_t PRIV_ENCODE_S = 1ULL;
constexpr uint64_t PRIV_ENCODE_M = 3ULL;
}

void tick(Vcsr_unit* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

void drive_defaults(Vcsr_unit* dut) {
    dut->clk = 0;
    dut->rst = 0;
    dut->csr_req = 0;
    dut->csr_op = 0;
    dut->csr_addr = 0;
    dut->csr_rs1_data = 0;
    dut->trap_ctrl_trigger = 0;
    dut->trap_ctrl_to_s_mode = 0;
    dut->trap_ctrl_mepc = 0;
    dut->trap_ctrl_mcause = 0;
    dut->trap_ctrl_mtval = 0;
    dut->trap_ctrl_mret_en = 0;
    dut->trap_ctrl_sret_en = 0;
}

void apply_reset(Vcsr_unit* dut) {
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;
    tick(dut);
}

void clear_trap_lines(Vcsr_unit* dut) {
    dut->trap_ctrl_trigger = 0;
    dut->trap_ctrl_to_s_mode = 0;
    dut->trap_ctrl_mepc = 0;
    dut->trap_ctrl_mcause = 0;
    dut->trap_ctrl_mtval = 0;
    dut->trap_ctrl_mret_en = 0;
    dut->trap_ctrl_sret_en = 0;
}

void trigger_trap(
    Vcsr_unit* dut,
    uint64_t mepc,
    uint64_t mcause,
    uint64_t mtval,
    bool to_s_mode
) {
    dut->trap_ctrl_trigger = 1;
    dut->trap_ctrl_mepc = mepc;
    dut->trap_ctrl_mcause = mcause;
    dut->trap_ctrl_mtval = mtval;
    dut->trap_ctrl_to_s_mode = to_s_mode ? 1 : 0;
    tick(dut);
    dut->trap_ctrl_trigger = 0;
    dut->trap_ctrl_mepc = 0;
    dut->trap_ctrl_mcause = 0;
    dut->trap_ctrl_mtval = 0;
    dut->trap_ctrl_to_s_mode = 0;
}

void trigger_interrupt(Vcsr_unit* dut, uint64_t mepc, uint64_t int_cause) {
    trigger_trap(dut, mepc, (INTERRUPT_BIT | int_cause), 0ULL, false);
}

template <typename Fn>
void run_csr_scenario(Vcsr_unit* dut, const char* name, Fn&& body) {
    clear_trap_lines(dut);
    std::cout << "[RUN] CSR scenario: " << name << std::endl;
    body(dut);
    std::cout << "[PASS] CSR scenario: " << name << std::endl;
}

uint64_t read_csr(Vcsr_unit* dut, uint16_t addr) {
    dut->csr_addr = addr;
    dut->csr_req = 0;
    dut->csr_op = 0;
    dut->csr_rs1_data = 0;
    dut->eval();
    return dut->csr_rdata;
}

void csr_op(Vcsr_unit* dut, uint16_t addr, uint32_t op, uint64_t data) {
    dut->csr_addr = addr;
    dut->csr_op = op;
    dut->csr_rs1_data = data;
    dut->csr_req = 1;
    tick(dut);
    dut->csr_req = 0;
    dut->csr_op = 0;
    dut->csr_rs1_data = 0;
    dut->csr_addr = 0;
}

void advance_cycles(Vcsr_unit* dut, int cycles) {
    for (int i = 0; i < cycles; ++i) {
        tick(dut);
    }
}

void force_priv_mode_via_mret(Vcsr_unit* dut, uint64_t encoded_mode) {
    uint64_t mstatus = read_csr(dut, CSR_MSTATUS);
    mstatus &= ~(0x3ULL << MSTATUS_MPP_LO);
    mstatus |= (encoded_mode << MSTATUS_MPP_LO);
    csr_op(dut, CSR_MSTATUS, CSR_OP_WRITE, mstatus);
    dut->trap_ctrl_mret_en = 1;
    tick(dut);
    dut->trap_ctrl_mret_en = 0;
}

uint64_t arbitrate_trap_priority(
    bool ext_external,
    bool ext_software,
    bool ext_timer,
    bool has_sync_exception,
    uint64_t sync_cause,
    bool& chose_interrupt
) {
    if (ext_external) {
        chose_interrupt = true;
        return INTERRUPT_BIT | INT_M_EXTERNAL;
    }
    if (ext_software) {
        chose_interrupt = true;
        return INTERRUPT_BIT | INT_M_SOFTWARE;
    }
    if (ext_timer) {
        chose_interrupt = true;
        return INTERRUPT_BIT | INT_M_TIMER;
    }
    chose_interrupt = false;
    return has_sync_exception ? sync_cause : 0ULL;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vcsr_unit* dut = new Vcsr_unit;

    drive_defaults(dut);
    apply_reset(dut);
    clear_trap_lines(dut);

    std::cout << "----------------------------------\n";
    std::cout << "        CSR Unit Testbench\n";
    std::cout << "----------------------------------\n";

    // Reset sanity
    assert(read_csr(dut, CSR_MSTATUS) == 0);
    assert(read_csr(dut, CSR_MIE) == 0);
    assert(read_csr(dut, CSR_MEPC) == 0);
    assert(read_csr(dut, CSR_SSTATUS) == 0);
    assert(read_csr(dut, CSR_SATP) == 0);
    std::cout << "[PASS] Reset clears CSR state" << std::endl;

    // CSRRW basic write
    csr_op(dut, CSR_MTVEC, CSR_OP_WRITE, 0x1020ULL);
    assert(read_csr(dut, CSR_MTVEC) == 0x1020ULL);
    csr_op(dut, CSR_MSCRATCH, CSR_OP_WRITE, 0xCAFEBABEULL);
    assert(read_csr(dut, CSR_MSCRATCH) == 0xCAFEBABEULL);
    std::cout << "[PASS] CSRRW updates mutable CSRs" << std::endl;

    // CSRRS / CSRRC behavior
    csr_op(dut, CSR_MIE, CSR_OP_WRITE, 0);
    csr_op(dut, CSR_MIE, CSR_OP_SET, 0xBuLL);
    assert(read_csr(dut, CSR_MIE) == 0xBuLL);
    csr_op(dut, CSR_MIE, CSR_OP_CLEAR, 0x3ULL);
    assert(read_csr(dut, CSR_MIE) == 0x8ULL);
    csr_op(dut, CSR_MIE, CSR_OP_SET, 0x0ULL);
    assert(read_csr(dut, CSR_MIE) == 0x8ULL);
    std::cout << "[PASS] CSRRS/CSRRC honor masks and ignore zero data" << std::endl;

    // Read-only CSRs ignore write attempts
    const uint64_t misa_ro_snapshot = read_csr(dut, CSR_MISA);
    csr_op(dut, CSR_MISA, CSR_OP_WRITE, 0ULL);
    assert(read_csr(dut, CSR_MISA) == misa_ro_snapshot);
    std::cout << "[PASS] Read-only CSRs reject writes" << std::endl;

    // Canonical aliases for cycle/minstret
    csr_op(dut, CSR_CYCLE, CSR_OP_WRITE, 0x40ULL);
    assert(read_csr(dut, CSR_MCYCLE) == 0x40ULL);
    advance_cycles(dut, 5);
    assert(read_csr(dut, CSR_MCYCLE) == 0x45ULL);
    const uint64_t instret_before = read_csr(dut, CSR_MINSTRET);
    advance_cycles(dut, 3);
    assert(read_csr(dut, CSR_MINSTRET) == instret_before + 3);
    std::cout << "[PASS] Cycle/instret counters tick correctly" << std::endl;

    // Trap should freeze minstret for that cycle
    uint64_t mstatus_cfg = read_csr(dut, CSR_MSTATUS) | (1ULL << MSTATUS_MIE_BIT);
    csr_op(dut, CSR_MSTATUS, CSR_OP_WRITE, mstatus_cfg);
    const uint64_t instret_pre_trap = read_csr(dut, CSR_MINSTRET);
    trigger_trap(dut, 0xDEADULL, EXC_INST_ILLEGAL, 0xDEADULL, false);
    assert(read_csr(dut, CSR_MINSTRET) == instret_pre_trap);
    advance_cycles(dut, 1);
    assert(read_csr(dut, CSR_MINSTRET) == instret_pre_trap + 1);
    std::cout << "[PASS] Trap cycle does not increment minstret" << std::endl;

    // Trap bookkeeping for illegal instruction
    assert(read_csr(dut, CSR_MEPC) == 0xDEADULL);
    assert(read_csr(dut, CSR_MCAUSE) == EXC_INST_ILLEGAL);
    assert(read_csr(dut, CSR_MTVAL) == 0xDEADULL);
    const uint64_t mstatus_after_trap = read_csr(dut, CSR_MSTATUS);
    assert(((mstatus_after_trap >> MSTATUS_MIE_BIT) & 1ULL) == 0ULL);
    assert(((mstatus_after_trap >> MSTATUS_MPIE_BIT) & 1ULL) == 1ULL);
    assert(((mstatus_after_trap >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_M);
    std::cout << "[PASS] Illegal trap updates mepc/mcause/mtval/mstatus" << std::endl;

    // mret restore flow
    dut->trap_ctrl_mret_en = 1;
    tick(dut);
    dut->trap_ctrl_mret_en = 0;
    const uint64_t mstatus_after_mret_reset = read_csr(dut, CSR_MSTATUS);
    assert(((mstatus_after_mret_reset >> MSTATUS_MIE_BIT) & 1ULL) == 1ULL);
    assert(((mstatus_after_mret_reset >> MSTATUS_MPIE_BIT) & 1ULL) == 1ULL);
    assert(((mstatus_after_mret_reset >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_U);
    std::cout << "[PASS] mret restores interrupt enable bits" << std::endl;

    // sret restore flow
    uint64_t sstatus_cfg = (1ULL << SSTATUS_SIE_BIT) | (1ULL << SSTATUS_SPP_BIT);
    csr_op(dut, CSR_SSTATUS, CSR_OP_WRITE, sstatus_cfg);
    dut->trap_ctrl_sret_en = 1;
    tick(dut);
    dut->trap_ctrl_sret_en = 0;
    const uint64_t sstatus_after_sret_reset = read_csr(dut, CSR_SSTATUS);
    assert(((sstatus_after_sret_reset >> SSTATUS_SIE_BIT) & 1ULL) == 0ULL);
    assert(((sstatus_after_sret_reset >> SSTATUS_SPIE_BIT) & 1ULL) == 1ULL);
    assert(((sstatus_after_sret_reset >> SSTATUS_SPP_BIT) & 1ULL) == 0ULL);
    std::cout << "[PASS] sret updates supervisor status bits" << std::endl;

    // Ecall causes depend on current privilege mode
    force_priv_mode_via_mret(dut, PRIV_ENCODE_S);
    trigger_trap(dut, 0x600ULL, EXC_ECALL_FROM_S, 0ULL, false);
    assert(read_csr(dut, CSR_MCAUSE) == EXC_ECALL_FROM_S);
    assert(read_csr(dut, CSR_MTVAL) == 0ULL);
    assert(read_csr(dut, CSR_MEPC) == 0x600ULL);
    assert(((read_csr(dut, CSR_MSTATUS) >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_S);

    force_priv_mode_via_mret(dut, PRIV_ENCODE_U);
    trigger_trap(dut, 0x700ULL, EXC_ECALL_FROM_U, 0ULL, false);
    assert(read_csr(dut, CSR_MCAUSE) == EXC_ECALL_FROM_U);
    assert(read_csr(dut, CSR_MTVAL) == 0ULL);
    assert(read_csr(dut, CSR_MEPC) == 0x700ULL);
    assert(((read_csr(dut, CSR_MSTATUS) >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_U);
    std::cout << "[PASS] Ecall encodes mcause per privilege" << std::endl;

    // Trap delegation from U to S via medeleg
    csr_op(dut, CSR_MEDELEG, CSR_OP_WRITE, 0ULL);
    csr_op(dut, CSR_MEDELEG, CSR_OP_SET, (1ULL << EXC_ECALL_FROM_U));
    csr_op(dut, CSR_STVEC, CSR_OP_WRITE, 0x4000ULL);
    assert(read_csr(dut, CSR_MEDELEG) & (1ULL << EXC_ECALL_FROM_U));
    force_priv_mode_via_mret(dut, PRIV_ENCODE_U);
    assert(dut->csr_priv_mode == PRIV_ENCODE_U);
    trigger_trap(dut, 0x900ULL, EXC_ECALL_FROM_U, 0ULL, true);
    assert(dut->csr_trap_to_s_mode);
    assert(dut->csr_trap_vector == 0x4000ULL);
    assert(read_csr(dut, CSR_SEPC) == 0x900ULL);
    assert(read_csr(dut, CSR_SCAUSE) == EXC_ECALL_FROM_U);
    assert(read_csr(dut, CSR_STVAL) == 0ULL);
    assert(dut->csr_priv_mode == PRIV_ENCODE_S);
    std::cout << "[PASS] medeleg routes ecall to supervisor" << std::endl;
    force_priv_mode_via_mret(dut, PRIV_ENCODE_M);

    // Basic supervisor CSRs touching for completeness
    csr_op(dut, CSR_SIE, CSR_OP_WRITE, 0x55ULL);
    csr_op(dut, CSR_SSCRATCH, CSR_OP_WRITE, 0xAAULL);
    csr_op(dut, CSR_SEPC, CSR_OP_WRITE, 0x1234ULL);
    csr_op(dut, CSR_SCAUSE, CSR_OP_WRITE, 0x21ULL);
    csr_op(dut, CSR_SIP, CSR_OP_WRITE, 0x3ULL);
    csr_op(dut, CSR_SATP, CSR_OP_WRITE, 0x8000000000000000ULL);
    assert(read_csr(dut, CSR_SIE) == 0x55ULL);
    assert(read_csr(dut, CSR_SSCRATCH) == 0xAAULL);
    assert(read_csr(dut, CSR_SEPC) == 0x1234ULL);
    assert(read_csr(dut, CSR_SCAUSE) == 0x21ULL);
    assert(read_csr(dut, CSR_SIP) == 0x3ULL);
    assert(read_csr(dut, CSR_SATP) == 0x8000000000000000ULL);
    std::cout << "[PASS] Supervisor space CSR read/write" << std::endl;

    // Writing SSTATUS should mirror MSTATUS subset bits
    csr_op(dut, CSR_SSTATUS, CSR_OP_WRITE, (1ULL << SSTATUS_SIE_BIT));
    assert(((read_csr(dut, CSR_MSTATUS) >> SSTATUS_SIE_BIT) & 1ULL) == 1ULL);
    csr_op(dut, CSR_SSTATUS, CSR_OP_WRITE, 0ULL);
    assert(((read_csr(dut, CSR_MSTATUS) >> SSTATUS_SIE_BIT) & 1ULL) == 0ULL);
    std::cout << "[PASS] SSTATUS write mirrors MSTATUS" << std::endl;

    // Floating-point CSR block
    csr_op(dut, CSR_FFLAGS, CSR_OP_WRITE, 0x1FULL);
    assert((read_csr(dut, CSR_FFLAGS) & 0x1FULL) == 0x1FULL);
    csr_op(dut, CSR_FRM, CSR_OP_WRITE, 0x7ULL);
    assert((read_csr(dut, CSR_FRM) & 0x7ULL) == 0x7ULL);
    csr_op(dut, CSR_FCSR, CSR_OP_WRITE, 0xABULL);
    assert((read_csr(dut, CSR_FFLAGS) & 0x1FULL) == (0xABULL & 0x1FULL));
    assert((read_csr(dut, CSR_FRM) & 0x7ULL) == ((0xABULL >> 5) & 0x7ULL));
    std::cout << "[PASS] FCSR/FM/FFLAGS read/write" << std::endl;

    std::cout << "[INFO] Extending CSR trap coverage" << std::endl;

    run_csr_scenario(dut, "interrupt_priority_matrix", [&](Vcsr_unit* target) {
        struct PriorityScenario {
            const char* name;
            bool ext_external;
            bool ext_software;
            bool ext_timer;
            bool sync_illegal;
            uint64_t mepc;
            uint64_t mtval;
            uint64_t expected_cause;
        };

        const std::array<PriorityScenario, 5> suite = {{
            {"external_over_others", true,  true,  true,  true,  0xA000ULL, 0x1111ULL, INTERRUPT_BIT | INT_M_EXTERNAL},
            {"software_over_timer",  false, true,  true,  true,  0xA040ULL, 0x2222ULL, INTERRUPT_BIT | INT_M_SOFTWARE},
            {"timer_over_sync",      false, false, true,  true,  0xA080ULL, 0x3333ULL, INTERRUPT_BIT | INT_M_TIMER},
            {"interrupt_only",       true,  false, false, false, 0xA0C0ULL, 0x0ULL,     INTERRUPT_BIT | INT_M_EXTERNAL},
            {"sync_fallback",        false, false, false, true,  0xA100ULL, 0x4444ULL, EXC_INST_ILLEGAL}
        }};

        for (const auto& scenario : suite) {
            bool chose_interrupt = false;
            const uint64_t chosen_cause = arbitrate_trap_priority(
                scenario.ext_external,
                scenario.ext_software,
                scenario.ext_timer,
                scenario.sync_illegal,
                EXC_INST_ILLEGAL,
                chose_interrupt
            );
            assert(chosen_cause == scenario.expected_cause);
            trigger_trap(
                target,
                scenario.mepc,
                chosen_cause,
                scenario.sync_illegal ? scenario.mtval : 0ULL,
                false
            );
            const uint64_t mcause = read_csr(target, CSR_MCAUSE);
            assert(mcause == scenario.expected_cause);
            assert(read_csr(target, CSR_MEPC) == scenario.mepc);
            const bool mcause_is_interrupt = (mcause & INTERRUPT_BIT) != 0ULL;
            assert(mcause_is_interrupt == chose_interrupt);
            if (scenario.sync_illegal && !mcause_is_interrupt) {
                assert(read_csr(target, CSR_MTVAL) == scenario.mtval);
            }
            assert(target->csr_priv_mode == PRIV_ENCODE_M);
        }
    });

    run_csr_scenario(dut, "delegated_multitrap_context", [&](Vcsr_unit* target) {
        csr_op(target, CSR_MEDELEG, CSR_OP_WRITE, 0ULL);
        csr_op(target, CSR_MEDELEG, CSR_OP_SET, (1ULL << EXC_ECALL_FROM_U));
        csr_op(target, CSR_STVEC, CSR_OP_WRITE, 0x4000ULL);
        csr_op(target, CSR_MTVEC, CSR_OP_WRITE, 0x8000ULL);

        force_priv_mode_via_mret(target, PRIV_ENCODE_S);
        assert(target->csr_priv_mode == PRIV_ENCODE_S);

        const uint64_t delegated_pc = 0x900ULL;
        trigger_trap(target, delegated_pc, EXC_ECALL_FROM_U, 0ULL, true);
        assert(target->csr_trap_to_s_mode);
        assert(target->csr_trap_vector == 0x4000ULL);
        assert(read_csr(target, CSR_SEPC) == delegated_pc);
        assert(read_csr(target, CSR_SCAUSE) == EXC_ECALL_FROM_U);
        assert(target->csr_priv_mode == PRIV_ENCODE_S);

        const uint64_t machine_pc = 0xA00ULL;
        trigger_trap(target, machine_pc, EXC_INST_ILLEGAL, 0ULL, false);
        assert(!target->csr_trap_to_s_mode);
        assert(target->csr_trap_vector == 0x8000ULL);
        assert(read_csr(target, CSR_MEPC) == machine_pc);
        assert(read_csr(target, CSR_SEPC) == delegated_pc);
        assert(target->csr_priv_mode == PRIV_ENCODE_M);
    });

    run_csr_scenario(dut, "nested_return_sequence", [&](Vcsr_unit* target) {
        force_priv_mode_via_mret(target, PRIV_ENCODE_U);
        csr_op(target, CSR_SSTATUS, CSR_OP_WRITE, (1ULL << SSTATUS_SIE_BIT));

        const uint64_t first_trap_pc = 0xB00ULL;
        trigger_trap(target, first_trap_pc, EXC_ECALL_FROM_U, 0ULL, true);
        uint64_t sstatus_snapshot = read_csr(target, CSR_SSTATUS);
        assert(((sstatus_snapshot >> SSTATUS_SIE_BIT) & 1ULL) == 0ULL);
        assert(((sstatus_snapshot >> SSTATUS_SPIE_BIT) & 1ULL) == 1ULL);
        assert(((sstatus_snapshot >> SSTATUS_SPP_BIT) & 1ULL) == 0ULL);
        assert(read_csr(target, CSR_SEPC) == first_trap_pc);

        target->trap_ctrl_sret_en = 1;
        tick(target);
        target->trap_ctrl_sret_en = 0;
        uint64_t sstatus_after_sret = read_csr(target, CSR_SSTATUS);
        assert(((sstatus_after_sret >> SSTATUS_SIE_BIT) & 1ULL) == 1ULL);
        assert(((sstatus_after_sret >> SSTATUS_SPIE_BIT) & 1ULL) == 1ULL);
        assert(target->csr_priv_mode == PRIV_ENCODE_U);

        // Enable machine interrupts so the subsequent machine trap captures pre-trap MIE=1
        csr_op(target, CSR_MSTATUS, CSR_OP_SET, (1ULL << MSTATUS_MIE_BIT));
        const uint64_t second_trap_pc = 0xB80ULL;
        trigger_trap(target, second_trap_pc, EXC_INST_ILLEGAL, 0ULL, false);
        uint64_t mstatus_snapshot = read_csr(target, CSR_MSTATUS);
        assert(((mstatus_snapshot >> MSTATUS_MIE_BIT) & 1ULL) == 0ULL);
        assert(((mstatus_snapshot >> MSTATUS_MPIE_BIT) & 1ULL) == 1ULL);
        assert(((mstatus_snapshot >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_U);
        assert(read_csr(target, CSR_MEPC) == second_trap_pc);

        target->trap_ctrl_mret_en = 1;
        tick(target);
        target->trap_ctrl_mret_en = 0;
        uint64_t mstatus_after_mret = read_csr(target, CSR_MSTATUS);
        assert(((mstatus_after_mret >> MSTATUS_MIE_BIT) & 1ULL) == 1ULL);
        assert(((mstatus_after_mret >> MSTATUS_MPIE_BIT) & 1ULL) == 1ULL);
        assert(((mstatus_after_mret >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_U);
        assert(target->csr_priv_mode == PRIV_ENCODE_U);
    });

    run_csr_scenario(dut, "illegal_csr_access_guard", [&](Vcsr_unit* target) {
        force_priv_mode_via_mret(target, PRIV_ENCODE_M);
        assert(target->csr_priv_mode == PRIV_ENCODE_M);
        const uint64_t misa_snapshot = read_csr(target, CSR_MISA);
        target->csr_addr = CSR_MISA;
        target->csr_op = CSR_OP_WRITE;
        target->csr_rs1_data = 0x1234ULL;
        target->csr_req = 1;
        tick(target);
        target->csr_req = 0;
        target->csr_op = 0;
        target->csr_rs1_data = 0;
        target->csr_addr = 0;
        assert(target->csr_illegal_access);
        assert(read_csr(target, CSR_MISA) == misa_snapshot);
        assert(!target->csr_trap_to_s_mode);
        assert(target->csr_priv_mode == PRIV_ENCODE_M);
        assert(target->csr_trap_vector_next == read_csr(target, CSR_MTVEC));
        tick(target);
        assert(!target->csr_illegal_access);
    });

    run_csr_scenario(dut, "delegation_cross_over", [&](Vcsr_unit* target) {
        csr_op(target, CSR_MTVEC, CSR_OP_WRITE, 0x1000ULL);
        csr_op(target, CSR_STVEC, CSR_OP_WRITE, 0x2000ULL);
        csr_op(target, CSR_MEDELEG, CSR_OP_WRITE, 0ULL);
        csr_op(target, CSR_MIDELEG, CSR_OP_WRITE, 0ULL);

        struct DelegationProbe {
            const char* name;
            bool is_interrupt;
            uint64_t cause;
            bool delegate_to_s;
            uint64_t pc;
        };

        const std::array<DelegationProbe, 4> probes = {{
            {"medeleg_set",   false, EXC_ECALL_FROM_U, true,  0xD000ULL},
            {"medeleg_clear", false, EXC_ECALL_FROM_U, false, 0xD040ULL},
            {"mideleg_set",   true,  INT_M_TIMER,      true,  0xD080ULL},
            {"mideleg_clear", true,  INT_M_TIMER,      false, 0xD0C0ULL}
        }};

        for (const auto& probe : probes) {
            if (probe.is_interrupt) {
                const uint64_t mask = (1ULL << probe.cause);
                if (probe.delegate_to_s) {
                    csr_op(target, CSR_MIDELEG, CSR_OP_SET, mask);
                } else {
                    csr_op(target, CSR_MIDELEG, CSR_OP_CLEAR, mask);
                }
                force_priv_mode_via_mret(target, PRIV_ENCODE_S);
                trigger_trap(target, probe.pc, (INTERRUPT_BIT | probe.cause), 0ULL, probe.delegate_to_s);
                if (probe.delegate_to_s) {
                    assert(target->csr_trap_to_s_mode);
                    assert(read_csr(target, CSR_SEPC) == probe.pc);
                } else {
                    assert(!target->csr_trap_to_s_mode);
                    assert(read_csr(target, CSR_MEPC) == probe.pc);
                }
            } else {
                const uint64_t mask = (1ULL << probe.cause);
                if (probe.delegate_to_s) {
                    csr_op(target, CSR_MEDELEG, CSR_OP_SET, mask);
                } else {
                    csr_op(target, CSR_MEDELEG, CSR_OP_CLEAR, mask);
                }
                force_priv_mode_via_mret(target, PRIV_ENCODE_U);
                trigger_trap(target, probe.pc, probe.cause, 0ULL, probe.delegate_to_s);
                if (probe.delegate_to_s) {
                    assert(target->csr_trap_to_s_mode);
                    assert(read_csr(target, CSR_SEPC) == probe.pc);
                } else {
                    assert(!target->csr_trap_to_s_mode);
                    assert(read_csr(target, CSR_MEPC) == probe.pc);
                }
            }

            const uint64_t expected_vector = probe.delegate_to_s ? read_csr(target, CSR_STVEC)
                                                                  : read_csr(target, CSR_MTVEC);
            assert(target->csr_trap_vector == expected_vector);
        }
    });

    std::cout << "----------------------------------\n";
    std::cout << "All CSR unit tests passed!\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
