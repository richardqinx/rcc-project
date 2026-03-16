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

// File path: tb/unit/tb_regfile.cc
// Description: Register file unit test

#include <iostream>
#include <cassert>
#include <verilated.h>
#include "Vregfile.h"

void tick(Vregfile* dut) {
    dut->clk = 0;
    dut->eval();
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Vregfile* dut = new Vregfile;

    dut->clk = 0;
    dut->rst = 0;
    dut->reg_write_en = 0;
    dut->rd_addr = 0;
    dut->rd_wdata = 0;
    dut->rs1_addr = 0;
    dut->rs2_addr = 0;
    
    std::cout << "----------------------------------" << std::endl;
    std::cout << "        Register File Unit Test" << std::endl;
    std::cout << "----------------------------------" << std::endl;
    
    // Test 1: Reset
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;
    std::cout << "\033[32m[PASS] Reset complete\033[0m" << std::endl;

    // Test 2: Write and read
    dut->reg_write_en = 1;
    dut->rd_addr = 5;
    dut->rd_wdata = 0xDEADBEEF;
    tick(dut);

    dut->reg_write_en = 0;
    dut->rs1_addr = 5;
    dut->eval();
    assert(dut->rs1_rdata == 0xDEADBEEF);
    std::cout << "\033[32m[PASS] Write and read (x5) complete\033[0m" << std::endl;

    // Test 3: x0
    dut->reg_write_en = 1;
    dut->rd_addr = 0;
    dut->rd_wdata = 0x12345678;
    tick(dut);
    
    dut->reg_write_en = 0;
    dut->rs2_addr = 0;
    dut->eval();
    assert(dut->rs2_rdata == 0);
    std::cout << "\033[32m[PASS] Write and read (x0) complete\033[0m" << std::endl;

    // Test 4: Dual-port simultaneous read
    dut->reg_write_en = 1;
    dut->rd_addr = 6;
    dut->rd_wdata = 0x87654321;
    tick(dut);

    dut->reg_write_en = 1;
    dut->rd_addr = 7;
    dut->rd_wdata = 0x11223344;
    tick(dut);

    dut->rs1_addr = 6;
    dut->rs2_addr = 7;
    dut->eval();

    assert(dut->rs1_rdata == 0x87654321 && dut->rs2_rdata == 0x11223344);
    std::cout << "\033[32m[PASS] Dual-port simultaneous read complete\033[0m" << std::endl;


    // Test 5: Write enable = 0 (Data should not change)
    dut->reg_write_en = 1;
    dut->rd_addr = 10;
    dut->rd_wdata = 0xCAFEBABE;
    tick(dut);

    dut->reg_write_en = 0;  // Disable write
    dut->rd_addr = 10;
    dut->rd_wdata = 0x11111111; // Attempt to overwrite
    tick(dut);

    dut->rs1_addr = 10;
    dut->eval();
    assert(dut->rs1_rdata == 0xCAFEBABE);
    std::cout << "\033[32m[PASS] Write enable = 0 correctly ignores write\033[0m" << std::endl;

    // Test 6: Verify full reset behavior
    // First, dirty up some registers
    for (int i = 1; i < 32; i += 5) {
        dut->reg_write_en = 1;
        dut->rd_addr = i;
        dut->rd_wdata = 0x1000 + i;
        tick(dut);
    }
    
    // Apply reset
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;

    // Check all are zero
    bool reset_ok = true;
    for (int i = 1; i < 32; i++) {
        dut->rs1_addr = i;
        dut->eval();
        if (dut->rs1_rdata != 0) {
            reset_ok = false;
            std::cerr << "\033[31m[FAIL] Reset failed on register x" << i << std::endl;
        }
    }
    assert(reset_ok);
    std::cout << "\033[32m[PASS] Full memory reset verified\033[0m" << std::endl;

    // Test 7: Read-After-Write (Same cycle collision check)
    // Combinational read should return the OLD value on the cycle WE is asserted
    // NOTE: Verilator handles combinational loops. Before the clock edge, it evaluates.
    dut->reg_write_en = 1;
    dut->rd_addr = 15;
    dut->rd_wdata = 0x55555555; // Pseudo
    
    // Setup for RAW
    dut->reg_write_en = 1;
    dut->rd_addr = 15;
    dut->rd_wdata = 0x99999999;
    
    // Before tick, read should be 0 (from previous reset)
    dut->rs1_addr = 15;
    dut->eval();
    assert(dut->rs1_rdata == 0); 
    
    tick(dut); // Clock edge happens
    
    dut->reg_write_en = 0; // Turn off write
    dut->eval(); // Combinational read update
    assert(dut->rs1_rdata == 0x99999999);
    std::cout << "\033[32m[PASS] Read-After-Write (RAW) behavior verified\033[0m" << std::endl;

    // Test 8: Sweep all registers (Write and Read distinct values)
    for (int i = 1; i < 32; i++) {
        dut->reg_write_en = 1;
        dut->rd_addr = i;
        dut->rd_wdata = 0xABCDE000 + i;
        tick(dut);
    }
    
    dut->reg_write_en = 0;
    bool sweep_ok = true;
    for (int i = 1; i < 32; i++) {
        dut->rs1_addr = i;
        dut->eval();
        if (dut->rs1_rdata != (0xABCDE000 + i)) {
            sweep_ok = false;
        }
    }
    assert(sweep_ok);
    std::cout << "\033[32m[PASS] All 31 mutable registers write/read successfully without collision\033[0m" << std::endl;

    std::cout << "----------------------------------" << std::endl;
    std::cout << "\033[32mAll 8 tests passed!\033[0m" << std::endl;   
    std::cout << "----------------------------------" << std::endl;
    
    delete dut;
    exit(0);
}