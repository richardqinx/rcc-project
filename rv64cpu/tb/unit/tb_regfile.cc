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
    
    std::cout << "Starting register file unit test" << std::endl;
    
    // Test 1: Reset
    dut->rst = 1;
    tick(dut);
    dut->rst = 0;
    std::cout << "[PASS] Reset complete" << std::endl;

    // Test 2: Write and read
    dut->reg_write_en = 1;
    dut->rd_addr = 5;
    dut->rd_wdata = 0xDEADBEEF;
    tick(dut);

    dut->reg_write_en = 0;
    dut->rs1_addr = 5;
    dut->eval();
    assert(dut->rs1_rdata == 0xDEADBEEF);
    std::cout << "[PASS] Write and read (x5) complete" << std::endl;

    // Test 3: x0
    dut->reg_write_en = 1;
    dut->rd_addr = 0;
    dut->rd_wdata = 0x12345678;
    tick(dut);
    
    dut->reg_write_en = 0;
    dut->rs2_addr = 0;
    dut->eval();
    assert(dut->rs2_rdata == 0);
    std::cout << "[PASS] Write and read (x0) complete" << std::endl;

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
    std::cout << "[PASS] Dual-port simultaneous read complete" << std::endl;

    std::cout << "Register file unit test complete" << std::endl;   
    
    delete dut;
    exit(0);
}