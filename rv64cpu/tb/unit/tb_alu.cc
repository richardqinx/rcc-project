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

// File path: tb/unit/tb_alu.cc
// Description: ALU unit test
#include "Valu.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <verilated.h>

enum AluOp {
    ALU_ADD  = 0b0000,
    ALU_SUB  = 0b1000,
    ALU_SLL  = 0b0001,
    ALU_SLT  = 0b0010,
    ALU_SLTU = 0b0011,
    ALU_XOR  = 0b0100,
    ALU_SRL  = 0b0101,
    ALU_SRA  = 0b1101,
    ALU_OR   = 0b0110,
    ALU_AND  = 0b0111
};

void test_alu(Valu* alu, uint8_t op, uint64_t rs1, uint64_t rs2, uint64_t expected, const char* test_name) {
    alu->alu_op = op;
    alu->operand_a = rs1;
    alu->operand_b = rs2;

    alu->eval();
    
    if (alu->alu_result == expected) {
        std::cout << "\033[32m";
        std::cout << "[PASS] " << test_name << " | " 
                  << rs1 << " op " << rs2 << " = " << expected << std::endl;
        std::cout << "\033[0m";
    } else {
        std::cout << "\033[31m";
        std::cerr << "[FAIL] " << test_name << " | " 
                  << "Expected: " << expected << ", Got: " << alu->alu_result << std::endl;
        std::cout << "\033[0m";
        assert(false);
    }
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    Valu* alu = new Valu;
    
    std::cout << "Running ALU Unit Test ..." << std::endl;
    
    test_alu(alu, ALU_ADD, 100, 50, 150, "ADD_Positive");
    test_alu(alu, ALU_SUB, 100, 50, 50,  "SUB_Positive");

    test_alu(alu, ALU_ADD, (uint64_t)-5, 10, 5, "ADD_Negative");

    test_alu(alu, ALU_AND, 0xFF00, 0x0F0F, 0x0F00, "AND_Mask");
    test_alu(alu, ALU_XOR, 0xAAAA, 0xFFFF, 0x5555, "XOR_Invert");

    test_alu(alu, ALU_SLL, 1, 10, 1024, "SLL_By_10");

    uint64_t minus_one = (uint64_t)-1;
    uint64_t five = 5;
    
    test_alu(alu, ALU_SLTU, minus_one, five, 0, "SLTU (Unsigned: -1 < 5 is False)");
    test_alu(alu, ALU_SLT,  minus_one, five, 1, "SLT  (Signed: -1 < 5 is True)");

    std::cout << "All tests passed!" << std::endl;

    delete alu;
    return 0;
}