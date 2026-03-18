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
    dut->trap_illegal_instr = 0;
    dut->trap_is_ecall = 0;
    dut->trap_is_ebreak = 0;
    dut->trap_is_mret = 0;
    dut->trap_is_sret = 0;
    dut->trap_epc = 0;
}

void apply_reset(Vcsr_unit* dut) {
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;
    tick(dut);
}

void clear_trap_lines(Vcsr_unit* dut) {
    dut->trap_illegal_instr = 0;
    dut->trap_is_ecall = 0;
    dut->trap_is_ebreak = 0;
    dut->trap_is_mret = 0;
    dut->trap_is_sret = 0;
    dut->trap_epc = 0;
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
    dut->trap_is_mret = 1;
    tick(dut);
    dut->trap_is_mret = 0;
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
    const uint64_t misa_before = read_csr(dut, CSR_MISA);
    csr_op(dut, CSR_MISA, CSR_OP_WRITE, 0ULL);
    assert(read_csr(dut, CSR_MISA) == misa_before);
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
    dut->trap_illegal_instr = 1;
    dut->trap_epc = 0xDEADULL;
    tick(dut);
    dut->trap_illegal_instr = 0;
    dut->trap_epc = 0;
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
    dut->trap_is_mret = 1;
    tick(dut);
    dut->trap_is_mret = 0;
    const uint64_t mstatus_after_mret = read_csr(dut, CSR_MSTATUS);
    assert(((mstatus_after_mret >> MSTATUS_MIE_BIT) & 1ULL) == 1ULL);
    assert(((mstatus_after_mret >> MSTATUS_MPIE_BIT) & 1ULL) == 1ULL);
    assert(((mstatus_after_mret >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_U);
    std::cout << "[PASS] mret restores interrupt enable bits" << std::endl;

    // sret restore flow
    uint64_t sstatus_cfg = (1ULL << SSTATUS_SIE_BIT) | (1ULL << SSTATUS_SPP_BIT);
    csr_op(dut, CSR_SSTATUS, CSR_OP_WRITE, sstatus_cfg);
    dut->trap_is_sret = 1;
    tick(dut);
    dut->trap_is_sret = 0;
    const uint64_t sstatus_after_sret = read_csr(dut, CSR_SSTATUS);
    assert(((sstatus_after_sret >> SSTATUS_SIE_BIT) & 1ULL) == 0ULL);
    assert(((sstatus_after_sret >> SSTATUS_SPIE_BIT) & 1ULL) == 1ULL);
    assert(((sstatus_after_sret >> SSTATUS_SPP_BIT) & 1ULL) == 0ULL);
    std::cout << "[PASS] sret updates supervisor status bits" << std::endl;

    // Ecall causes depend on current privilege mode
    force_priv_mode_via_mret(dut, PRIV_ENCODE_S);
    dut->trap_is_ecall = 1;
    dut->trap_epc = 0x600ULL;
    tick(dut);
    dut->trap_is_ecall = 0;
    dut->trap_epc = 0;
    assert(read_csr(dut, CSR_MCAUSE) == EXC_ECALL_FROM_S);
    assert(read_csr(dut, CSR_MTVAL) == 0ULL);
    assert(read_csr(dut, CSR_MEPC) == 0x600ULL);
    assert(((read_csr(dut, CSR_MSTATUS) >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_S);

    force_priv_mode_via_mret(dut, PRIV_ENCODE_U);
    dut->trap_is_ecall = 1;
    dut->trap_epc = 0x700ULL;
    tick(dut);
    dut->trap_is_ecall = 0;
    dut->trap_epc = 0;
    assert(read_csr(dut, CSR_MCAUSE) == EXC_ECALL_FROM_U);
    assert(read_csr(dut, CSR_MTVAL) == 0ULL);
    assert(read_csr(dut, CSR_MEPC) == 0x700ULL);
    assert(((read_csr(dut, CSR_MSTATUS) >> MSTATUS_MPP_LO) & 0x3ULL) == PRIV_ENCODE_U);
    std::cout << "[PASS] Ecall encodes mcause per privilege" << std::endl;

    // Trap delegation from U to S via medeleg
    csr_op(dut, CSR_MEDELEG, CSR_OP_WRITE, 0ULL);
    csr_op(dut, CSR_MEDELEG, CSR_OP_SET, (1ULL << EXC_ECALL_FROM_U));
    csr_op(dut, CSR_STVEC, CSR_OP_WRITE, 0x4000ULL);
    force_priv_mode_via_mret(dut, PRIV_ENCODE_U);
    dut->trap_is_ecall = 1;
    dut->trap_epc = 0x900ULL;
    tick(dut);
    dut->trap_is_ecall = 0;
    dut->trap_epc = 0;
    assert(read_csr(dut, CSR_SEPC) == 0x900ULL);
    assert(read_csr(dut, CSR_SCAUSE) == EXC_ECALL_FROM_U);
    assert(read_csr(dut, CSR_STVAL) == 0ULL);
    assert(dut->csr_priv_mode == PRIV_ENCODE_S);
    assert(dut->csr_trap_to_s_mode);
    assert(dut->csr_trap_vector == 0x4000ULL);
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

    std::cout << "----------------------------------\n";
    std::cout << "All CSR unit tests passed!\n";
    std::cout << "----------------------------------\n";

    delete dut;
    return 0;
}
