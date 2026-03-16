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

// File path: tb/unit/tb_if_id_reg.cc
// Description: IF/ID Pipeline Register unit test

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vif_id_reg.h"

int pass_count = 0;

void tick(Vif_id_reg* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

void check_reg(uint64_t expected_pc, uint32_t expected_instr, uint64_t actual_pc, uint32_t actual_instr, const char* test_name) {
    if (expected_pc == actual_pc && expected_instr == actual_instr) {
        std::cout << "\033[32m[PASS] " << test_name << " | PC = 0x" << std::hex << actual_pc << " Instr = 0x" << actual_instr << "\033[0m" << std::dec << std::endl;
        pass_count++;
    } else {
        std::cout << "\033[31m[FAIL] " << test_name << "\n   Expected: PC=0x" << std::hex << expected_pc << " Instr=0x" << expected_instr 
                  << "\n   Got:      PC=0x" << actual_pc << " Instr=0x" << actual_instr << "\033[0m" << std::dec << std::endl;
        assert(false);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vif_id_reg* dut = new Vif_id_reg;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        IF/ID Register Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // --- 1. Init / Reset ---
    dut->stall_en = 0;
    dut->flush_en = 0;
    dut->if_pc = 0x80000000;
    dut->if_instr = 0xDEADBEEF;
    
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;
    
    // After reset, PC should be 0, instr should be NOP (0x00000013)
    check_reg(0, 0x00000013, dut->id_pc, dut->id_instr, "Reset_Inserts_NOP");

    // --- 2. Normal Propagation ---
    dut->if_pc = 0x80001000;
    dut->if_instr = 0x02A00093; // ADDI x1, x0, 42
    tick(dut);
    check_reg(0x80001000, 0x02A00093, dut->id_pc, dut->id_instr, "Normal_Propagation");

    // --- 3. Stall (Freeze) ---
    dut->if_pc = 0x80001004;
    dut->if_instr = 0x000280B3; // New incoming data
    dut->stall_en = 1;
    tick(dut); // Clock ticks but stall is active
    
    // Register should hold previous values
    check_reg(0x80001000, 0x02A00093, dut->id_pc, dut->id_instr, "Stall_Holds_Previous_Data");

    dut->if_pc = 0x80001008;
    dut->if_instr = 0x008000EF;
    tick(dut); // Still stalled
    check_reg(0x80001000, 0x02A00093, dut->id_pc, dut->id_instr, "Stall_Multiple_Cycles");

    // Release stall
    dut->stall_en = 0;
    tick(dut);
    check_reg(0x80001008, 0x008000EF, dut->id_pc, dut->id_instr, "Release_Stall_Captures_New");

    // --- 4. Flush (Insert NOP) ---
    dut->if_pc = 0x8000100C;
    dut->if_instr = 0xDEADBEEF; // Some invalid instruction
    dut->flush_en = 1;
    tick(dut);
    
    // PC should be 0, instr should be NOP (0x00000013)
    check_reg(0, 0x00000013, dut->id_pc, dut->id_instr, "Flush_Inserts_NOP");

    // --- 5. Stall overrides Flush ? OR Flush overrides Stall ? ---
    // Usually, in RISC-V pipelines, if we have a branch mispredict (Flush),
    // we want to flush even if stalled, OR flush takes priority.
    // Based on if_id_reg.sv, `flush_en` has higher priority than `stall_en`.
    
    // Pre-load good data
    dut->flush_en = 0;
    dut->if_pc = 0x80002000;
    dut->if_instr = 0x12345678;
    tick(dut);
    check_reg(0x80002000, 0x12345678, dut->id_pc, dut->id_instr, "Pre_Flush_Stall_Test");

    // Apply simultaneous flush & stall
    dut->flush_en = 1;
    dut->stall_en = 1;
    tick(dut);
    check_reg(0, 0x00000013, dut->id_pc, dut->id_instr, "Flush_Overrides_Stall");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll " << pass_count << " tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}
