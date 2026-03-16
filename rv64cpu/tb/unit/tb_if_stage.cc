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

// File path: tb/unit/tb_if_stage.cc
// Description: IF Stage unit test

#include <iostream>
#include <iomanip>
#include <cstdint>
#include <cassert>
#include <verilated.h>
#include "Vif_stage.h"

void tick(Vif_stage* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

void check_pc(uint64_t expected_pc, uint64_t actual_pc, const char* test_name) {
    if (expected_pc == actual_pc) {
        std::cout << "\033[32m[PASS] " << test_name << " | PC = 0x" << std::hex << actual_pc << "\033[0m" << std::dec << std::endl;
    } else {
        std::cout << "\033[31m[FAIL] " << test_name << " | Expected PC = 0x" << std::hex << expected_pc << " | Got PC = 0x" << actual_pc << "\033[0m" << std::dec << std::endl;
        assert(false);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vif_stage* dut = new Vif_stage;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "        IF Stage Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    // --- 1. Init / Reset ---
    dut->stall_en = 0;
    dut->ex_branch_taken = 0;
    dut->ex_branch_target = 0;
    
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;
    check_pc(0x80000000ULL, dut->if_pc, "Reset_Boot_Address");

    // --- 2. Normal Execution (PC + 4) ---
    tick(dut);
    check_pc(0x80000004ULL, dut->if_pc, "Normal_Increment_1");
    tick(dut);
    check_pc(0x80000008ULL, dut->if_pc, "Normal_Increment_2");

    // --- 3. Pipeline Stall ---
    dut->stall_en = 1;
    tick(dut);
    check_pc(0x80000008ULL, dut->if_pc, "Stall_Maintains_PC");
    tick(dut);
    check_pc(0x80000008ULL, dut->if_pc, "Stall_Maintains_PC_Again");
    
    // Release Stall
    dut->stall_en = 0;
    tick(dut);
    check_pc(0x8000000CULL, dut->if_pc, "Release_Stall_Normal_Increment");

    // --- 4. Branch Taken ---
    dut->ex_branch_taken = 1;
    dut->ex_branch_target = 0x80000100ULL;
    tick(dut);
    check_pc(0x80000100ULL, dut->if_pc, "Branch_Taken_Updates_PC");

    // Return to normal
    dut->ex_branch_taken = 0;
    tick(dut);
    check_pc(0x80000104ULL, dut->if_pc, "Post_Branch_Normal_Increment");

    // --- 5. Stall and Branch simultaneous priority ---
    // If a branch is taken but the pipeline is stalled, the PC should freeze.
    // However, if the stall clears later, the branch target should theoretically be taken. 
    // In our RV64 simple pipeline design, stall_en freezes the PC reg completely. 
    dut->ex_branch_taken = 1;
    dut->ex_branch_target = 0x80000200ULL;
    dut->stall_en = 1;
    tick(dut);
    check_pc(0x80000104ULL, dut->if_pc, "Stall_Overrides_Branch_Target_Load");
    
    dut->stall_en = 0;
    tick(dut);
    check_pc(0x80000200ULL, dut->if_pc, "Branch_Target_Loaded_After_Stall_Clears");

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll tests passed!\033[0m" << std::endl;
    std::cout << "----------------------------------" << std::endl;

    delete dut;
    return 0;
}